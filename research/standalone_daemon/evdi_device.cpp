#include "evdi_device.hpp"
#include "vulkan_converter.hpp"
#include "common.hpp"
#include <unistd.h>
#include <sys/select.h>
#include <cstring>

namespace dl_turbo {

// Standard Fallback 1080p EDID block (128 bytes)
static const uint8_t s_default_1080p_edid[128] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x11, 0xEE, 0x15, 0x60, 0x01, 0x01, 0x01, 0x01,
    0x0A, 0x1E, 0x01, 0x03, 0x80, 0x3C, 0x22, 0x78, 0xEA, 0x2E, 0x35, 0xA4, 0x54, 0x4E, 0x9D, 0x26,
    0x0F, 0x50, 0x54, 0xA5, 0x4B, 0x00, 0x71, 0x4F, 0x81, 0x80, 0x81, 0x99, 0x95, 0x00, 0xB3, 0x00,
    0xD1, 0xC0, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C,
    0x45, 0x00, 0x56, 0x50, 0x21, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x44, 0x4C, 0x54,
    0x55, 0x52, 0x42, 0x4F, 0x30, 0x30, 0x31, 0x0A, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFD, 0x00, 0x38,
    0x4C, 0x1E, 0x53, 0x0F, 0x00, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFC,
    0x00, 0x44, 0x4C, 0x20, 0x54, 0x75, 0x72, 0x62, 0x6F, 0x20, 0x48, 0x44, 0x0A, 0x20, 0x00, 0x47
};

EvdiDevice::EvdiDevice(int device_index)
    : m_device_index(device_index),
      m_handle(EVDI_INVALID_HANDLE),
      m_running(false) {
    m_current_mode = {1920, 1080, 60, 32, static_cast<uint32_t>(PixelFormat::XRGB8888)};
    m_rects_scratch.resize(128);
}

EvdiDevice::~EvdiDevice() {
    Close();
}

bool EvdiDevice::Open() {
    Close();

    // Dynamically discover available EVDI DRM card nodes
    // Linux DRM enumerates all GPUs (/dev/dri/cardX); primary GPU may occupy card1, while EVDI occupies card0 and card2
    auto find_card = [this]() -> int {
        int found = 0;
        for (int card = 0; card < 32; ++card) {
            enum evdi_device_status status = evdi_check_device(card);
            if (status == AVAILABLE) {
                if (found == m_device_index) {
                    return card;
                }
                found++;
            }
        }
        return -1;
    };

    int chosen_card = find_card();
    if (chosen_card == -1) {
        LOG_INFO("Head %d: No available EVDI node found. Adding new EVDI device...", m_device_index);
        int added = evdi_add_device();
        if (added < 0) {
            LOG_ERROR("Failed to add EVDI device");
            return false;
        }
        chosen_card = find_card();
    }

    if (chosen_card == -1) {
        LOG_ERROR("Failed to find or allocate EVDI card for Head %d", m_device_index);
        return false;
    }

    m_card_index = chosen_card;
    m_handle = evdi_open(m_card_index);
    if (m_handle == EVDI_INVALID_HANDLE) {
        LOG_ERROR("Failed to open EVDI device at card %d (Head %d)", m_card_index, m_device_index);
        return false;
    }

    LOG_INFO("Head %d: Successfully opened EVDI interface on /dev/dri/card%d", m_device_index, m_card_index);

    // Start background event pump
    m_running = true;
    m_event_thread = std::thread(&EvdiDevice::EventLoop, this);

    return true;
}

void EvdiDevice::Close() {
    m_running = false;
    if (m_event_thread.joinable()) {
        m_event_thread.join();
    }

    if (m_handle != EVDI_INVALID_HANDLE) {
        DisconnectDisplay();
        FreeBuffers();
        evdi_close(m_handle);
        m_handle = EVDI_INVALID_HANDLE;
        LOG_INFO("Head %d: Closed EVDI device on /dev/dri/card%d", m_device_index, m_card_index);
        m_card_index = -1;
    }
}

bool EvdiDevice::ConnectDisplay(const uint8_t* edid_data, size_t edid_len) {
    if (m_handle == EVDI_INVALID_HANDLE) return false;

    const uint8_t* edid = (edid_data && edid_len >= 128) ? edid_data : s_default_1080p_edid;
    uint32_t len = (edid_data && edid_len >= 128) ? static_cast<uint32_t>(edid_len) : sizeof(s_default_1080p_edid);

    LOG_INFO("Connecting virtual display to EVDI (EDID length: %u)", len);
    evdi_connect2(m_handle, edid, len, 3840 * 2160, 3840 * 2160 * 60);
    return true;
}

void EvdiDevice::DisconnectDisplay() {
    if (m_handle != EVDI_INVALID_HANDLE) {
        evdi_disconnect(m_handle);
    }
}

void EvdiDevice::AllocateBuffers(int width, int height) {
    FreeBuffers();

    int stride = width * 4;
    size_t size = static_cast<size_t>(stride) * height;

    LOG_INFO("Allocating double buffers: %dx%d (stride=%d, size=%zu)", width, height, stride, size);

    auto& vk = VulkanConverter::Instance();
    bool use_vk = vk.IsAvailable();

    m_buffers.resize(2);
    for (int i = 0; i < 2; ++i) {
        m_buffers[i].id = i;
        m_buffers[i].width = width;
        m_buffers[i].height = height;
        m_buffers[i].stride = stride;

        uint8_t* ptr = nullptr;
        if (use_vk) {
            ptr = vk.GetMappedInputBuffer(m_device_index, i, size);
        }

        if (ptr) {
            m_buffers[i].external = true;
            m_buffers[i].data_ptr = ptr;
            m_buffers[i].memory.clear();
            LOG_INFO("EVDI [Head %d] Buffer %d registered directly to Vulkan mapped GPU buffer (Zero-Copy)", m_device_index, i);
        } else {
            m_buffers[i].external = false;
            m_buffers[i].memory.resize(size, 0);
            m_buffers[i].data_ptr = m_buffers[i].memory.data();
        }

        struct evdi_buffer buf;
        buf.id = i;
        buf.buffer = m_buffers[i].data_ptr;
        buf.width = width;
        buf.height = height;
        buf.stride = stride;
        buf.rects = nullptr;
        buf.rect_count = 0;

        evdi_register_buffer(m_handle, buf);
    }

    // Trigger initial update request
    evdi_request_update(m_handle, 0);
}

void EvdiDevice::FreeBuffers() {
    if (m_handle == EVDI_INVALID_HANDLE) return;

    for (const auto& b : m_buffers) {
        evdi_unregister_buffer(m_handle, b.id);
    }
    m_buffers.clear();
}

void EvdiDevice::EventLoop() {
    struct evdi_event_context ctx = {};
    ctx.dpms_handler = DpmsHandler;
    ctx.mode_changed_handler = ModeChangedHandler;
    ctx.update_ready_handler = UpdateReadyHandler;
    ctx.crtc_state_handler = CrtcStateHandler;
    ctx.cursor_set_handler = CursorSetHandler;
    ctx.cursor_move_handler = CursorMoveHandler;
    ctx.ddcci_data_handler = DdcciDataHandler;
    ctx.user_data = this;

    while (m_running && m_handle != EVDI_INVALID_HANDLE) {
        evdi_selectable sel = evdi_get_event_ready(m_handle);
        if (sel >= 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sel, &rfds);

            struct timeval tv = {0, 20000}; // 20ms
            int retval = select(sel + 1, &rfds, nullptr, nullptr, &tv);
            if (retval > 0 && FD_ISSET(sel, &rfds)) {
                evdi_handle_events(m_handle, &ctx);
            }
        } else {
            usleep(10000);
        }
    }
}

// EVDI C Callback Implementations
void EvdiDevice::DpmsHandler(int dpms_mode, [[maybe_unused]] void* user_data) {
    LOG_INFO("EVDI DPMS state changed to: %d", dpms_mode);
}

void EvdiDevice::ModeChangedHandler(struct evdi_mode mode, void* user_data) {
    auto self = reinterpret_cast<EvdiDevice*>(user_data);
    LOG_INFO("EVDI Mode changed: %dx%d @ %dHz (%d bpp)", mode.width, mode.height, mode.refresh_rate, mode.bits_per_pixel);

    self->m_current_mode.width = mode.width;
    self->m_current_mode.height = mode.height;
    self->m_current_mode.refresh_rate = mode.refresh_rate;
    self->m_current_mode.bits_per_pixel = mode.bits_per_pixel;
    self->m_current_mode.pixel_format = mode.pixel_format;

    self->AllocateBuffers(mode.width, mode.height);

    if (self->m_mode_cb) {
        self->m_mode_cb(self->m_current_mode);
    }
}

void EvdiDevice::UpdateReadyHandler(int buffer_id, void* user_data) {
    auto self = reinterpret_cast<EvdiDevice*>(user_data);

    if (buffer_id < 0 || buffer_id >= static_cast<int>(self->m_buffers.size())) {
        return;
    }

    int num_rects = static_cast<int>(self->m_rects_scratch.size());
    evdi_grab_pixels(self->m_handle, self->m_rects_scratch.data(), &num_rects);

    std::vector<DirtyRect> dirty;
    dirty.reserve(num_rects);
    for (int i = 0; i < num_rects; ++i) {
        dirty.push_back({
            self->m_rects_scratch[i].x1,
            self->m_rects_scratch[i].y1,
            self->m_rects_scratch[i].x2,
            self->m_rects_scratch[i].y2
        });
    }

    if (self->m_frame_cb) {
        const auto& fb = self->m_buffers[buffer_id];
        self->m_frame_cb(buffer_id, fb.data_ptr, fb.width, fb.height, fb.stride, dirty);
    }

    // Request next buffer flip (double buffering swap)
    int next_buf = 1 - buffer_id;
    evdi_request_update(self->m_handle, next_buf);
}

void EvdiDevice::CrtcStateHandler(int state, [[maybe_unused]] void* user_data) {
    LOG_DEBUG("EVDI CRTC State: %d", state);
}

void EvdiDevice::CursorSetHandler(struct evdi_cursor_set cursor_set, [[maybe_unused]] void* user_data) {
    LOG_DEBUG("EVDI Cursor Set: %ux%u", cursor_set.width, cursor_set.height);
}

void EvdiDevice::CursorMoveHandler([[maybe_unused]] struct evdi_cursor_move cursor_move, [[maybe_unused]] void* user_data) {
    // High-frequency cursor position updates
}

void EvdiDevice::DdcciDataHandler(struct evdi_ddcci_data ddcci_data, void* user_data) {
    auto self = reinterpret_cast<EvdiDevice*>(user_data);
    LOG_DEBUG("EVDI DDC/CI request on address 0x%04x", ddcci_data.address);
    // Respond with empty ack
    evdi_ddcci_response(self->m_handle, nullptr, 0, true);
}

} // namespace dl_turbo
