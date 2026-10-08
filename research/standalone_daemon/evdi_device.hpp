#pragma once

#include <evdi_lib.h>
#include <cstdint>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include "tile_engine.hpp"

namespace dl_turbo {

struct ScreenMode {
    int width;
    int height;
    int refresh_rate;
    int bits_per_pixel;
    uint32_t pixel_format;
};

using FrameReadyCallback = std::function<void(int buffer_id, const uint8_t* fb_data, int width, int height, int stride, const std::vector<DirtyRect>& dirty_rects)>;
using ModeChangedCallback = std::function<void(const ScreenMode& new_mode)>;

class EvdiDevice {
public:
    EvdiDevice(int device_index = 0);
    ~EvdiDevice();

    // Connects to EVDI kernel interface
    bool Open();

    // Disconnects and releases EVDI buffers
    void Close();

    // Send monitor EDID to kernel DRM to trigger resolution negotiation
    bool ConnectDisplay(const uint8_t* edid_data, size_t edid_len);

    // Disconnect virtual monitor
    void DisconnectDisplay();

    // Register frame callback
    void SetFrameCallback(FrameReadyCallback cb) { m_frame_cb = cb; }
    void SetModeCallback(ModeChangedCallback cb) { m_mode_cb = cb; }

    int GetWidth() const { return m_current_mode.width; }
    int GetHeight() const { return m_current_mode.height; }
    int GetHeadIndex() const { return m_device_index; }
    int GetCardIndex() const { return m_card_index; }
    bool IsActive() const { return m_handle != EVDI_INVALID_HANDLE; }

private:
    void EventLoop();
    void AllocateBuffers(int width, int height);
    void FreeBuffers();

    // Internal static EVDI C callbacks
    static void DpmsHandler(int dpms_mode, void* user_data);
    static void ModeChangedHandler(struct evdi_mode mode, void* user_data);
    static void UpdateReadyHandler(int buffer_to_be_updated, void* user_data);
    static void CrtcStateHandler(int state, void* user_data);
    static void CursorSetHandler(struct evdi_cursor_set cursor_set, void* user_data);
    static void CursorMoveHandler(struct evdi_cursor_move cursor_move, void* user_data);
    static void DdcciDataHandler(struct evdi_ddcci_data ddcci_data, void* user_data);

    int m_device_index;
    int m_card_index = -1;
    evdi_handle m_handle;
    ScreenMode m_current_mode;

    struct FrameBuffer {
        int id;
        bool external = false;
        uint8_t* data_ptr = nullptr;
        std::vector<uint8_t> memory;
        int width;
        int height;
        int stride;
    };

    std::vector<FrameBuffer> m_buffers;
    std::vector<struct evdi_rect> m_rects_scratch;

    std::atomic<bool> m_running;
    std::thread m_event_thread;
    std::mutex m_mutex;

    FrameReadyCallback m_frame_cb;
    ModeChangedCallback m_mode_cb;
};

} // namespace dl_turbo
