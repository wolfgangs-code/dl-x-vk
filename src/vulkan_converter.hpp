#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <string>
#include <mutex>
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

    // Direct mapped GPU buffers for zero-copy color conversion
    uint8_t* GetMappedInputBuffer(size_t required_bytes);
    void GetMappedOutputPlanes(uint8_t*& y_plane, uint8_t*& u_plane, uint8_t*& v_plane);
    bool DispatchCompute(int width, int height, int src_stride, int dst_y_stride, int dst_uv_stride);

    // GPU-accelerated temporal macro-tile differencing
    bool DetectDirtyTiles(
        const uint8_t* curr_fb,
        int fb_stride,
        int width,
        int height,
        int tile_size,
        std::vector<TileCoordinate>& out_dirty_tiles
    );

    // Filter candidate tiles against GPU dirty bitmask
    bool FilterDirtyTilesGpu(
        const uint8_t* curr_fb,
        int fb_stride,
        int width,
        int height,
        int tile_size,
        const std::vector<TileCoordinate>& candidate_tiles,
        std::vector<TileCoordinate>& out_changed_tiles
    );

    // Direct GPU differencing dispatch (zero-copy when input is already in mapped buffer)
    bool DispatchTileDifferencing(
        int width,
        int height,
        int stride_words,
        int tile_size,
        bool update_reference
    );

    // GPU-accelerated parallel tile compression directly into DisplayLink USB packet format
    const uint8_t* EncodeFramePacketsGpu(
        const uint8_t* curr_fb,
        int fb_stride,
        int width,
        int height,
        int tile_size,
        uint32_t frame_index,
        uint32_t& out_total_packet_bytes
    );

    uint32_t GetDirtyTileCount() const;
    const uint32_t* GetDirtyTileIndices() const;
    const uint32_t* GetDirtyBitmask() const;
    const uint8_t* GetMappedPacketBuffer() const;

    ~VulkanConverter();

private:
    VulkanConverter();
    VulkanConverter(const VulkanConverter&) = delete;
    VulkanConverter& operator=(const VulkanConverter&) = delete;

    bool EnsureBuffers(size_t src_size, size_t y_size, size_t uv_size);
    bool EnsureDiffBuffers(size_t fb_size, uint32_t total_tiles);
    bool EnsurePacketBuffers(size_t max_capacity);
    void UpdateCompDescriptors();
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
    VkDescriptorSet m_desc_set = VK_NULL_HANDLE;

    // Tile differencing pipeline
    VkShaderModule m_diff_shader_module = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_diff_desc_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_diff_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline m_diff_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_diff_desc_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_diff_desc_set = VK_NULL_HANDLE;

    // Tile compression pipeline
    VkShaderModule m_comp_shader_module = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_comp_desc_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_comp_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline m_comp_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_comp_desc_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_comp_desc_set = VK_NULL_HANDLE;

    VkCommandPool m_cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd_buffer = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;

    // Buffers
    VulkanBuffer m_buf_input;
    VulkanBuffer m_buf_y;
    VulkanBuffer m_buf_u;
    VulkanBuffer m_buf_v;

    VulkanBuffer m_buf_diff_ref;   // Previous frame stored in GPU Device Local memory
    VulkanBuffer m_buf_diff_mask;  // 1-bit per tile dirty bitmask
    VulkanBuffer m_buf_diff_list;  // Atomic dirty count + uint32 dirty tile indices
    uint32_t m_diff_total_tiles = 0;

    VulkanBuffer m_buf_packet_meta; // uint32 total_packet_bytes atomic counter
    VulkanBuffer m_buf_packet_out;  // Contiguous USB packet payload (Host-Cached)
    size_t m_packet_capacity = 0;
};

} // namespace dl_turbo
