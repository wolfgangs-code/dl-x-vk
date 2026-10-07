#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include "common.hpp"
#include "usb_transport.hpp"
#include "evdi_device.hpp"
#include "tile_engine.hpp"
#include "codec_dl3.hpp"
#include "color_converter.hpp"

namespace dl_turbo {

class DisplayLinkService {
public:
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
    void OnFrameReady(int buffer_id, const uint8_t* fb_data, int width, int height, int stride, const std::vector<DirtyRect>& dirty_rects);
    void OnModeChanged(const ScreenMode& new_mode);

    std::unique_ptr<UsbTransport> m_usb;
    std::unique_ptr<EvdiDevice> m_evdi;
    std::unique_ptr<TileEngine> m_tile_engine;
    std::unique_ptr<Dl3Encoder> m_encoder;

    std::atomic<bool> m_running;
    std::thread m_heartbeat_thread;
    uint32_t m_frame_counter;

    std::vector<uint8_t> m_tile_scratch;
    std::vector<uint8_t> m_packet_scratch;
};

} // namespace dl_turbo
