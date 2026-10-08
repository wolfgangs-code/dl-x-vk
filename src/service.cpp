#include "service.hpp"
#include "vulkan_converter.hpp"
#include <chrono>

namespace dl_turbo {

DisplayLinkService::DisplayLinkService()
    : m_usb(std::make_unique<UsbTransport>()),
      m_running(false) {
    for (int h = 0; h < MAX_HEADS; ++h) {
        m_evdi[h] = std::make_unique<EvdiDevice>(h);
        m_tile_engine[h] = std::make_unique<TileEngine>(1920, 1080);
        m_encoder[h] = std::make_unique<Dl3Encoder>();
        m_frame_counters[h].store(0);
        m_packet_scratch[h].reserve(1024 * 1024);
    }
}

DisplayLinkService::~DisplayLinkService() {
    Stop();
}

bool DisplayLinkService::Start() {
    if (m_running) return true;

    LOG_INFO("Starting DisplayLink Turbo Multi-Head Driver Service (%d Heads)...", MAX_HEADS);

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

    // 2. Set up EVDI Callbacks and open each display head
    for (int h = 0; h < MAX_HEADS; ++h) {
        m_evdi[h]->SetModeCallback([this, h](const ScreenMode& mode) {
            OnModeChanged(h, mode);
        });

        m_evdi[h]->SetFrameCallback([this, h](int buf_id, const uint8_t* fb, int w, int h_dim, int stride, const std::vector<DirtyRect>& rects) {
            OnFrameReady(h, buf_id, fb, w, h_dim, stride, rects);
        });

        // 3. Open EVDI virtual display
        if (!m_evdi[h]->Open()) {
            LOG_ERROR("Failed to open EVDI interface %d", h);
            if (h == 0) return false;
        } else {
            // 4. Attach display EDID
            m_evdi[h]->ConnectDisplay(nullptr, 0);
            LOG_INFO("Head %d: EVDI device successfully initialized and connected", h);
        }
    }

    // 5. Start keepalive thread
    m_running = true;
    m_heartbeat_thread = std::thread(&DisplayLinkService::HeartbeatThread, this);

    LOG_INFO("DisplayLink Turbo Multi-Head Service started successfully");
    return true;
}

void DisplayLinkService::Stop() {
    if (!m_running) return;

    LOG_INFO("Stopping DisplayLink Turbo Service...");
    m_running = false;

    if (m_heartbeat_thread.joinable()) {
        m_heartbeat_thread.join();
    }

    for (int h = 0; h < MAX_HEADS; ++h) {
        if (m_evdi[h]) {
            m_evdi[h]->Close();
        }
    }

    auto& vk = VulkanConverter::Instance();
    if (vk.IsAvailable()) {
        for (int h = 0; h < MAX_HEADS; ++h) {
            uint32_t flush_bytes = 0;
            const uint8_t* last_packet = vk.FlushFramePacketsGpu(flush_bytes, static_cast<uint8_t>(h));
            if (last_packet && flush_bytes > sizeof(protocol::FrameSectionHeader)) {
                if (m_usb && m_usb->IsConnected()) {
                    uint8_t ep = (h == 1) ? protocol::EP_VIDEO_HEAD1 : protocol::EP_VIDEO_HEAD0;
                    m_usb->SendVideoData(ep, last_packet, flush_bytes);
                }
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
            for (uint8_t h = 0; h < MAX_HEADS; ++h) {
                m_usb->SendHeartbeat(h);
            }
        }
    }
}

void DisplayLinkService::OnModeChanged(int head_id, const ScreenMode& new_mode) {
    if (head_id < 0 || head_id >= MAX_HEADS) return;
    LOG_INFO("Head %d received new mode: %dx%d @ %dHz", head_id, new_mode.width, new_mode.height, new_mode.refresh_rate);
    m_tile_engine[head_id]->Resize(new_mode.width, new_mode.height);

    if (m_usb && m_usb->IsConnected()) {
        protocol::ModeConfigPayload mode_pkt = {};
        mode_pkt.width = static_cast<uint16_t>(new_mode.width);
        mode_pkt.height = static_cast<uint16_t>(new_mode.height);
        mode_pkt.refresh_rate = static_cast<uint16_t>(new_mode.refresh_rate);
        mode_pkt.color_depth = static_cast<uint8_t>(new_mode.bits_per_pixel);
        mode_pkt.pixel_format = static_cast<uint8_t>(new_mode.pixel_format);
        mode_pkt.pixel_clock_khz = new_mode.width * new_mode.height * new_mode.refresh_rate / 1000;

        m_usb->SendCommand(protocol::CommandOpcode::SetVideoMode, static_cast<uint8_t>(head_id), &mode_pkt, sizeof(mode_pkt));
    }
}

void DisplayLinkService::OnFrameReady(
    int head_id,
    [[maybe_unused]] int buffer_id,
    const uint8_t* fb_data,
    int width,
    int height,
    int stride,
    const std::vector<DirtyRect>& dirty_rects
) {
    if (!m_running || !fb_data || head_id < 0 || head_id >= MAX_HEADS) return;

    uint32_t frame_idx = m_frame_counters[head_id].fetch_add(1) + 1;
    uint8_t target_ep = (head_id == 1) ? protocol::EP_VIDEO_HEAD1 : protocol::EP_VIDEO_HEAD0;

    // Fast Path: End-to-End GPU Temporal Differencing and Parallel Tile Compression
    auto& vk = VulkanConverter::Instance();
    if (vk.IsAvailable()) {
        uint32_t total_packet_bytes = 0;
        const uint8_t* packet_data = vk.EncodeFramePacketsGpu(
            fb_data, stride, width, height, 32, frame_idx, total_packet_bytes, dirty_rects, true, static_cast<uint8_t>(head_id)
        );

        if (packet_data && total_packet_bytes > sizeof(protocol::FrameSectionHeader)) {
            if (m_usb && m_usb->IsConnected()) {
                int comp_slot = vk.GetCompletedSlot(head_id);
                m_usb->SendVideoDataZeroCopy(
                    target_ep, packet_data, total_packet_bytes, vk.GetPacketCompletionFlag(head_id, comp_slot)
                );
            }
        }
        return;
    }

    // Fallback Path: CPU SIMD differencing and CPU RLE compression
    auto merged_rects = TileEngine::CoalesceRects(dirty_rects);
    auto candidate_tiles = m_tile_engine[head_id]->GenerateDirtyTiles(merged_rects);

    auto dirty_tiles = m_tile_engine[head_id]->FilterChangedTiles(fb_data, stride, candidate_tiles);
    if (dirty_tiles.empty()) return;

    // 2. Prepare Frame Section Header
    protocol::FrameSectionHeader frame_hdr = {};
    frame_hdr.magic         = protocol::DL_FRAME_MAGIC;
    frame_hdr.frame_index   = frame_idx;
    frame_hdr.screen_width  = static_cast<uint16_t>(width);
    frame_hdr.screen_height = static_cast<uint16_t>(height);
    frame_hdr.tile_count    = static_cast<uint16_t>(dirty_tiles.size());
    frame_hdr.head_id       = static_cast<uint8_t>(head_id);

    m_packet_scratch[head_id].clear();
    m_packet_scratch[head_id].resize(sizeof(frame_hdr));
    std::memcpy(m_packet_scratch[head_id].data(), &frame_hdr, sizeof(frame_hdr));

    // 3. Compress each dirty tile
    std::vector<uint8_t> compressed_tile;
    for (const auto& tile : dirty_tiles) {
        m_tile_engine[head_id]->ExtractTileRgb32(fb_data, stride, tile, m_tile_scratch[head_id]);

        if (m_encoder[head_id]->EncodeTile(m_tile_scratch[head_id].data(), tile, compressed_tile)) {
            size_t curr_pos = m_packet_scratch[head_id].size();
            m_packet_scratch[head_id].resize(curr_pos + compressed_tile.size());
            std::memcpy(m_packet_scratch[head_id].data() + curr_pos, compressed_tile.data(), compressed_tile.size());
        }
    }

    // 4. Send frame payload over target USB Video Endpoint
    if (m_usb && m_usb->IsConnected()) {
        m_usb->SendVideoData(target_ep, m_packet_scratch[head_id].data(), m_packet_scratch[head_id].size());
    }
}

} // namespace dl_turbo
