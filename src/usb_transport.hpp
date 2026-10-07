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

    // Send bulk video frame slice data over video endpoint (EP 8 or EP 10)
    bool SendVideoData(uint8_t endpoint, const uint8_t* data, size_t length, unsigned int timeout_ms = 1000);

    // Send keepalive heartbeat
    bool SendHeartbeat(uint8_t head_id = 0);

    bool IsConnected() const { return m_handle != nullptr; }
    uint16_t GetConnectedPid() const { return m_connected_pid; }

private:
    void EventThreadLoop();

    libusb_context* m_ctx;
    libusb_device_handle* m_handle;
    uint16_t m_connected_pid;
    uint16_t m_seq_counter;

    std::atomic<bool> m_running;
    std::thread m_event_thread;
    std::mutex m_io_mutex;
};

} // namespace dl_turbo
