#include "service.hpp"
#include "vulkan_converter.hpp"
#include <chrono>

namespace dl_turbo {

DisplayLinkService::DisplayLinkService()
    : m_usb(std::make_unique<UsbTransport>()),
      m_evdi(std::make_unique<EvdiDevice>(0)),
      m_tile_engine(std::make_unique<TileEngine>(1920, 1080)),
      m_encoder(std::make_unique<Dl3Encoder>()),
      m_running(false),
      m_frame_counter(0) {
    m_packet_scratch.reserve(1024 * 1024);
}

DisplayLinkService::~DisplayLinkService() {
    Stop();
}

bool DisplayLinkService::Start() {
    if (m_running) return true;

    LOG_INFO("Starting DisplayLink Turbo Driver Service...");

    // 1. Initialize USB transport and enumerate devices
    if (!m_usb->Initialize()) {
        LOG_ERROR("Could not initialize USB transport");
        return false;
    }

    auto devices = m_usb->EnumerateDevices();
    LOG_INFO("Discovered %zu DisplayLink USB device(s) on bus", devices.size());
    for (const auto& d : devices) {
        LOG_INFO("  -> %04x:%04x (%s, Serial: %s)", d.vendor_id, d.product_id,
                 d.product_name.c_str(), d.serial_number.c_str());
    }

    if (!devices.empty()) {
        if (!m_usb->OpenDevice(devices[0].vendor_id, devices[0].product_id)) {
            LOG_WARN("Could not open hardware USB device; proceeding in EVDI emulation mode");
        }
    } else {
        LOG_WARN("No DisplayLink dock connected; operating in virtual EVDI pipeline mode");
    }

    // 2. Set up EVDI Callbacks
    m_evdi->SetModeCallback([this](const ScreenMode& mode) {
        OnModeChanged(mode);
    });

    m_evdi->SetFrameCallback([this](int buf_id, const uint8_t* fb, int w, int h, int stride, const std::vector<DirtyRect>& rects) {
        OnFrameReady(buf_id, fb, w, h, stride, rects);
    });

    // 3. Open EVDI virtual display
    if (!m_evdi->Open()) {
        LOG_ERROR("Failed to open EVDI interface");
        return false;
    }

    // 4. Attach display EDID
    m_evdi->ConnectDisplay(nullptr, 0);

    // 5. Start keepalive thread
    m_running = true;
    m_heartbeat_thread = std::thread(&DisplayLinkService::HeartbeatThread, this);

    LOG_INFO("DisplayLink Turbo Service started successfully");
    return true;
}

void DisplayLinkService::Stop() {
    if (!m_running) return;

    LOG_INFO("Stopping DisplayLink Turbo Service...");
    m_running = false;

    if (m_heartbeat_thread.joinable()) {
        m_heartbeat_thread.join();
    }

    if (m_evdi) {
        m_evdi->Close();
    }

    auto& vk = VulkanConverter::Instance();
    if (vk.IsAvailable()) {
        uint32_t flush_bytes = 0;
        const uint8_t* last_packet = vk.FlushFramePacketsGpu(flush_bytes);
        if (last_packet && flush_bytes > sizeof(protocol::FrameSectionHeader)) {
            if (m_usb && m_usb->IsConnected()) {
                m_usb->SendVideoData(protocol::EP_VIDEO_HEAD0, last_packet, flush_bytes);
            }
        }
    }

    if (m_usb) {
        m_usb->FlushVideoTransfers();
        m_usb->CloseDevice();
    }

    LOG_INFO("DisplayLink Turbo Service stopped");
}

void DisplayLinkService::Join() {
    if (m_heartbeat_thread.joinable()) {
        m_heartbeat_thread.join();
    }
}

void DisplayLinkService::HeartbeatThread() {
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (m_usb && m_usb->IsConnected()) {
            m_usb->SendHeartbeat(0);
        }
    }
}

void DisplayLinkService::OnModeChanged(const ScreenMode& new_mode) {
    LOG_INFO("Service received new mode: %dx%d @ %dHz", new_mode.width, new_mode.height, new_mode.refresh_rate);
    m_tile_engine->Resize(new_mode.width, new_mode.height);

    if (m_usb && m_usb->IsConnected()) {
        protocol::ModeConfigPayload mode_pkt = {};
        mode_pkt.width = static_cast<uint16_t>(new_mode.width);
        mode_pkt.height = static_cast<uint16_t>(new_mode.height);
        mode_pkt.refresh_rate = static_cast<uint16_t>(new_mode.refresh_rate);
        mode_pkt.color_depth = static_cast<uint8_t>(new_mode.bits_per_pixel);
        mode_pkt.pixel_format = static_cast<uint8_t>(new_mode.pixel_format);
        mode_pkt.pixel_clock_khz = new_mode.width * new_mode.height * new_mode.refresh_rate / 1000;

        m_usb->SendCommand(protocol::CommandOpcode::SetVideoMode, 0, &mode_pkt, sizeof(mode_pkt));
    }
}

void DisplayLinkService::OnFrameReady(
    [[maybe_unused]] int buffer_id,
    const uint8_t* fb_data,
    int width,
    int height,
    int stride,
    const std::vector<DirtyRect>& dirty_rects
) {
    if (!m_running || !fb_data) return;

    m_frame_counter++;

    // Fast Path: End-to-End GPU Temporal Differencing and Parallel Tile Compression
    auto& vk = VulkanConverter::Instance();
    if (vk.IsAvailable()) {
        uint32_t total_packet_bytes = 0;
        const uint8_t* packet_data = vk.EncodeFramePacketsGpu(
            fb_data, stride, width, height, 32, m_frame_counter, total_packet_bytes, dirty_rects, true
        );

        if (packet_data && total_packet_bytes > sizeof(protocol::FrameSectionHeader)) {
            if (m_usb && m_usb->IsConnected()) {
                m_usb->SendVideoData(protocol::EP_VIDEO_HEAD0, packet_data, total_packet_bytes);
            }
        }
        return;
    }

    // Fallback Path: CPU SIMD differencing and CPU RLE compression
    auto merged_rects = TileEngine::CoalesceRects(dirty_rects);
    auto candidate_tiles = m_tile_engine->GenerateDirtyTiles(merged_rects);

    auto dirty_tiles = m_tile_engine->FilterChangedTiles(fb_data, stride, candidate_tiles);
    if (dirty_tiles.empty()) return;

    // 2. Prepare Frame Section Header
    protocol::FrameSectionHeader frame_hdr = {};
    frame_hdr.magic         = protocol::DL_FRAME_MAGIC;
    frame_hdr.frame_index   = m_frame_counter;
    frame_hdr.screen_width  = static_cast<uint16_t>(width);
    frame_hdr.screen_height = static_cast<uint16_t>(height);
    frame_hdr.tile_count    = static_cast<uint16_t>(dirty_tiles.size());
    frame_hdr.head_id       = 0;

    m_packet_scratch.clear();
    m_packet_scratch.resize(sizeof(frame_hdr));
    std::memcpy(m_packet_scratch.data(), &frame_hdr, sizeof(frame_hdr));

    // 3. Compress each dirty tile
    std::vector<uint8_t> compressed_tile;
    for (const auto& tile : dirty_tiles) {
        m_tile_engine->ExtractTileRgb32(fb_data, stride, tile, m_tile_scratch);

        if (m_encoder->EncodeTile(m_tile_scratch.data(), tile, compressed_tile)) {
            size_t curr_pos = m_packet_scratch.size();
            m_packet_scratch.resize(curr_pos + compressed_tile.size());
            std::memcpy(m_packet_scratch.data() + curr_pos, compressed_tile.data(), compressed_tile.size());
        }
    }

    // 4. Send frame payload over USB Video Endpoint (EP 8)
    if (m_usb && m_usb->IsConnected()) {
        m_usb->SendVideoData(protocol::EP_VIDEO_HEAD0, m_packet_scratch.data(), m_packet_scratch.size());
    }
}

} // namespace dl_turbo
