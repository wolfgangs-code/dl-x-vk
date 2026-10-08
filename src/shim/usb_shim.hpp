#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <libusb-1.0/libusb.h>
#include <cstdint>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <string>

namespace dl_turbo {

struct EndpointMetrics {
    std::atomic<uint64_t> total_submits{0};
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> total_submit_cpu_ns{0};
    std::atomic<uint64_t> total_submit_wall_ns{0};
    std::atomic<int> min_size{0x7fffffff};
    std::atomic<int> max_size{0};

    // Window metrics
    std::atomic<uint64_t> window_submits{0};
    std::atomic<uint64_t> window_bytes{0};
    std::atomic<uint64_t> window_submit_cpu_ns{0};
};

class UsbShimManager {
public:
    static UsbShimManager& Instance();

    void EnsureRealLibUsb();
    void* GetRealSymbol(const char* name);

    int InterceptSubmitTransfer(struct libusb_transfer* transfer);
    int InterceptCancelTransfer(struct libusb_transfer* transfer);
    struct libusb_transfer* InterceptAllocTransfer(int iso_packets);
    void InterceptFreeTransfer(struct libusb_transfer* transfer);
    int InterceptBulkTransfer(struct libusb_device_handle* dev_handle, unsigned char endpoint,
                              unsigned char* data, int length, int* transferred, unsigned int timeout);
    int InterceptControlTransfer(struct libusb_device_handle* dev_handle, uint8_t request_type,
                                 uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                                 unsigned char* data, uint16_t wLength, unsigned int timeout);
    int InterceptHandleEventsTimeoutCompleted(libusb_context* ctx, struct timeval* tv, int* completed);

    void DumpPeriodicSummary();

private:
    UsbShimManager();
    ~UsbShimManager() = default;

    std::once_flag m_init_once;
    void* m_real_libusb = nullptr;

    std::atomic<int> m_allocated_transfers{0};
    std::atomic<uint64_t> m_total_submits{0};

    std::mutex m_ep_mutex;
    std::unordered_map<uint8_t, EndpointMetrics*> m_endpoints;
    EndpointMetrics* GetEndpointMetrics(uint8_t ep);

    struct timespec m_last_global_report{0, 0};
};

} // namespace dl_turbo
