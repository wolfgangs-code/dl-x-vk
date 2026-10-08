#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "usb_shim.hpp"
#include "common.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <vector>
#include <algorithm>

namespace dl_turbo {

struct ThreadSubmitContext {
    pid_t tid = 0;
    char name[16] = {0};
    uint64_t submits = 0;
    uint64_t bytes = 0;
    uint64_t submit_cpu_ns = 0;
    struct timespec wall_start{0, 0};
    struct timespec cpu_start{0, 0};
    bool initialized = false;
};

UsbShimManager& UsbShimManager::Instance() {
    static UsbShimManager s_instance;
    return s_instance;
}

UsbShimManager::UsbShimManager() {
    clock_gettime(CLOCK_MONOTONIC, &m_last_global_report);
    EnsureRealLibUsb();
    LOG_INFO("[USB-TURBO-SHIM] DisplayLink Turbo USB Interception Layer initialized");
}

void UsbShimManager::EnsureRealLibUsb() {
    std::call_once(m_init_once, [this]() {
        const char* paths[] = {
            "/usr/lib64/libusb-1.0.so.0",
            "/usr/lib/libusb-1.0.so.0",
            "/usr/lib64/libusb-1.0.so",
            "/usr/lib/libusb-1.0.so",
            "libusb-1.0.so.0"
        };
        for (const char* path : paths) {
            m_real_libusb = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (m_real_libusb) {
                LOG_INFO("[USB-TURBO-SHIM] Loaded system libusb from %s", path);
                break;
            }
        }
        if (!m_real_libusb) {
            LOG_ERROR("[USB-TURBO-SHIM] Failed to dlopen system libusb: %s", dlerror());
        }
    });
}

void* UsbShimManager::GetRealSymbol(const char* name) {
    EnsureRealLibUsb();
    if (m_real_libusb) {
        return dlsym(m_real_libusb, name);
    }
    return dlsym(RTLD_NEXT, name);
}

EndpointMetrics* UsbShimManager::GetEndpointMetrics(uint8_t ep) {
    std::lock_guard<std::mutex> lock(m_ep_mutex);
    auto it = m_endpoints.find(ep);
    if (it != m_endpoints.end()) {
        return it->second;
    }
    auto* m = new EndpointMetrics();
    m_endpoints[ep] = m;
    return m;
}

int UsbShimManager::InterceptSubmitTransfer(struct libusb_transfer* transfer) {
    static auto real_fn = reinterpret_cast<int (*)(struct libusb_transfer*)>(
        GetRealSymbol("libusb_submit_transfer"));
    if (!real_fn) return LIBUSB_ERROR_NOT_FOUND;

    if (!transfer) return real_fn(transfer);

    uint8_t ep = transfer->endpoint;
    int len = transfer->length;

    // Per-thread profiling
    thread_local ThreadSubmitContext tl_ctx;
    struct timespec now_wall, now_cpu;
    clock_gettime(CLOCK_MONOTONIC, &now_wall);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now_cpu);

    if (!tl_ctx.initialized) {
        tl_ctx.wall_start = now_wall;
        tl_ctx.cpu_start = now_cpu;
        tl_ctx.tid = static_cast<pid_t>(syscall(SYS_gettid));
        pthread_getname_np(pthread_self(), tl_ctx.name, sizeof(tl_ctx.name));
        tl_ctx.initialized = true;
    }

    // Measure exact execution time spent inside real libusb_submit_transfer
    struct timespec t_sub_start, t_sub_end;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t_sub_start);

    int rc = real_fn(transfer);

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t_sub_end);

    int64_t sub_cpu_ns = (t_sub_end.tv_sec - t_sub_start.tv_sec) * 1000000000LL +
                         (t_sub_end.tv_nsec - t_sub_start.tv_nsec);
    if (sub_cpu_ns < 0) sub_cpu_ns = 0;

    // Update endpoint stats
    EndpointMetrics* ep_m = GetEndpointMetrics(ep);
    if (ep_m) {
        ep_m->total_submits.fetch_add(1, std::memory_order_relaxed);
        ep_m->total_bytes.fetch_add(len, std::memory_order_relaxed);
        ep_m->total_submit_cpu_ns.fetch_add(sub_cpu_ns, std::memory_order_relaxed);

        ep_m->window_submits.fetch_add(1, std::memory_order_relaxed);
        ep_m->window_bytes.fetch_add(len, std::memory_order_relaxed);
        ep_m->window_submit_cpu_ns.fetch_add(sub_cpu_ns, std::memory_order_relaxed);

        int cur_min = ep_m->min_size.load(std::memory_order_relaxed);
        if (len < cur_min) ep_m->min_size.store(len, std::memory_order_relaxed);
        int cur_max = ep_m->max_size.load(std::memory_order_relaxed);
        if (len > cur_max) ep_m->max_size.store(len, std::memory_order_relaxed);
    }

    tl_ctx.submits++;
    tl_ctx.bytes += len;
    tl_ctx.submit_cpu_ns += sub_cpu_ns;

    // Report per-thread stats every 1.0 second of wall time
    int64_t elapsed_wall_ns = (now_wall.tv_sec - tl_ctx.wall_start.tv_sec) * 1000000000LL +
                              (now_wall.tv_nsec - tl_ctx.wall_start.tv_nsec);
    if (elapsed_wall_ns >= 1000000000LL) {
        int64_t thread_cpu_ns = (t_sub_end.tv_sec - tl_ctx.cpu_start.tv_sec) * 1000000000LL +
                                (t_sub_end.tv_nsec - tl_ctx.cpu_start.tv_nsec);
        if (thread_cpu_ns < 0) thread_cpu_ns = 0;

        double wall_sec = static_cast<double>(elapsed_wall_ns) / 1e9;
        double thread_cpu_pct = (static_cast<double>(thread_cpu_ns) / static_cast<double>(elapsed_wall_ns)) * 100.0;
        double submit_cpu_pct = (static_cast<double>(tl_ctx.submit_cpu_ns) / static_cast<double>(elapsed_wall_ns)) * 100.0;
        double submit_ratio   = (thread_cpu_ns > 0) ?
            (static_cast<double>(tl_ctx.submit_cpu_ns) / static_cast<double>(thread_cpu_ns) * 100.0) : 0.0;
        double mb_sec         = (static_cast<double>(tl_ctx.bytes) / (1024.0 * 1024.0)) / wall_sec;
        double xfers_sec      = static_cast<double>(tl_ctx.submits) / wall_sec;

        // Refresh thread name in case it changed dynamically
        pthread_getname_np(pthread_self(), tl_ctx.name, sizeof(tl_ctx.name));

        LOG_INFO("[USB-PROFILER] TID %d ('%s') | Thread CPU: %5.1f%% | Inside submit: %5.2f%% (%4.1f%% of thread) | %5.0f xfers/s (%5.2f MB/s) | EP 0x%02x, len %d",
                 tl_ctx.tid, tl_ctx.name,
                 thread_cpu_pct, submit_cpu_pct, submit_ratio,
                 xfers_sec, mb_sec, ep, len);

        tl_ctx.wall_start = now_wall;
        tl_ctx.cpu_start = t_sub_end;
        tl_ctx.submits = 0;
        tl_ctx.bytes = 0;
        tl_ctx.submit_cpu_ns = 0;
    }

    // Periodic summary check
    uint64_t gs = m_total_submits.fetch_add(1, std::memory_order_relaxed);
    if (gs > 0 && (gs % 2500 == 0)) {
        DumpPeriodicSummary();
    }

    return rc;
}

int UsbShimManager::InterceptCancelTransfer(struct libusb_transfer* transfer) {
    static auto real_fn = reinterpret_cast<int (*)(struct libusb_transfer*)>(
        GetRealSymbol("libusb_cancel_transfer"));
    if (!real_fn) return LIBUSB_ERROR_NOT_FOUND;
    return real_fn(transfer);
}

struct libusb_transfer* UsbShimManager::InterceptAllocTransfer(int iso_packets) {
    static auto real_fn = reinterpret_cast<struct libusb_transfer* (*)(int)>(
        GetRealSymbol("libusb_alloc_transfer"));
    if (!real_fn) return nullptr;
    auto* xfer = real_fn(iso_packets);
    if (xfer) {
        m_allocated_transfers.fetch_add(1, std::memory_order_relaxed);
    }
    return xfer;
}

void UsbShimManager::InterceptFreeTransfer(struct libusb_transfer* transfer) {
    static auto real_fn = reinterpret_cast<void (*)(struct libusb_transfer*)>(
        GetRealSymbol("libusb_free_transfer"));
    if (transfer) {
        m_allocated_transfers.fetch_sub(1, std::memory_order_relaxed);
    }
    if (real_fn) real_fn(transfer);
}

int UsbShimManager::InterceptBulkTransfer(struct libusb_device_handle* dev_handle, unsigned char endpoint,
                                         unsigned char* data, int length, int* transferred, unsigned int timeout) {
    static auto real_fn = reinterpret_cast<int (*)(struct libusb_device_handle*, unsigned char, unsigned char*, int, int*, unsigned int)>(
        GetRealSymbol("libusb_bulk_transfer"));
    if (!real_fn) return LIBUSB_ERROR_NOT_FOUND;
    LOG_INFO("[USB-TURBO-SHIM] libusb_bulk_transfer: ep=0x%02x, len=%d, timeout=%u", endpoint, length, timeout);
    return real_fn(dev_handle, endpoint, data, length, transferred, timeout);
}

int UsbShimManager::InterceptControlTransfer(struct libusb_device_handle* dev_handle, uint8_t request_type,
                                            uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                                            unsigned char* data, uint16_t wLength, unsigned int timeout) {
    static auto real_fn = reinterpret_cast<int (*)(struct libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, unsigned int)>(
        GetRealSymbol("libusb_control_transfer"));
    if (!real_fn) return LIBUSB_ERROR_NOT_FOUND;
    return real_fn(dev_handle, request_type, bRequest, wValue, wIndex, data, wLength, timeout);
}

int UsbShimManager::InterceptHandleEventsTimeoutCompleted(libusb_context* ctx, struct timeval* tv, int* completed) {
    static auto real_fn = reinterpret_cast<int (*)(libusb_context*, struct timeval*, int*)>(
        GetRealSymbol("libusb_handle_events_timeout_completed"));
    if (!real_fn) return LIBUSB_ERROR_NOT_FOUND;
    return real_fn(ctx, tv, completed);
}

void UsbShimManager::DumpPeriodicSummary() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t elapsed_ns = (now.tv_sec - m_last_global_report.tv_sec) * 1000000000LL +
                         (now.tv_nsec - m_last_global_report.tv_nsec);
    if (elapsed_ns < 1500000000LL) return; // at least 1.5s interval
    m_last_global_report = now;
    double sec = static_cast<double>(elapsed_ns) / 1e9;

    std::lock_guard<std::mutex> lock(m_ep_mutex);
    LOG_INFO("[USB-SUMMARY] Active Pool: %d xfers allocated | Bus Traffic Breakdown:",
             m_allocated_transfers.load(std::memory_order_relaxed));

    for (auto& pair : m_endpoints) {
        uint8_t ep = pair.first;
        EndpointMetrics* m = pair.second;
        uint64_t w_submits = m->window_submits.exchange(0, std::memory_order_relaxed);
        uint64_t w_bytes   = m->window_bytes.exchange(0, std::memory_order_relaxed);
        uint64_t w_cpu_ns  = m->window_submit_cpu_ns.exchange(0, std::memory_order_relaxed);

        if (w_submits == 0) continue;

        double rate_xfers = static_cast<double>(w_submits) / sec;
        double mb_s = (static_cast<double>(w_bytes) / (1024.0 * 1024.0)) / sec;
        double avg_b = static_cast<double>(w_bytes) / static_cast<double>(w_submits);
        double submit_cpu_ms = (static_cast<double>(w_cpu_ns) / 1e6) / sec;

        const char* desc = "Unknown";
        if (ep == 0x02) desc = "Control/Cmd OUT";
        else if (ep == 0x84) desc = "Status/EDID IN";
        else if (ep == 0x08) desc = "Head 0 Video OUT";
        else if (ep == 0x0a) desc = "Head 1 Video OUT";
        else if (ep == 0x0b || ep == 0x0c) desc = "Aux Video OUT";
        else if (ep == 0x09) desc = "Audio OUT";
        else if (ep == 0x81) desc = "Audio IN";

        LOG_INFO("   EP 0x%02x (%-16s): %5.0f xfers/s | %6.2f MB/s | avg=%5.0f B (min=%d, max=%d) | submit_cpu=%4.2f ms/s",
                 ep, desc, rate_xfers, mb_s, avg_b,
                 m->min_size.load(std::memory_order_relaxed),
                 m->max_size.load(std::memory_order_relaxed),
                 submit_cpu_ms);
    }
}

} // namespace dl_turbo

// =========================================================================
// Exported C API Proxy Implementation for libusb
// =========================================================================
extern "C" {

int libusb_submit_transfer(struct libusb_transfer *transfer) {
    return dl_turbo::UsbShimManager::Instance().InterceptSubmitTransfer(transfer);
}

int libusb_cancel_transfer(struct libusb_transfer *transfer) {
    return dl_turbo::UsbShimManager::Instance().InterceptCancelTransfer(transfer);
}

struct libusb_transfer *libusb_alloc_transfer(int iso_packets) {
    return dl_turbo::UsbShimManager::Instance().InterceptAllocTransfer(iso_packets);
}

void libusb_free_transfer(struct libusb_transfer *transfer) {
    dl_turbo::UsbShimManager::Instance().InterceptFreeTransfer(transfer);
}

int libusb_bulk_transfer(struct libusb_device_handle *dev_handle,
                         unsigned char endpoint, unsigned char *data, int length,
                         int *transferred, unsigned int timeout) {
    return dl_turbo::UsbShimManager::Instance().InterceptBulkTransfer(
        dev_handle, endpoint, data, length, transferred, timeout);
}

int libusb_control_transfer(struct libusb_device_handle *dev_handle,
                            uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                            unsigned char *data, uint16_t wLength, unsigned int timeout) {
    return dl_turbo::UsbShimManager::Instance().InterceptControlTransfer(
        dev_handle, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeout);
}

int libusb_handle_events_timeout_completed(libusb_context *ctx,
                                           struct timeval *tv, int *completed) {
    return dl_turbo::UsbShimManager::Instance().InterceptHandleEventsTimeoutCompleted(ctx, tv, completed);
}

} // extern "C"
