#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <vulkan/vulkan.h>
#include "tile_engine.hpp"

namespace dl_turbo {

struct VulkanBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    size_t size = 0;
};

class VulkanConverter {
public:
    static VulkanConverter& Instance();

    bool Initialize();
    void Cleanup();
    bool IsAvailable() const { return m_initialized; }
    const std::string& GetDeviceName() const { return m_device_name; }

    static constexpr int MAX_HEADS = 2;

    // High-level color conversion for arbitrary host pointers
    bool ConvertRgb32ToYuv420(
        const uint8_t* src_argb,
        int src_stride,
        int width,
        int height,
        uint8_t* dst_y,
        uint8_t* dst_u,
        uint8_t* dst_v,
        int dst_y_stride,
        int dst_uv_stride
    );

    // Direct mapped GPU buffers for zero-copy frame ingestion
    uint8_t* GetMappedInputBuffer(int head_id, int buffer_id, size_t required_bytes = 0);
    uint8_t* GetMappedInputBuffer(int buffer_id = 0, size_t required_bytes = 0) {
        return GetMappedInputBuffer(0, buffer_id, required_bytes);
    }
    void GetMappedOutputPlanes(uint8_t*& y_plane, uint8_t*& u_plane, uint8_t*& v_plane);
    bool DispatchCompute(int width, int height, int src_stride, int dst_y_stride, int dst_uv_stride);

    // GPU-accelerated temporal macro-tile differencing
    bool DetectDirtyTiles(
        const uint8_t* curr_fb,
        int fb_stride,
        int width,
        int height,
        int tile_size,
        std::vector<TileCoordinate>& out_dirty_tiles,
        int head_id = 0
    );

    // Filter candidate tiles against GPU dirty bitmask
    bool FilterDirtyTilesGpu(
        const uint8_t* curr_fb,
        int fb_stride,
        int width,
        int height,
        int tile_size,
        const std::vector<TileCoordinate>& candidate_tiles,
        std::vector<TileCoordinate>& out_changed_tiles,
        int head_id = 0
    );

    // Direct GPU differencing dispatch (zero-copy when input is already in mapped buffer)
    bool DispatchTileDifferencing(
        int width,
        int height,
        int stride_words,
        int tile_size,
        bool update_reference,
        uint32_t start_col = 0,
        uint32_t start_row = 0,
        uint32_t num_cols = 0,
        uint32_t num_rows = 0,
        int head_id = 0
    );

    // GPU-accelerated parallel tile compression directly into DisplayLink USB packet format
    const uint8_t* EncodeFramePacketsGpu(
        const uint8_t* curr_fb,
        int fb_stride,
        int width,
        int height,
        int tile_size,
        uint32_t frame_index,
        uint32_t& out_total_packet_bytes,
        const std::vector<DirtyRect>& dirty_rects = {},
        bool pipelined = false,
        uint8_t head_id = 0
    );

    // Flush any pending in-flight pipelined frame
    const uint8_t* FlushFramePacketsGpu(uint32_t& out_total_packet_bytes, uint8_t head_id = 0);

    uint32_t GetDirtyTileCount(int head_id = 0) const;
    const uint32_t* GetDirtyTileIndices(int head_id = 0) const;
    const uint32_t* GetDirtyBitmask(int head_id = 0) const;
    const uint8_t* GetMappedPacketBuffer(int head_id = 0) const;

    std::atomic<bool>* GetPacketCompletionFlag(int head_id, int slot);
    std::atomic<bool>* GetPacketCompletionFlag(int slot) {
        return GetPacketCompletionFlag(0, slot);
    }
    int GetCompletedSlot(int head_id = 0) const;

    ~VulkanConverter();

private:
    struct HeadResources {
        VulkanBuffer buf_input[2];        // Double-buffered mapped input for zero-copy EVDI ingestion
        VulkanBuffer buf_diff_ref;        // Previous frame stored in GPU Device Local memory
        VulkanBuffer buf_diff_mask[2];    // 1-bit per tile dirty bitmask (double-buffered)
        VulkanBuffer buf_diff_list[2];    // Atomic dirty count + uint32 dirty tile indices (double-buffered)
        uint32_t diff_total_tiles = 0;

        VulkanBuffer buf_packet_meta[2];  // uint32 total_packet_bytes atomic counter (double-buffered)
        VulkanBuffer buf_packet_out[2];   // Contiguous USB packet payload (Host-Cached, double-buffered)
        VulkanBuffer buf_indirect[2];     // VkDispatchIndirectCommand for GPU indirect dispatch (double-buffered)
        size_t packet_capacity = 0;

        VkDescriptorSet diff_desc_set[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkDescriptorSet comp_desc_set[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };

        VkCommandBuffer cmd_buffer[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkFence fence[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };

        int in_flight_slot = -1;
        int last_completed_slot = -1;
        std::atomic<bool> packet_usb_in_flight[2]{false, false};
        uint32_t in_flight_frame_index = 0;
        int in_flight_width = 0;
        int in_flight_height = 0;
        bool in_flight = false;
    };

    VulkanConverter();
    VulkanConverter(const VulkanConverter&) = delete;
    VulkanConverter& operator=(const VulkanConverter&) = delete;

    bool EnsureBuffers(size_t src_size, size_t y_size, size_t uv_size);
    bool EnsureDiffBuffers(int head_id, size_t fb_size, uint32_t total_tiles);
    bool EnsurePacketBuffers(int head_id, size_t max_capacity);
    void UpdateCompDescriptors(int head_id);
    void DestroyBuffer(VulkanBuffer& buf);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags required);

    std::recursive_mutex m_mutex;
    bool m_initialized = false;
    std::string m_device_name;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_compute_queue = VK_NULL_HANDLE;
    uint32_t m_compute_queue_family = 0;

    // Color conversion pipeline
    VkShaderModule m_shader_module = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_desc_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_desc_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_desc_set[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };

    // Tile differencing pipeline
    VkShaderModule m_diff_shader_module = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_diff_desc_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_diff_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline m_diff_pipeline = VK_NULL_HANDLE;

    // Tile compression pipeline
    VkShaderModule m_comp_shader_module = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_comp_desc_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_comp_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline m_comp_pipeline = VK_NULL_HANDLE;

    VkCommandPool m_cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_color_cmd_buffer[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkFence m_color_fence[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };

    // Buffers for color conversion
    VulkanBuffer m_color_buf_input[2];
    VulkanBuffer m_buf_y;
    VulkanBuffer m_buf_u;
    VulkanBuffer m_buf_v;

    // Independent Per-Head Resources (Head 0 = EP 8, Head 1 = EP 10)
    HeadResources m_heads[MAX_HEADS];
};

} // namespace dl_turbo
