#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <evdi_lib.h>
#include <dlfcn.h>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <iostream>
#include <algorithm>

#include "vulkan_converter.hpp"
#include "tile_engine.hpp"
#include "common.hpp"

namespace dl_turbo {

class ShimManager {
public:
    static ShimManager& Instance() {
        static ShimManager s_instance;
        return s_instance;
    }

    void EnsureRealEvdi() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_real_libevdi) return;

        static auto real_dlopen_fn = reinterpret_cast<void* (*)(const char*, int)>(
            dlsym(RTLD_NEXT, "dlopen"));

        const char* candidates[] = {
            "/usr/lib64/libevdi.so.1.15.1",
            "/usr/lib/libevdi.so.1.15.1",
            "/usr/lib64/libevdi.so.1",
            "/usr/lib/libevdi.so.1",
            "/usr/lib64/libevdi.so",
            "/usr/lib/libevdi.so"
        };

        for (const char* path : candidates) {
            if (real_dlopen_fn) {
                m_real_libevdi = real_dlopen_fn(path, RTLD_NOW | RTLD_LOCAL);
            }
            if (m_real_libevdi) {
                LOG_INFO("[EVDI-TURBO-SHIM] Loaded system libevdi from: %s", path);
                break;
            }
        }

        if (!m_real_libevdi) {
            LOG_ERROR("[EVDI-TURBO-SHIM] Failed to open system libevdi.so: %s", dlerror());
        }
    }

    void* GetRealSymbol(const char* name) {
        EnsureRealEvdi();
        if (!m_real_libevdi) return nullptr;
        return dlsym(m_real_libevdi, name);
    }

    void OnOpen(evdi_handle handle, int device) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_handle_to_device[handle] = device;
        LOG_INFO("[EVDI-TURBO-SHIM] evdi_open: handle=%p, device=%d", handle, device);
    }

    void OnClose(evdi_handle handle) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_buffers.erase(handle);
        m_active_buffer.erase(handle);
        m_handle_to_device.erase(handle);
        LOG_INFO("[EVDI-TURBO-SHIM] evdi_close: handle=%p", handle);
    }

    void OnRegisterBuffer(evdi_handle handle, struct evdi_buffer buffer) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_buffers[handle][buffer.id] = buffer;
        m_warmup_frames[handle] = 6;
        LOG_INFO("[EVDI-TURBO-SHIM] evdi_register_buffer: handle=%p, id=%d, %dx%d, stride=%d, ptr=%p",
                 handle, buffer.id, buffer.width, buffer.height, buffer.stride, buffer.buffer);
    }

    void OnUnregisterBuffer(evdi_handle handle, int bufferId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_buffers[handle].erase(bufferId);
        LOG_INFO("[EVDI-TURBO-SHIM] evdi_unregister_buffer: handle=%p, id=%d", handle, bufferId);
    }

    void OnRequestUpdate(evdi_handle handle, int bufferId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active_buffer[handle] = bufferId;
    }

    void FilterDirtyPixels(evdi_handle handle, struct evdi_rect* rects, int* num_rects) {
        static bool s_passthrough = (getenv("EVDI_TURBO_PASSTHROUGH") != nullptr);
        if (s_passthrough) return;

        m_grab_count.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(m_mutex);

        // Warmup: Pass first 6 frames through cleanly to allow display initialization & sync
        auto warm_it = m_warmup_frames.find(handle);
        if (warm_it != m_warmup_frames.end() && warm_it->second > 0) {
            warm_it->second--;
            m_passthrough_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        auto buf_it = m_buffers.find(handle);
        if (buf_it == m_buffers.end() || buf_it->second.empty()) return;

        int active_id = 0;
        auto act_it = m_active_buffer.find(handle);
        if (act_it != m_active_buffer.end()) {
            active_id = act_it->second;
        }

        auto it = buf_it->second.find(active_id);
        if (it == buf_it->second.end()) {
            it = buf_it->second.begin();
        }
        if (it == buf_it->second.end()) return;

        const auto& buf = it->second;
        if (!buf.buffer || buf.width <= 0 || buf.height <= 0 || buf.stride <= 0) return;

        // Calculate bounding box and total damaged area from EVDI
        int in_min_x = rects[0].x1, in_min_y = rects[0].y1;
        int in_max_x = rects[0].x2, in_max_y = rects[0].y2;
        int64_t total_damage_area = 0;

        for (int i = 0; i < *num_rects; ++i) {
            in_min_x = std::min(in_min_x, rects[i].x1);
            in_min_y = std::min(in_min_y, rects[i].y1);
            in_max_x = std::max(in_max_x, rects[i].x2);
            in_max_y = std::max(in_max_y, rects[i].y2);
            total_damage_area += static_cast<int64_t>(std::max(0, rects[i].x2 - rects[i].x1)) *
                                 std::max(0, rects[i].y2 - rects[i].y1);
        }

        in_min_x = std::max(0, in_min_x);
        in_min_y = std::max(0, in_min_y);
        in_max_x = std::min(buf.width, in_max_x);
        in_max_y = std::min(buf.height, in_max_y);

        if (in_max_x <= in_min_x || in_max_y <= in_min_y) {
            *num_rects = 0;
            m_suppressed_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        int64_t screen_area = static_cast<int64_t>(buf.width) * buf.height;

        // FAST-PATH: If damage is already small (< 25% of screen),
        // pass through directly with ZERO CPU copy and ZERO GPU dispatch!
        // DisplayLinkManager's internal SSE diffing loop handles small areas in < 0.05ms.
        if (total_damage_area < (screen_area / 4)) {
            m_passthrough_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        int head_id = 0;
        auto dev_it = m_handle_to_device.find(handle);
        if (dev_it != m_handle_to_device.end()) {
            head_id = (dev_it->second == 0) ? 0 : 1;
        }

        auto& vk = VulkanConverter::Instance();
        if (!vk.IsAvailable()) return;

        // Run Vulkan SPIR-V GPU differencing with sub-region clipping!
        const int TILE_SIZE = 32;
        std::vector<TileCoordinate> dirty_tiles;
        bool ok = vk.DetectDirtyTiles(
            reinterpret_cast<const uint8_t*>(buf.buffer),
            buf.stride,
            buf.width,
            buf.height,
            TILE_SIZE,
            dirty_tiles,
            head_id,
            in_min_x,
            in_min_y,
            in_max_x,
            in_max_y
        );

        if (!ok) return;

        int orig_count = *num_rects;
        if (dirty_tiles.empty()) {
            // Large damage was a false alarm from compositor! Suppress completely!
            *num_rects = 0;
            m_suppressed_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        m_dirty_count.fetch_add(1, std::memory_order_relaxed);

        // Convert changed tile coordinates into bounding rectangles
        std::vector<DirtyRect> tile_rects;
        tile_rects.reserve(dirty_tiles.size());
        for (const auto& t : dirty_tiles) {
            tile_rects.push_back({
                t.x,
                t.y,
                t.x + t.width,
                t.y + t.height
            });
        }

        auto coalesced = TileEngine::CoalesceRects(tile_rects);
        int max_capacity = std::min(orig_count, 16);
        if (max_capacity <= 0) max_capacity = 16;

        if (static_cast<int>(coalesced.size()) <= max_capacity) {
            for (size_t i = 0; i < coalesced.size(); ++i) {
                rects[i].x1 = coalesced[i].x1;
                rects[i].y1 = coalesced[i].y1;
                rects[i].x2 = coalesced[i].x2;
                rects[i].y2 = coalesced[i].y2;
            }
            *num_rects = static_cast<int>(coalesced.size());
        } else {
            int keep = max_capacity - 1;
            for (int i = 0; i < keep; ++i) {
                rects[i].x1 = coalesced[i].x1;
                rects[i].y1 = coalesced[i].y1;
                rects[i].x2 = coalesced[i].x2;
                rects[i].y2 = coalesced[i].y2;
            }
            int min_x = coalesced[keep].x1, min_y = coalesced[keep].y1;
            int max_x = coalesced[keep].x2, max_y = coalesced[keep].y2;
            for (size_t i = keep + 1; i < coalesced.size(); ++i) {
                min_x = std::min(min_x, coalesced[i].x1);
                min_y = std::min(min_y, coalesced[i].y1);
                max_x = std::max(max_x, coalesced[i].x2);
                max_y = std::max(max_y, coalesced[i].y2);
            }
            rects[keep].x1 = min_x;
            rects[keep].y1 = min_y;
            rects[keep].x2 = max_x;
            rects[keep].y2 = max_y;
            *num_rects = max_capacity;
        }

        uint64_t g = m_grab_count.load(std::memory_order_relaxed);
        if (g % 120 == 0) {
            LOG_INFO("[EVDI-TURBO-SHIM] Stats: frames=%lu, passthru=%lu, suppressed=%lu, gpu_dirty=%lu, tiles=%zu",
                     g, m_passthrough_count.load(std::memory_order_relaxed),
                     m_suppressed_count.load(std::memory_order_relaxed),
                     m_dirty_count.load(std::memory_order_relaxed), dirty_tiles.size());
        }
    }

private:
    ShimManager() {
        EnsureRealEvdi();
        auto& vk = VulkanConverter::Instance();
        if (vk.Initialize()) {
            LOG_INFO("[EVDI-TURBO-SHIM] Vulkan compute engine initialized successfully (%s)",
                     vk.GetDeviceName().c_str());
        } else {
            LOG_ERROR("[EVDI-TURBO-SHIM] Failed to initialize Vulkan compute engine");
        }
        LOG_INFO("[EVDI-TURBO-SHIM] DisplayLink Turbo Vulkan Shim Initialized");
    }

    std::mutex m_mutex;
    void* m_real_libevdi = nullptr;
    std::unordered_map<evdi_handle, int> m_handle_to_device;
    std::unordered_map<evdi_handle, std::unordered_map<int, struct evdi_buffer>> m_buffers;
    std::unordered_map<evdi_handle, int> m_active_buffer;
    std::unordered_map<evdi_handle, int> m_warmup_frames;

    std::atomic<uint64_t> m_grab_count{0};
    std::atomic<uint64_t> m_passthrough_count{0};
    std::atomic<uint64_t> m_suppressed_count{0};
    std::atomic<uint64_t> m_dirty_count{0};
};

} // namespace dl_turbo

// =========================================================================
// Exported C API Proxy Implementation
// =========================================================================
extern "C" {

// Intercept DisplayLinkManager's runtime dlopen of libevdi.so / libevdi.so.1
void* dlopen(const char* filename, int flags) {
    static auto real_dlopen = reinterpret_cast<void* (*)(const char*, int)>(
        dlsym(RTLD_NEXT, "dlopen"));

    if (filename && (strcmp(filename, "libevdi.so.1") == 0 ||
                     strcmp(filename, "libevdi.so") == 0 ||
                     strcmp(filename, "libevdi.so.0") == 0)) {
        LOG_INFO("[EVDI-TURBO-SHIM] Intercepted dlopen(\"%s\") -> redirecting to /usr/local/lib/libevdi_turbo.so", filename);
        return real_dlopen ? real_dlopen("/usr/local/lib/libevdi_turbo.so", flags) : nullptr;
    }

    return real_dlopen ? real_dlopen(filename, flags) : nullptr;
}

#define FORWARD_CALL(ret_type, func, args_decl, args_pass) \
    ret_type func args_decl { \
        static auto real_fn = reinterpret_cast<ret_type (*) args_decl>( \
            dl_turbo::ShimManager::Instance().GetRealSymbol(#func)); \
        if (!real_fn) return static_cast<ret_type>(0); \
        return real_fn args_pass; \
    }

#define FORWARD_VOID(func, args_decl, args_pass) \
    void func args_decl { \
        static auto real_fn = reinterpret_cast<void (*) args_decl>( \
            dl_turbo::ShimManager::Instance().GetRealSymbol(#func)); \
        if (real_fn) real_fn args_pass; \
    }

FORWARD_CALL(enum evdi_device_status, evdi_check_device, (int device), (device))
FORWARD_CALL(int, evdi_add_device, (void), ())
FORWARD_CALL(evdi_handle, evdi_open_attached_to, (const char* sysfs_parent_device), (sysfs_parent_device))
FORWARD_CALL(evdi_handle, evdi_open_attached_to_fixed, (const char* sysfs_parent_device, size_t length), (sysfs_parent_device, length))

evdi_handle evdi_open(int device) {
    static auto real_fn = reinterpret_cast<evdi_handle (*)(int)>(
        dl_turbo::ShimManager::Instance().GetRealSymbol("evdi_open"));
    if (!real_fn) return EVDI_INVALID_HANDLE;
    evdi_handle h = real_fn(device);
    if (h != EVDI_INVALID_HANDLE) {
        dl_turbo::ShimManager::Instance().OnOpen(h, device);
    }
    return h;
}

void evdi_close(evdi_handle handle) {
    static auto real_fn = reinterpret_cast<void (*)(evdi_handle)>(
        dl_turbo::ShimManager::Instance().GetRealSymbol("evdi_close"));
    dl_turbo::ShimManager::Instance().OnClose(handle);
    if (real_fn) real_fn(handle);
}

FORWARD_VOID(evdi_connect, (evdi_handle handle, const unsigned char* edid, const unsigned int edid_length, const uint32_t sku_area_limit), (handle, edid, edid_length, sku_area_limit))
FORWARD_VOID(evdi_connect2, (evdi_handle handle, const unsigned char* edid, const unsigned int edid_length, const uint32_t pixel_area_limit, const uint32_t pixel_per_second_limit), (handle, edid, edid_length, pixel_area_limit, pixel_per_second_limit))
FORWARD_VOID(evdi_disconnect, (evdi_handle handle), (handle))
FORWARD_VOID(evdi_enable_cursor_events, (evdi_handle handle, bool enable), (handle, enable))

void evdi_register_buffer(evdi_handle handle, struct evdi_buffer buffer) {
    static auto real_fn = reinterpret_cast<void (*)(evdi_handle, struct evdi_buffer)>(
        dl_turbo::ShimManager::Instance().GetRealSymbol("evdi_register_buffer"));
    dl_turbo::ShimManager::Instance().OnRegisterBuffer(handle, buffer);
    if (real_fn) real_fn(handle, buffer);
}

void evdi_unregister_buffer(evdi_handle handle, int bufferId) {
    static auto real_fn = reinterpret_cast<void (*)(evdi_handle, int)>(
        dl_turbo::ShimManager::Instance().GetRealSymbol("evdi_unregister_buffer"));
    dl_turbo::ShimManager::Instance().OnUnregisterBuffer(handle, bufferId);
    if (real_fn) real_fn(handle, bufferId);
}

bool evdi_request_update(evdi_handle handle, int bufferId) {
    static auto real_fn = reinterpret_cast<bool (*)(evdi_handle, int)>(
        dl_turbo::ShimManager::Instance().GetRealSymbol("evdi_request_update"));
    dl_turbo::ShimManager::Instance().OnRequestUpdate(handle, bufferId);
    if (!real_fn) return false;
    return real_fn(handle, bufferId);
}

void evdi_grab_pixels(evdi_handle handle, struct evdi_rect* rects, int* num_rects) {
    static auto real_fn = reinterpret_cast<void (*)(evdi_handle, struct evdi_rect*, int*)>(
        dl_turbo::ShimManager::Instance().GetRealSymbol("evdi_grab_pixels"));
    if (real_fn) {
        real_fn(handle, rects, num_rects);
    }
    // Accelerate with Vulkan GPU temporal differencing and tile filtering
    dl_turbo::ShimManager::Instance().FilterDirtyPixels(handle, rects, num_rects);
}

FORWARD_VOID(evdi_ddcci_response, (evdi_handle handle, const unsigned char* buffer, const uint32_t buffer_length, const bool result), (handle, buffer, buffer_length, result))
FORWARD_VOID(evdi_handle_events, (evdi_handle handle, struct evdi_event_context* evtctx), (handle, evtctx))
FORWARD_CALL(evdi_selectable, evdi_get_event_ready, (evdi_handle handle), (handle))
FORWARD_VOID(evdi_get_lib_version, (struct evdi_lib_version* version), (version))
FORWARD_VOID(evdi_set_logging, (struct evdi_logging evdi_logging), (evdi_logging))
FORWARD_CALL(bool, Xorg_running, (void), ())

} // extern "C"
