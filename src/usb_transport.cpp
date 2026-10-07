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

    // Start event processing thread
    m_running = true;
    m_event_thread = std::thread(&UsbTransport::EventThreadLoop, this);

    LOG_INFO("Connected to DisplayLink Device (PID 0x%04x)", m_connected_pid);
    return true;
}

void UsbTransport::CloseDevice() {
    m_running = false;
    if (m_event_thread.joinable()) {
        m_event_thread.join();
    }

    if (m_handle) {
        libusb_release_interface(m_handle, protocol::INTERFACE_VIDEO_CONTROL);
        libusb_close(m_handle);
        m_handle = nullptr;
        m_connected_pid = 0;
    }
}

void UsbTransport::EventThreadLoop() {
    timeval tv{0, 50000}; // 50ms timeout
    while (m_running) {
        libusb_handle_events_timeout_completed(m_ctx, &tv, nullptr);
    }
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

    std::lock_guard<std::mutex> lock(m_io_mutex);
    int transferred = 0;
    int rc = libusb_bulk_transfer(m_handle, endpoint, const_cast<uint8_t*>(data), static_cast<int>(length), &transferred, timeout_ms);
    if (rc != 0) {
        LOG_ERROR("Bulk video transfer failed on EP 0x%02x: %s", endpoint, libusb_error_name(rc));
        return false;
    }
    return true;
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

} // namespace dl_turbo
