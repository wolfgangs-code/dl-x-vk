#include "usb_transport.hpp"
#include <chrono>

namespace dl_turbo {

UsbTransport::UsbTransport()
    : m_ctx(nullptr),
      m_handle(nullptr),
      m_connected_pid(0),
      m_seq_counter(0),
      m_running(false) {}

UsbTransport::~UsbTransport() {
    CloseDevice();
    if (m_ctx) {
        libusb_exit(m_ctx);
        m_ctx = nullptr;
    }
}

bool UsbTransport::Initialize() {
    int rc = libusb_init(&m_ctx);
    if (rc < 0) {
        LOG_ERROR("Failed to initialize libusb: %s", libusb_error_name(rc));
        return false;
    }
    LOG_INFO("libusb-1.0 initialized successfully");
    return true;
}

std::vector<UsbDeviceDescriptor> UsbTransport::EnumerateDevices() {
    std::vector<UsbDeviceDescriptor> found;
    if (!m_ctx) return found;

    libusb_device** dev_list = nullptr;
    ssize_t count = libusb_get_device_list(m_ctx, &dev_list);
    if (count < 0) {
        LOG_ERROR("Failed to get USB device list: %s", libusb_error_name(count));
        return found;
    }

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* dev = dev_list[i];
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) == 0) {
            if (desc.idVendor == VENDOR_DISPLAYLINK) {
                UsbDeviceDescriptor info;
                info.vendor_id = desc.idVendor;
                info.product_id = desc.idProduct;
                info.bus_number = libusb_get_bus_number(dev);
                info.device_address = libusb_get_device_address(dev);

                libusb_device_handle* h = nullptr;
                if (libusb_open(dev, &h) == 0) {
                    unsigned char str_buf[256];
                    if (desc.iProduct > 0 && libusb_get_string_descriptor_ascii(h, desc.iProduct, str_buf, sizeof(str_buf)) > 0) {
                        info.product_name = reinterpret_cast<char*>(str_buf);
                    }
                    if (desc.iSerialNumber > 0 && libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, str_buf, sizeof(str_buf)) > 0) {
                        info.serial_number = reinterpret_cast<char*>(str_buf);
                    }
                    libusb_close(h);
                }

                found.push_back(info);
            }
        }
    }

    libusb_free_device_list(dev_list, 1);
    return found;
}

bool UsbTransport::OpenDevice(uint16_t vendor_id, uint16_t product_id) {
    if (!m_ctx) return false;
    CloseDevice();

    libusb_device** dev_list = nullptr;
    ssize_t count = libusb_get_device_list(m_ctx, &dev_list);
    if (count < 0) return false;

    libusb_device* target_dev = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* dev = dev_list[i];
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) == 0) {
            if (desc.idVendor == vendor_id) {
                if (product_id == 0 || desc.idProduct == product_id) {
                    target_dev = dev;
                    m_connected_pid = desc.idProduct;
                    break;
                }
            }
        }
    }

    if (!target_dev) {
        LOG_WARN("No matching DisplayLink USB device found (%04x:%04x)", vendor_id, product_id);
        libusb_free_device_list(dev_list, 1);
        return false;
    }

    int rc = libusb_open(target_dev, &m_handle);
    libusb_free_device_list(dev_list, 1);

    if (rc != 0 || !m_handle) {
        LOG_ERROR("Failed to open DisplayLink USB device: %s", libusb_error_name(rc));
        return false;
    }

    // Auto detach kernel drivers if attached
    libusb_set_auto_detach_kernel_driver(m_handle, 1);

    // Claim Video Control Interface (0)
    rc = libusb_claim_interface(m_handle, protocol::INTERFACE_VIDEO_CONTROL);
    if (rc != 0) {
        LOG_WARN("Could not claim interface 0 (busy or owned by existing driver): %s", libusb_error_name(rc));
    } else {
        LOG_INFO("Claimed DisplayLink Control Interface 0");
    }

    // Initialize Asynchronous Multi-URB Ring Queue
    {
        std::lock_guard<std::mutex> lock(m_ring_mutex);
        m_in_flight_count.store(0);
        m_next_urb_index = 0;
        for (size_t i = 0; i < NUM_ASYNC_URBS; ++i) {
            m_urbs[i].transfer = libusb_alloc_transfer(0);
            if (!m_urbs[i].transfer) {
                LOG_ERROR("Failed to allocate libusb transfer for URB %zu", i);
            }
            m_urbs[i].buffer.resize(4 * 1024 * 1024); // 4MB per URB buffer
            m_urbs[i].in_flight.store(false);
            m_urbs[i].transport = this;
            m_urbs[i].urb_id = static_cast<uint32_t>(i);
        }
    }

    // Start event processing thread
    m_running = true;
    m_event_thread = std::thread(&UsbTransport::EventThreadLoop, this);

    LOG_INFO("Connected to DisplayLink Device (PID 0x%04x) with %zu async URBs", m_connected_pid, NUM_ASYNC_URBS);
    return true;
}

void UsbTransport::CloseDevice() {
    // 1. Cancel all in-flight URBs
    {
        std::lock_guard<std::mutex> lock(m_ring_mutex);
        for (auto& urb : m_urbs) {
            if (urb.in_flight.load(std::memory_order_acquire) && urb.transfer) {
                libusb_cancel_transfer(urb.transfer);
            }
        }
    }

    // 2. Wait up to 300ms for in-flight cancellations to complete while event thread is running
    {
        std::unique_lock<std::mutex> lock(m_ring_mutex);
        m_ring_cv.wait_for(lock, std::chrono::milliseconds(300), [this]() {
            return m_in_flight_count.load(std::memory_order_acquire) == 0;
        });
    }

    // 3. Stop event processing thread
    m_running = false;
    if (m_event_thread.joinable()) {
        m_event_thread.join();
    }

    // 4. Free all transfer descriptors
    {
        std::lock_guard<std::mutex> lock(m_ring_mutex);
        for (auto& urb : m_urbs) {
            if (urb.transfer) {
                libusb_free_transfer(urb.transfer);
                urb.transfer = nullptr;
            }
            urb.in_flight.store(false);
        }
        m_in_flight_count.store(0);
    }

    // 5. Release interface and close handle
    if (m_handle) {
        libusb_release_interface(m_handle, protocol::INTERFACE_VIDEO_CONTROL);
        libusb_close(m_handle);
        m_handle = nullptr;
        m_connected_pid = 0;
    }
}

void UsbTransport::EventThreadLoop() {
    timeval tv{0, 20000}; // 20ms timeout
    while (m_running) {
        libusb_handle_events_timeout_completed(m_ctx, &tv, nullptr);
    }
}

void LIBUSB_CALL UsbTransport::AsyncTransferCallback(libusb_transfer* transfer) {
    AsyncUrb* urb = static_cast<AsyncUrb*>(transfer->user_data);
    if (!urb || !urb->transport) return;
    urb->transport->OnTransferCompleted(urb, transfer->status);
}

void UsbTransport::OnTransferCompleted(AsyncUrb* urb, int status) {
    if (status != LIBUSB_TRANSFER_COMPLETED && status != LIBUSB_TRANSFER_CANCELLED) {
        LOG_WARN("Async USB URB %u completed with status %d: %s",
                 urb->urb_id, status, libusb_error_name(status));
    }
    if (urb->completion_flag) {
        urb->completion_flag->store(false, std::memory_order_release);
        urb->completion_flag = nullptr;
    }
    urb->is_zero_copy = false;
    urb->in_flight.store(false, std::memory_order_release);
    m_in_flight_count.fetch_sub(1, std::memory_order_acq_rel);
    m_ring_cv.notify_all();
}

bool UsbTransport::SendCommand(protocol::CommandOpcode opcode, uint8_t head_id, const void* payload, size_t payload_len) {
    if (!m_handle) return false;

    std::lock_guard<std::mutex> lock(m_io_mutex);

    protocol::CommandPacketHeader hdr;
    hdr.sync_word      = protocol::DL_SYNC_WORD;
    hdr.head_id        = head_id;
    hdr.opcode         = static_cast<uint8_t>(opcode);
    hdr.sequence_id    = m_seq_counter++;
    hdr.payload_length = static_cast<uint16_t>(payload_len);

    std::vector<uint8_t> packet(sizeof(hdr) + payload_len);
    std::memcpy(packet.data(), &hdr, sizeof(hdr));
    if (payload && payload_len > 0) {
        std::memcpy(packet.data() + sizeof(hdr), payload, payload_len);
    }

    int transferred = 0;
    int rc = libusb_bulk_transfer(m_handle, protocol::EP_CMD_OUT, packet.data(), packet.size(), &transferred, 1000);
    if (rc != 0) {
        LOG_ERROR("Failed to send command 0x%02x: %s", static_cast<uint8_t>(opcode), libusb_error_name(rc));
        return false;
    }
    return true;
}

bool UsbTransport::SendVideoData(uint8_t endpoint, const uint8_t* data, size_t length, unsigned int timeout_ms) {
    if (!m_handle || !data || length == 0) return false;

    AsyncUrb* target_urb = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_ring_mutex);

        // Wait until at least one URB slot is available in the ring
        bool available = m_ring_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
            for (const auto& urb : m_urbs) {
                if (!urb.in_flight.load(std::memory_order_acquire)) return true;
            }
            return false;
        });

        if (!available) {
            LOG_WARN("All %zu async USB URBs in flight (pipeline backpressure timeout)", NUM_ASYNC_URBS);
            return false;
        }

        // Round-robin selection of the next available URB
        for (size_t i = 0; i < NUM_ASYNC_URBS; ++i) {
            size_t idx = (m_next_urb_index + i) % NUM_ASYNC_URBS;
            if (!m_urbs[idx].in_flight.load(std::memory_order_acquire) && m_urbs[idx].transfer) {
                target_urb = &m_urbs[idx];
                m_next_urb_index = (idx + 1) % NUM_ASYNC_URBS;
                break;
            }
        }

        if (!target_urb || !target_urb->transfer) {
            // Fallback to synchronous transfer if ring URBs are not allocated
            return SendVideoDataSync(endpoint, data, length, timeout_ms);
        }

        // Resize buffer if payload exceeds current capacity
        if (target_urb->buffer.size() < length) {
            target_urb->buffer.resize(length);
        }

        // Fast copy into URB transmission buffer
        std::memcpy(target_urb->buffer.data(), data, length);
        target_urb->in_flight.store(true, std::memory_order_release);
        m_in_flight_count.fetch_add(1, std::memory_order_acq_rel);
    }

    // Fill and submit asynchronous bulk transfer
    libusb_fill_bulk_transfer(
        target_urb->transfer,
        m_handle,
        endpoint,
        target_urb->buffer.data(),
        static_cast<int>(length),
        AsyncTransferCallback,
        target_urb,
        timeout_ms
    );

    int rc = libusb_submit_transfer(target_urb->transfer);
    if (rc != 0) {
        LOG_ERROR("Failed to submit async USB URB %u: %s", target_urb->urb_id, libusb_error_name(rc));
        target_urb->in_flight.store(false, std::memory_order_release);
        m_in_flight_count.fetch_sub(1, std::memory_order_acq_rel);
        m_ring_cv.notify_all();
        return false;
    }

    return true;
}

bool UsbTransport::SendVideoDataZeroCopy(
    uint8_t endpoint,
    const uint8_t* data,
    size_t length,
    std::atomic<bool>* completion_flag,
    unsigned int timeout_ms
) {
    if (!m_handle || !data || length == 0) return false;

    AsyncUrb* target_urb = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_ring_mutex);

        // Wait until at least one URB slot is available in the ring
        bool available = m_ring_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
            for (const auto& urb : m_urbs) {
                if (!urb.in_flight.load(std::memory_order_acquire)) return true;
            }
            return false;
        });

        if (!available) {
            LOG_WARN("All %zu async USB URBs in flight (pipeline backpressure timeout)", NUM_ASYNC_URBS);
            return false;
        }

        // Round-robin selection of the next available URB
        for (size_t i = 0; i < NUM_ASYNC_URBS; ++i) {
            size_t idx = (m_next_urb_index + i) % NUM_ASYNC_URBS;
            if (!m_urbs[idx].in_flight.load(std::memory_order_acquire) && m_urbs[idx].transfer) {
                target_urb = &m_urbs[idx];
                m_next_urb_index = (idx + 1) % NUM_ASYNC_URBS;
                break;
            }
        }

        if (!target_urb || !target_urb->transfer) {
            return SendVideoDataSync(endpoint, data, length, timeout_ms);
        }

        target_urb->is_zero_copy = true;
        target_urb->completion_flag = completion_flag;
        if (completion_flag) {
            completion_flag->store(true, std::memory_order_release);
        }
        target_urb->in_flight.store(true, std::memory_order_release);
        m_in_flight_count.fetch_add(1, std::memory_order_acq_rel);
    }

    // Zero-Copy direct memory DMA fill
    libusb_fill_bulk_transfer(
        target_urb->transfer,
        m_handle,
        endpoint,
        const_cast<uint8_t*>(data),
        static_cast<int>(length),
        AsyncTransferCallback,
        target_urb,
        timeout_ms
    );

    int rc = libusb_submit_transfer(target_urb->transfer);
    if (rc != 0) {
        LOG_ERROR("Failed to submit zero-copy async USB URB %u: %s", target_urb->urb_id, libusb_error_name(rc));
        if (target_urb->completion_flag) {
            target_urb->completion_flag->store(false, std::memory_order_release);
            target_urb->completion_flag = nullptr;
        }
        target_urb->is_zero_copy = false;
        target_urb->in_flight.store(false, std::memory_order_release);
        m_in_flight_count.fetch_sub(1, std::memory_order_acq_rel);
        m_ring_cv.notify_all();
        return false;
    }

    return true;
}

bool UsbTransport::SendVideoDataSync(uint8_t endpoint, const uint8_t* data, size_t length, unsigned int timeout_ms) {
    if (!m_handle || !data || length == 0) return false;

    std::lock_guard<std::mutex> lock(m_io_mutex);
    int transferred = 0;
    int rc = libusb_bulk_transfer(m_handle, endpoint, const_cast<uint8_t*>(data), static_cast<int>(length), &transferred, timeout_ms);
    if (rc != 0) {
        LOG_ERROR("Bulk video transfer failed on EP 0x%02x: %s", endpoint, libusb_error_name(rc));
        return false;
    }
    return true;
}

bool UsbTransport::FlushVideoTransfers(unsigned int timeout_ms) {
    std::unique_lock<std::mutex> lock(m_ring_mutex);
    return m_ring_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
        return m_in_flight_count.load(std::memory_order_acquire) == 0;
    });
}

bool UsbTransport::SendHeartbeat(uint8_t head_id) {
    protocol::KeepAlivePacket pkt;
    pkt.sync_word  = protocol::DL_SYNC_WORD;
    pkt.head_id    = head_id;
    pkt.opcode     = static_cast<uint8_t>(protocol::CommandOpcode::Heartbeat);
    pkt.uptime_ms  = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count());

    return SendCommand(protocol::CommandOpcode::Heartbeat, head_id, &pkt, sizeof(pkt));
}

double UsbTransport::BenchmarkRingDispatch(const uint8_t* data, size_t length, int iterations, bool zero_copy) {
    if (!data || length == 0 || iterations <= 0) return 0.0;

    std::lock_guard<std::mutex> lock(m_ring_mutex);
    if (!zero_copy) {
        for (size_t i = 0; i < NUM_ASYNC_URBS; ++i) {
            if (m_urbs[i].buffer.size() < length) {
                m_urbs[i].buffer.resize(length);
            }
        }
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (int it = 0; it < iterations; ++it) {
        size_t idx = it % NUM_ASYNC_URBS;
        if (!zero_copy) {
            std::memcpy(m_urbs[idx].buffer.data(), data, length);
        }
        m_urbs[idx].in_flight.store(true, std::memory_order_release);
        m_urbs[idx].in_flight.store(false, std::memory_order_release);
    }
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::micro>(end - start).count() / iterations;
}

} // namespace dl_turbo
