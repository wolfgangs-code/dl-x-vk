#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <vulkan/vulkan.h>

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

    // High-level conversion for arbitrary host pointers
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

    // Direct mapped GPU buffers for zero-copy operation
    uint8_t* GetMappedInputBuffer(size_t required_bytes);
    void GetMappedOutputPlanes(uint8_t*& y_plane, uint8_t*& u_plane, uint8_t*& v_plane);
    bool DispatchCompute(int width, int height, int src_stride, int dst_y_stride, int dst_uv_stride);

    ~VulkanConverter();

private:
    VulkanConverter();
    VulkanConverter(const VulkanConverter&) = delete;
    VulkanConverter& operator=(const VulkanConverter&) = delete;

    bool EnsureBuffers(size_t src_size, size_t y_size, size_t uv_size);
    void DestroyBuffer(VulkanBuffer& buf);
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags required);

    std::mutex m_mutex;
    bool m_initialized = false;
    std::string m_device_name;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_compute_queue = VK_NULL_HANDLE;
    uint32_t m_compute_queue_family = 0;

    VkShaderModule m_shader_module = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_desc_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;

    VkDescriptorPool m_desc_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_desc_set = VK_NULL_HANDLE;

    VkCommandPool m_cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd_buffer = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;

    VulkanBuffer m_buf_input;
    VulkanBuffer m_buf_y;
    VulkanBuffer m_buf_u;
    VulkanBuffer m_buf_v;
};

} // namespace dl_turbo
