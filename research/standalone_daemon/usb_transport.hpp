#pragma once

#include <libusb-1.0/libusb.h>
#include <cstdint>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "common.hpp"
#include "protocol.hpp"

namespace dl_turbo {

struct UsbDeviceDescriptor {
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  bus_number;
    uint8_t  device_address;
    std::string serial_number;
    std::string product_name;
};

class UsbTransport {
public:
    static constexpr size_t NUM_ASYNC_URBS = 4;

    UsbTransport();
    ~UsbTransport();

    // Initialize libusb context
    bool Initialize();

    // Scan bus for connected DisplayLink devices
    std::vector<UsbDeviceDescriptor> EnumerateDevices();

    // Open connection to a specific DisplayLink device
    bool OpenDevice(uint16_t vendor_id = VENDOR_DISPLAYLINK, uint16_t product_id = 0);

    // Close device connection
    void CloseDevice();

    // Send a command packet over EP 2 OUT
    bool SendCommand(protocol::CommandOpcode opcode, uint8_t head_id, const void* payload = nullptr, size_t payload_len = 0);

    // Send bulk video frame slice data over video endpoint (EP 8 or EP 10) asynchronously via Multi-URB ring queue
    bool SendVideoData(uint8_t endpoint, const uint8_t* data, size_t length, unsigned int timeout_ms = 1000);

    // Send bulk video frame slice data over video endpoint asynchronously (Zero-Copy direct pointer)
    bool SendVideoDataZeroCopy(
        uint8_t endpoint,
        const uint8_t* data,
        size_t length,
        std::atomic<bool>* completion_flag = nullptr,
        unsigned int timeout_ms = 1000
    );

    // Synchronous fallback for sending video data
    bool SendVideoDataSync(uint8_t endpoint, const uint8_t* data, size_t length, unsigned int timeout_ms = 1000);

    // Flush any in-flight asynchronous USB video transfers
    bool FlushVideoTransfers(unsigned int timeout_ms = 1000);

    // Benchmark ring buffer dispatch latency and memory queuing overhead
    double BenchmarkRingDispatch(const uint8_t* data, size_t length, int iterations = 100, bool zero_copy = false);

    // Send keepalive heartbeat
    bool SendHeartbeat(uint8_t head_id = 0);

    bool IsConnected() const { return m_handle != nullptr; }
    uint16_t GetConnectedPid() const { return m_connected_pid; }
    size_t GetInFlightUrbsCount() const { return m_in_flight_count.load(std::memory_order_relaxed); }

private:
    struct AsyncUrb {
        libusb_transfer* transfer{nullptr};
        std::vector<uint8_t> buffer;
        std::atomic<bool> in_flight{false};
        std::atomic<bool>* completion_flag{nullptr};
        UsbTransport* transport{nullptr};
        uint32_t urb_id{0};
        bool is_zero_copy{false};
    };

    void EventThreadLoop();
    static void LIBUSB_CALL AsyncTransferCallback(libusb_transfer* transfer);
    void OnTransferCompleted(AsyncUrb* urb, int status);

    libusb_context* m_ctx;
    libusb_device_handle* m_handle;
    uint16_t m_connected_pid;
    uint16_t m_seq_counter;

    std::atomic<bool> m_running;
    std::thread m_event_thread;
    std::mutex m_io_mutex;

    // Asynchronous Multi-URB Ring Queue
    std::array<AsyncUrb, NUM_ASYNC_URBS> m_urbs;
    std::mutex m_ring_mutex;
    std::condition_variable m_ring_cv;
    std::atomic<size_t> m_in_flight_count{0};
    size_t m_next_urb_index{0};
};

} // namespace dl_turbo
