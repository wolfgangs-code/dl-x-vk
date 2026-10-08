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

struct ImportedBuffer {
    int buffer_id = -1;
    void* host_ptr = nullptr;
    uintptr_t page_addr = 0;
    uint32_t page_offset = 0;
    size_t alloc_size = 0;
    size_t fb_size = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorSet desc_set = VK_NULL_HANDLE;
    bool desc_set_valid = false;
};

class VulkanConverter {
public:
    static VulkanConverter& Instance();

    bool Initialize();
    void Cleanup();
    bool IsAvailable() const { return m_initialized; }
    const std::string& GetDeviceName() const { return m_device_name; }
    bool HasExternalMemoryHost() const { return m_has_external_memory_host; }

    static constexpr int MAX_HEADS = 2;

    // Buffer registration for zero-copy host memory import (Vector 2)
    bool RegisterExternalBuffer(int head_id, int buffer_id, void* host_ptr, size_t fb_size);
    void UnregisterExternalBuffer(int head_id, int buffer_id);
    void UnregisterAllExternalBuffers(int head_id);
    bool IsBufferImported(int head_id, const void* host_ptr) const;

    // GPU-accelerated temporal macro-tile differencing with sub-region clipping
    bool DetectDirtyTiles(
        const uint8_t* curr_fb,
        int fb_stride,
        int width,
        int height,
        int tile_size,
        std::vector<TileCoordinate>& out_dirty_tiles,
        int head_id = 0,
        int clip_x1 = 0,
        int clip_y1 = 0,
        int clip_x2 = 0,
        int clip_y2 = 0
    );

    // Direct GPU differencing dispatch
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
        int head_id = 0,
        VkDescriptorSet custom_desc_set = VK_NULL_HANDLE
    );

    uint32_t GetDirtyTileCount(int head_id = 0) const;
    const uint32_t* GetDirtyTileIndices(int head_id = 0) const;
    const uint32_t* GetDirtyBitmask(int head_id = 0) const;

    uint64_t GetZeroCopyDispatchCount() const { return m_zero_copy_dispatches.load(std::memory_order_relaxed); }
    uint64_t GetFallbackDispatchCount() const { return m_fallback_dispatches.load(std::memory_order_relaxed); }

    ~VulkanConverter();

private:
    struct HeadResources {
        VulkanBuffer buf_input[2];        // Double-buffered mapped fallback input
        VulkanBuffer buf_diff_ref;        // Previous frame stored in GPU Device Local memory
        VulkanBuffer buf_diff_mask[2];    // 1-bit per tile dirty bitmask (double-buffered)
        VulkanBuffer buf_diff_list[2];    // Atomic dirty count + uint32 dirty tile indices (double-buffered)
        uint32_t diff_total_tiles = 0;

        VkDescriptorSet diff_desc_set[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkCommandBuffer cmd_buffer[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkFence fence[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };

        std::vector<ImportedBuffer> imported_buffers;
    };

    VulkanConverter();
    VulkanConverter(const VulkanConverter&) = delete;
    VulkanConverter& operator=(const VulkanConverter&) = delete;

    bool EnsureDiffBuffers(int head_id, size_t fb_size, uint32_t total_tiles);
    void DestroyBuffer(VulkanBuffer& buf);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags required);

    ImportedBuffer* FindImportedBuffer(int head_id, const void* host_ptr);
    void UpdateImportedDescriptorSet(int head_id, ImportedBuffer& imp, size_t fb_size, size_t mask_size, size_t list_size);

    std::recursive_mutex m_mutex;
    bool m_initialized = false;
    std::string m_device_name;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_compute_queue = VK_NULL_HANDLE;
    uint32_t m_compute_queue_family = 0;

    // VK_EXT_external_memory_host support
    bool m_has_external_memory_host = false;
    size_t m_min_imported_host_pointer_alignment = 4096;
    PFN_vkGetMemoryHostPointerPropertiesEXT m_vkGetMemoryHostPointerPropertiesEXT = nullptr;

    // Tile differencing pipeline
    VkShaderModule m_diff_shader_module = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_diff_desc_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_diff_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline m_diff_pipeline = VK_NULL_HANDLE;

    VkDescriptorPool m_desc_pool = VK_NULL_HANDLE;
    VkCommandPool m_cmd_pool = VK_NULL_HANDLE;

    // Independent Per-Head Resources (Head 0 = device 0, Head 1 = device 2)
    HeadResources m_heads[MAX_HEADS];

    std::atomic<uint64_t> m_zero_copy_dispatches{0};
    std::atomic<uint64_t> m_fallback_dispatches{0};
};

} // namespace dl_turbo
