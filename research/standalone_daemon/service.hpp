#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <array>
#include "common.hpp"
#include "usb_transport.hpp"
#include "evdi_device.hpp"
#include "tile_engine.hpp"
#include "codec_dl3.hpp"
#include "color_converter.hpp"

namespace dl_turbo {

class DisplayLinkService {
public:
    static constexpr int MAX_HEADS = 2;

    DisplayLinkService();
    ~DisplayLinkService();

    // Start DisplayLink service
    bool Start();

    // Stop service
    void Stop();

    // Wait until service terminates
    void Join();

    bool IsRunning() const { return m_running; }

private:
    void HeartbeatThread();
    void OnFrameReady(int head_id, int buffer_id, const uint8_t* fb_data, int width, int height, int stride, const std::vector<DirtyRect>& dirty_rects);
    void OnModeChanged(int head_id, const ScreenMode& new_mode);

    std::unique_ptr<UsbTransport> m_usb;
    std::array<std::unique_ptr<EvdiDevice>, MAX_HEADS> m_evdi;
    std::array<std::unique_ptr<TileEngine>, MAX_HEADS> m_tile_engine;
    std::array<std::unique_ptr<Dl3Encoder>, MAX_HEADS> m_encoder;

    std::atomic<bool> m_running;
    std::thread m_heartbeat_thread;
    std::array<std::atomic<uint32_t>, MAX_HEADS> m_frame_counters;

    std::array<std::vector<uint8_t>, MAX_HEADS> m_tile_scratch;
    std::array<std::vector<uint8_t>, MAX_HEADS> m_packet_scratch;
};

} // namespace dl_turbo
