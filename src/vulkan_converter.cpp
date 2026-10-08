#include "vulkan_converter.hpp"
#include "common.hpp"
#include "protocol.hpp"
#include <cstring>
#include <algorithm>
#include <thread>

namespace dl_turbo {

static const uint32_t s_rgb_to_yuv420_spv[] =
#include "shaders/rgb_to_yuv420_spv.inc"
;

static const uint32_t s_tile_differencing_spv[] =
#include "shaders/tile_differencing_spv.inc"
;

static const uint32_t s_tile_compression_spv[] =
#include "shaders/tile_compression_spv.inc"
;

struct ColorPushConstants {
    uint32_t width;
    uint32_t height;
    uint32_t src_stride;
    uint32_t dst_y_stride;
    uint32_t dst_uv_stride;
};

struct DiffPushConstants {
    uint32_t width;
    uint32_t height;
    uint32_t stride_words;
    uint32_t grid_cols;
    uint32_t grid_rows;
    uint32_t tile_size;
    uint32_t update_ref;
    uint32_t start_col;
    uint32_t start_row;
};

struct CompPushConstants {
    uint32_t width;
    uint32_t height;
    uint32_t stride_words;
    uint32_t grid_cols;
    uint32_t grid_rows;
    uint32_t tile_size;
    uint32_t dirty_tile_count;
};

VulkanConverter& VulkanConverter::Instance() {
    static VulkanConverter s_instance;
    return s_instance;
}

VulkanConverter::VulkanConverter() {
    for (int h = 0; h < MAX_HEADS; ++h) {
        m_heads[h].packet_usb_in_flight[0].store(false);
        m_heads[h].packet_usb_in_flight[1].store(false);
    }
    Initialize();
}

VulkanConverter::~VulkanConverter() {
    Cleanup();
}

uint32_t VulkanConverter::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physical_device, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            ((memProperties.memoryTypes[i].propertyFlags & preferred) == preferred)) {
            return i;
        }
    }
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            ((memProperties.memoryTypes[i].propertyFlags & required) == required)) {
            return i;
        }
    }
    LOG_ERROR("Failed to find appropriate Vulkan memory type (filter: 0x%x, req: 0x%x)", typeFilter, required);
    return UINT32_MAX;
}

void VulkanConverter::DestroyBuffer(VulkanBuffer& buf) {
    if (buf.mapped && buf.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(m_device, buf.memory);
        buf.mapped = nullptr;
    }
    if (buf.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, buf.buffer, nullptr);
        buf.buffer = VK_NULL_HANDLE;
    }
    if (buf.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, buf.memory, nullptr);
        buf.memory = VK_NULL_HANDLE;
    }
    buf.size = 0;
}

bool VulkanConverter::EnsureBuffers(size_t src_size, size_t y_size, size_t uv_size) {
    if (m_color_buf_input[0].size >= src_size &&
        m_color_buf_input[1].size >= src_size &&
        m_buf_y.size >= y_size &&
        m_buf_u.size >= uv_size &&
        m_buf_v.size >= uv_size) {
        return true;
    }

    size_t alloc_src = std::max(src_size, m_color_buf_input[0].size * 2);
    size_t alloc_y   = std::max(y_size, m_buf_y.size * 2);
    size_t alloc_uv  = std::max(uv_size, m_buf_u.size * 2);

    DestroyBuffer(m_color_buf_input[0]);
    DestroyBuffer(m_color_buf_input[1]);
    DestroyBuffer(m_buf_y);
    DestroyBuffer(m_buf_u);
    DestroyBuffer(m_buf_v);

    struct BufferSpec {
        VulkanBuffer& buf;
        size_t size;
        bool is_output;
    } specs[5] = {
        { m_color_buf_input[0], alloc_src, false },
        { m_color_buf_input[1], alloc_src, false },
        { m_buf_y,              alloc_y,   true  },
        { m_buf_u,              alloc_uv,  true  },
        { m_buf_v,              alloc_uv,  true  }
    };

    for (int i = 0; i < 5; i++) {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = specs[i].size;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &bci, nullptr, &specs[i].buf.buffer) != VK_SUCCESS) {
            LOG_ERROR("Failed to create Vulkan storage buffer %d of size %zu", i, specs[i].size);
            return false;
        }

        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(m_device, specs[i].buf.buffer, &memReq);

        VkMemoryPropertyFlags pref = specs[i].is_output
            ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
            : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryPropertyFlags req = (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        uint32_t memType = FindMemoryType(memReq.memoryTypeBits, pref, req);
        if (memType == UINT32_MAX) return false;

        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = memReq.size;
        ai.memoryTypeIndex = memType;

        if (vkAllocateMemory(m_device, &ai, nullptr, &specs[i].buf.memory) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate Vulkan memory for buffer %d", i);
            return false;
        }

        vkBindBufferMemory(m_device, specs[i].buf.buffer, specs[i].buf.memory, 0);
        if (vkMapMemory(m_device, specs[i].buf.memory, 0, specs[i].size, 0, &specs[i].buf.mapped) != VK_SUCCESS) {
            LOG_ERROR("Failed to map Vulkan buffer %d", i);
            return false;
        }
        specs[i].buf.size = specs[i].size;
    }

    for (int b = 0; b < 2; b++) {
        VkDescriptorBufferInfo bufDescs[4] = {
            { m_color_buf_input[b].buffer, 0, m_color_buf_input[b].size },
            { m_buf_y.buffer,              0, m_buf_y.size },
            { m_buf_u.buffer,              0, m_buf_u.size },
            { m_buf_v.buffer,              0, m_buf_v.size }
        };

        VkWriteDescriptorSet writes[4] = {};
        for (int i = 0; i < 4; i++) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = m_desc_set[b];
            writes[i].dstBinding = i;
            writes[i].dstArrayElement = 0;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo = &bufDescs[i];
        }
        vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
    }

    return true;
}

bool VulkanConverter::EnsureDiffBuffers(int head_id, size_t fb_size, uint32_t total_tiles) {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    auto& head = m_heads[head_id];

    size_t mask_size = ((total_tiles + 31) / 32) * sizeof(uint32_t);
    size_t list_size = sizeof(uint32_t) + total_tiles * sizeof(uint32_t);

    if (head.buf_input[0].size >= fb_size &&
        head.buf_input[1].size >= fb_size &&
        head.buf_diff_ref.size >= fb_size &&
        head.buf_diff_mask[0].size >= mask_size &&
        head.buf_diff_mask[1].size >= mask_size &&
        head.buf_diff_list[0].size >= list_size &&
        head.buf_diff_list[1].size >= list_size) {
        return true;
    }

    // Allocate / resize input double-buffers for this head (EVDI Zero-Copy mapped input)
    for (int b = 0; b < 2; b++) {
        if (head.buf_input[b].size < fb_size) {
            DestroyBuffer(head.buf_input[b]);
            size_t alloc_src = std::max(fb_size, head.buf_input[b].size * 2);

            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, alloc_src,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
            if (vkCreateBuffer(m_device, &bci, nullptr, &head.buf_input[b].buffer) != VK_SUCCESS) return false;

            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(m_device, head.buf_input[b].buffer, &req);
            uint32_t memType = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (memType == UINT32_MAX) return false;

            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memType };
            if (vkAllocateMemory(m_device, &ai, nullptr, &head.buf_input[b].memory) != VK_SUCCESS) return false;

            vkBindBufferMemory(m_device, head.buf_input[b].buffer, head.buf_input[b].memory, 0);
            if (vkMapMemory(m_device, head.buf_input[b].memory, 0, alloc_src, 0, &head.buf_input[b].mapped) != VK_SUCCESS) return false;
            head.buf_input[b].size = alloc_src;
        }
    }

    // Allocate reference frame buffer (Device Local on GPU for max performance)
    if (head.buf_diff_ref.size < fb_size) {
        DestroyBuffer(head.buf_diff_ref);
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = fb_size;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &bci, nullptr, &head.buf_diff_ref.buffer) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_device, head.buf_diff_ref.buffer, &req);
        uint32_t memType = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);

        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memType };
        if (vkAllocateMemory(m_device, &ai, nullptr, &head.buf_diff_ref.memory) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, head.buf_diff_ref.buffer, head.buf_diff_ref.memory, 0);
        head.buf_diff_ref.size = fb_size;

        // Zero out initial reference frame using head's command buffer
        VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, 0, nullptr };
        vkBeginCommandBuffer(head.cmd_buffer[0], &cbbi);
        vkCmdFillBuffer(head.cmd_buffer[0], head.buf_diff_ref.buffer, 0, VK_WHOLE_SIZE, 0);
        vkEndCommandBuffer(head.cmd_buffer[0]);

        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &head.cmd_buffer[0], 0, nullptr };
        vkResetFences(m_device, 1, &head.fence[0]);
        vkQueueSubmit(m_compute_queue, 1, &si, head.fence[0]);
        vkWaitForFences(m_device, 1, &head.fence[0], VK_TRUE, UINT64_MAX);
    }

    // Allocate double-buffered dirty bitmask and dirty list buffers
    for (int b = 0; b < 2; b++) {
        if (head.buf_diff_mask[b].size < mask_size) {
            DestroyBuffer(head.buf_diff_mask[b]);
            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, mask_size,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
            if (vkCreateBuffer(m_device, &bci, nullptr, &head.buf_diff_mask[b].buffer) != VK_SUCCESS) return false;
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(m_device, head.buf_diff_mask[b].buffer, &req);
            uint32_t memType = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memType };
            if (vkAllocateMemory(m_device, &ai, nullptr, &head.buf_diff_mask[b].memory) != VK_SUCCESS) return false;
            vkBindBufferMemory(m_device, head.buf_diff_mask[b].buffer, head.buf_diff_mask[b].memory, 0);
            vkMapMemory(m_device, head.buf_diff_mask[b].memory, 0, mask_size, 0, &head.buf_diff_mask[b].mapped);
            head.buf_diff_mask[b].size = mask_size;
        }

        if (head.buf_diff_list[b].size < list_size) {
            DestroyBuffer(head.buf_diff_list[b]);
            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, list_size,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
            if (vkCreateBuffer(m_device, &bci, nullptr, &head.buf_diff_list[b].buffer) != VK_SUCCESS) return false;
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(m_device, head.buf_diff_list[b].buffer, &req);
            uint32_t memType = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memType };
            if (vkAllocateMemory(m_device, &ai, nullptr, &head.buf_diff_list[b].memory) != VK_SUCCESS) return false;
            vkBindBufferMemory(m_device, head.buf_diff_list[b].buffer, head.buf_diff_list[b].memory, 0);
            vkMapMemory(m_device, head.buf_diff_list[b].memory, 0, list_size, 0, &head.buf_diff_list[b].mapped);
            head.buf_diff_list[b].size = list_size;
        }
    }

    head.diff_total_tiles = total_tiles;

    // Update differencing descriptor sets for both double-buffered inputs for this head
    for (int b = 0; b < 2; b++) {
        VkDescriptorBufferInfo dbi[4] = {
            { head.buf_input[b].buffer,     0, fb_size },
            { head.buf_diff_ref.buffer,     0, fb_size },
            { head.buf_diff_mask[b].buffer, 0, mask_size },
            { head.buf_diff_list[b].buffer, 0, list_size }
        };

        VkWriteDescriptorSet writes[4] = {};
        for (int i = 0; i < 4; i++) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = head.diff_desc_set[b];
            writes[i].dstBinding = i;
            writes[i].dstArrayElement = 0;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo = &dbi[i];
        }
        vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
    }
    UpdateCompDescriptors(head_id);

    return true;
}

bool VulkanConverter::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_initialized) return true;

    // 1. Instance
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "dl-x-vk";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 1, 0);
    appInfo.pEngineName = "DisplayLinkTurbo";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instInfo = {};
    instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instInfo.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&instInfo, nullptr, &m_instance) != VK_SUCCESS) {
        LOG_WARN("Could not initialize Vulkan instance; falling back to CPU");
        return false;
    }

    // 2. Physical Device
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        LOG_WARN("No Vulkan physical devices found");
        Cleanup();
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    for (const auto& dev : devices) {
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfProps(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qfCount, qfProps.data());

        for (uint32_t i = 0; i < qfCount; i++) {
            if (qfProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                m_physical_device = dev;
                m_compute_queue_family = i;
                break;
            }
        }
        if (m_physical_device != VK_NULL_HANDLE) break;
    }

    if (m_physical_device == VK_NULL_HANDLE) {
        LOG_WARN("No Vulkan physical device with compute queue found");
        Cleanup();
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_physical_device, &props);
    m_device_name = props.deviceName;
    LOG_INFO("Vulkan compute device selected: %s (Driver: %u.%u.%u)",
             props.deviceName,
             VK_VERSION_MAJOR(props.driverVersion),
             VK_VERSION_MINOR(props.driverVersion),
             VK_VERSION_PATCH(props.driverVersion));

    // 3. Logical Device
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = m_compute_queue_family;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceVulkan12Features features12 = {};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.storageBuffer8BitAccess = VK_TRUE;
    features12.shaderInt8 = VK_TRUE;

    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.pNext = &features12;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;

    if (vkCreateDevice(m_physical_device, &deviceCreateInfo, nullptr, &m_device) != VK_SUCCESS) {
        LOG_WARN("Failed to create Vulkan logical device with 8-bit storage");
        Cleanup();
        return false;
    }

    vkGetDeviceQueue(m_device, m_compute_queue_family, 0, &m_compute_queue);

    // 4. Color Conversion Pipeline
    VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                            sizeof(s_rgb_to_yuv420_spv), s_rgb_to_yuv420_spv };
    if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &m_shader_module) != VK_SUCCESS) {
        LOG_ERROR("Failed to create color conversion shader module");
        Cleanup();
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[4] = {};
    for (int i = 0; i < 4; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4, bindings };
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_desc_layout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create color descriptor set layout");
        Cleanup();
        return false;
    }

    VkPushConstantRange colorPush = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ColorPushConstants) };
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &m_desc_layout, 1, &colorPush };
    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipeline_layout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create color pipeline layout");
        Cleanup();
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m_shader_module, "main", nullptr },
        m_pipeline_layout, VK_NULL_HANDLE, 0
    };
    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        LOG_ERROR("Failed to create color compute pipeline");
        Cleanup();
        return false;
    }

    // 5. Tile Differencing Pipeline
    VkShaderModuleCreateInfo diffModuleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                                sizeof(s_tile_differencing_spv), s_tile_differencing_spv };
    if (vkCreateShaderModule(m_device, &diffModuleInfo, nullptr, &m_diff_shader_module) != VK_SUCCESS) {
        LOG_ERROR("Failed to create tile differencing shader module");
        Cleanup();
        return false;
    }

    VkDescriptorSetLayoutCreateInfo diffLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4, bindings };
    if (vkCreateDescriptorSetLayout(m_device, &diffLayoutInfo, nullptr, &m_diff_desc_layout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create diff descriptor set layout");
        Cleanup();
        return false;
    }

    VkPushConstantRange diffPush = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DiffPushConstants) };
    VkPipelineLayoutCreateInfo diffPipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &m_diff_desc_layout, 1, &diffPush };
    if (vkCreatePipelineLayout(m_device, &diffPipelineLayoutInfo, nullptr, &m_diff_pipeline_layout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create diff pipeline layout");
        Cleanup();
        return false;
    }

    VkComputePipelineCreateInfo diffPipelineInfo = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m_diff_shader_module, "main", nullptr },
        m_diff_pipeline_layout, VK_NULL_HANDLE, 0
    };
    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &diffPipelineInfo, nullptr, &m_diff_pipeline) != VK_SUCCESS) {
        LOG_ERROR("Failed to create diff compute pipeline");
        Cleanup();
        return false;
    }

    // 6. Tile Compression Pipeline
    VkShaderModuleCreateInfo compModuleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                                sizeof(s_tile_compression_spv), s_tile_compression_spv };
    if (vkCreateShaderModule(m_device, &compModuleInfo, nullptr, &m_comp_shader_module) != VK_SUCCESS) {
        LOG_ERROR("Failed to create tile compression shader module");
        Cleanup();
        return false;
    }

    VkDescriptorSetLayoutCreateInfo compLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4, bindings };
    if (vkCreateDescriptorSetLayout(m_device, &compLayoutInfo, nullptr, &m_comp_desc_layout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create comp descriptor set layout");
        Cleanup();
        return false;
    }

    VkPushConstantRange compPush = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CompPushConstants) };
    VkPipelineLayoutCreateInfo compPipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &m_comp_desc_layout, 1, &compPush };
    if (vkCreatePipelineLayout(m_device, &compPipelineLayoutInfo, nullptr, &m_comp_pipeline_layout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create comp pipeline layout");
        Cleanup();
        return false;
    }

    VkComputePipelineCreateInfo compPipelineInfo = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m_comp_shader_module, "main", nullptr },
        m_comp_pipeline_layout, VK_NULL_HANDLE, 0
    };
    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &compPipelineInfo, nullptr, &m_comp_pipeline) != VK_SUCCESS) {
        LOG_ERROR("Failed to create comp compute pipeline");
        Cleanup();
        return false;
    }

    // 7. Descriptor Pools & Sets
    VkDescriptorPoolSize poolSizes[1] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128 } };
    VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 32, 1, poolSizes };

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_desc_pool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create descriptor pool");
        Cleanup();
        return false;
    }

    VkDescriptorSetLayout colorLayouts[2] = { m_desc_layout, m_desc_layout };
    VkDescriptorSetAllocateInfo allocSetInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, m_desc_pool, 2, colorLayouts };
    if (vkAllocateDescriptorSets(m_device, &allocSetInfo, m_desc_set) != VK_SUCCESS) {
        LOG_ERROR("Failed to allocate color descriptor sets");
        Cleanup();
        return false;
    }

    for (int h = 0; h < MAX_HEADS; ++h) {
        VkDescriptorSetLayout diffLayouts[2] = { m_diff_desc_layout, m_diff_desc_layout };
        VkDescriptorSetAllocateInfo diffAllocSetInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, m_desc_pool, 2, diffLayouts };
        if (vkAllocateDescriptorSets(m_device, &diffAllocSetInfo, m_heads[h].diff_desc_set) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate diff descriptor sets for head %d", h);
            Cleanup();
            return false;
        }

        VkDescriptorSetLayout compLayouts[2] = { m_comp_desc_layout, m_comp_desc_layout };
        VkDescriptorSetAllocateInfo compAllocSetInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, m_desc_pool, 2, compLayouts };
        if (vkAllocateDescriptorSets(m_device, &compAllocSetInfo, m_heads[h].comp_desc_set) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate comp descriptor sets for head %d", h);
            Cleanup();
            return false;
        }
    }

    // 8. Command Pool & Command Buffers & Fences (Double-Buffered per Head + Color)
    VkCommandPoolCreateInfo cmdPoolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, m_compute_queue_family };
    if (vkCreateCommandPool(m_device, &cmdPoolInfo, nullptr, &m_cmd_pool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create command pool");
        Cleanup();
        return false;
    }

    VkCommandBufferAllocateInfo colorCmdAlloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
                                                  m_cmd_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2 };
    if (vkAllocateCommandBuffers(m_device, &colorCmdAlloc, m_color_cmd_buffer) != VK_SUCCESS) {
        LOG_ERROR("Failed to allocate color command buffers");
        Cleanup();
        return false;
    }

    for (int h = 0; h < MAX_HEADS; ++h) {
        VkCommandBufferAllocateInfo headCmdAlloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
                                                     m_cmd_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2 };
        if (vkAllocateCommandBuffers(m_device, &headCmdAlloc, m_heads[h].cmd_buffer) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate command buffers for head %d", h);
            Cleanup();
            return false;
        }
    }

    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
    for (int i = 0; i < 2; i++) {
        if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_color_fence[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create color fence %d", i);
            Cleanup();
            return false;
        }
    }

    for (int h = 0; h < MAX_HEADS; ++h) {
        for (int i = 0; i < 2; i++) {
            if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_heads[h].fence[i]) != VK_SUCCESS) {
                LOG_ERROR("Failed to create fence %d for head %d", i, h);
                Cleanup();
                return false;
            }
        }
    }

    m_initialized = true;
    LOG_INFO("Vulkan SPIR-V compute acceleration (Multi-Head Diff + Compression + Color) initialized successfully (%d Heads)", MAX_HEADS);
    return true;
}

void VulkanConverter::Cleanup() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

        DestroyBuffer(m_color_buf_input[0]);
        DestroyBuffer(m_color_buf_input[1]);
        DestroyBuffer(m_buf_y);
        DestroyBuffer(m_buf_u);
        DestroyBuffer(m_buf_v);

        for (int h = 0; h < MAX_HEADS; ++h) {
            auto& head = m_heads[h];
            DestroyBuffer(head.buf_input[0]);
            DestroyBuffer(head.buf_input[1]);
            DestroyBuffer(head.buf_diff_ref);
            DestroyBuffer(head.buf_diff_mask[0]);
            DestroyBuffer(head.buf_diff_mask[1]);
            DestroyBuffer(head.buf_diff_list[0]);
            DestroyBuffer(head.buf_diff_list[1]);
            DestroyBuffer(head.buf_packet_meta[0]);
            DestroyBuffer(head.buf_packet_meta[1]);
            DestroyBuffer(head.buf_packet_out[0]);
            DestroyBuffer(head.buf_packet_out[1]);
            DestroyBuffer(head.buf_indirect[0]);
            DestroyBuffer(head.buf_indirect[1]);
            head.packet_capacity = 0;
            head.in_flight = false;
            head.in_flight_slot = -1;
            head.diff_desc_set[0] = VK_NULL_HANDLE;
            head.diff_desc_set[1] = VK_NULL_HANDLE;
            head.comp_desc_set[0] = VK_NULL_HANDLE;
            head.comp_desc_set[1] = VK_NULL_HANDLE;

            for (int i = 0; i < 2; i++) {
                if (head.fence[i] != VK_NULL_HANDLE) {
                    vkDestroyFence(m_device, head.fence[i], nullptr);
                    head.fence[i] = VK_NULL_HANDLE;
                }
            }
        }

        m_desc_set[0] = VK_NULL_HANDLE;
        m_desc_set[1] = VK_NULL_HANDLE;
        for (int i = 0; i < 2; i++) {
            if (m_color_fence[i] != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, m_color_fence[i], nullptr);
                m_color_fence[i] = VK_NULL_HANDLE;
            }
        }

        if (m_cmd_pool != VK_NULL_HANDLE) { vkDestroyCommandPool(m_device, m_cmd_pool, nullptr); m_cmd_pool = VK_NULL_HANDLE; }
        if (m_desc_pool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(m_device, m_desc_pool, nullptr); m_desc_pool = VK_NULL_HANDLE; }

        if (m_pipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
        if (m_pipeline_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr); m_pipeline_layout = VK_NULL_HANDLE; }
        if (m_desc_layout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, m_desc_layout, nullptr); m_desc_layout = VK_NULL_HANDLE; }
        if (m_shader_module != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_shader_module, nullptr); m_shader_module = VK_NULL_HANDLE; }

        if (m_diff_pipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_diff_pipeline, nullptr); m_diff_pipeline = VK_NULL_HANDLE; }
        if (m_diff_pipeline_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_diff_pipeline_layout, nullptr); m_diff_pipeline_layout = VK_NULL_HANDLE; }
        if (m_diff_desc_layout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, m_diff_desc_layout, nullptr); m_diff_desc_layout = VK_NULL_HANDLE; }
        if (m_diff_shader_module != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_diff_shader_module, nullptr); m_diff_shader_module = VK_NULL_HANDLE; }

        if (m_comp_pipeline != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_comp_pipeline, nullptr); m_comp_pipeline = VK_NULL_HANDLE; }
        if (m_comp_pipeline_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_comp_pipeline_layout, nullptr); m_comp_pipeline_layout = VK_NULL_HANDLE; }
        if (m_comp_desc_layout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, m_comp_desc_layout, nullptr); m_comp_desc_layout = VK_NULL_HANDLE; }
        if (m_comp_shader_module != VK_NULL_HANDLE) { vkDestroyShaderModule(m_device, m_comp_shader_module, nullptr); m_comp_shader_module = VK_NULL_HANDLE; }

        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    m_initialized = false;
}

uint8_t* VulkanConverter::GetMappedInputBuffer(int head_id, int buffer_id, size_t required_bytes) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return nullptr;
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    if (buffer_id < 0 || buffer_id >= 2) buffer_id = 0;
    if (required_bytes > 0) {
        if (!EnsureDiffBuffers(head_id, required_bytes, static_cast<uint32_t>((required_bytes / 4 + 31) / 32))) return nullptr;
    }
    return static_cast<uint8_t*>(m_heads[head_id].buf_input[buffer_id].mapped);
}

void VulkanConverter::GetMappedOutputPlanes(uint8_t*& y_plane, uint8_t*& u_plane, uint8_t*& v_plane) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    y_plane = static_cast<uint8_t*>(m_buf_y.mapped);
    u_plane = static_cast<uint8_t*>(m_buf_u.mapped);
    v_plane = static_cast<uint8_t*>(m_buf_v.mapped);
}

bool VulkanConverter::DispatchCompute(int width, int height, int src_stride, int dst_y_stride, int dst_uv_stride) {
    ColorPushConstants pc = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<uint32_t>(src_stride),
        static_cast<uint32_t>(dst_y_stride),
        static_cast<uint32_t>(dst_uv_stride)
    };

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr };
    if (vkBeginCommandBuffer(m_color_cmd_buffer[0], &beginInfo) != VK_SUCCESS) return false;

    vkCmdBindPipeline(m_color_cmd_buffer[0], VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(m_color_cmd_buffer[0], VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_desc_set[0], 0, nullptr);
    vkCmdPushConstants(m_color_cmd_buffer[0], m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    uint32_t group_x = ((width + 1) / 2 + 15) / 16;
    uint32_t group_y = ((height + 1) / 2 + 15) / 16;
    vkCmdDispatch(m_color_cmd_buffer[0], group_x, group_y, 1);

    if (vkEndCommandBuffer(m_color_cmd_buffer[0]) != VK_SUCCESS) return false;

    vkResetFences(m_device, 1, &m_color_fence[0]);
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &m_color_cmd_buffer[0], 0, nullptr };
    if (vkQueueSubmit(m_compute_queue, 1, &submitInfo, m_color_fence[0]) != VK_SUCCESS) return false;
    if (vkWaitForFences(m_device, 1, &m_color_fence[0], VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;

    return true;
}

bool VulkanConverter::ConvertRgb32ToYuv420(
    const uint8_t* src_argb,
    int src_stride,
    int width,
    int height,
    uint8_t* dst_y,
    uint8_t* dst_u,
    uint8_t* dst_v,
    int dst_y_stride,
    int dst_uv_stride
) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return false;

    size_t src_size = static_cast<size_t>(src_stride) * height;
    size_t y_size   = static_cast<size_t>(dst_y_stride) * height;
    size_t uv_size  = static_cast<size_t>(dst_uv_stride) * ((height + 1) / 2);

    if (!EnsureBuffers(src_size, y_size, uv_size)) return false;

    std::memcpy(m_color_buf_input[0].mapped, src_argb, src_size);

    if (!DispatchCompute(width, height, src_stride, dst_y_stride, dst_uv_stride)) {
        return false;
    }

    std::memcpy(dst_y, m_buf_y.mapped, y_size);
    std::memcpy(dst_u, m_buf_u.mapped, uv_size);
    std::memcpy(dst_v, m_buf_v.mapped, uv_size);

    return true;
}

bool VulkanConverter::DispatchTileDifferencing(
    int width,
    int height,
    int stride_words,
    int tile_size,
    bool update_reference,
    uint32_t start_col,
    uint32_t start_row,
    uint32_t num_cols,
    uint32_t num_rows,
    int head_id
) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return false;
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    auto& head = m_heads[head_id];

    uint32_t grid_cols = (width + tile_size - 1) / tile_size;
    uint32_t grid_rows = (height + tile_size - 1) / tile_size;
    uint32_t total_tiles = grid_cols * grid_rows;
    size_t fb_size = static_cast<size_t>(stride_words) * 4 * height;

    if (!EnsureDiffBuffers(head_id, fb_size, total_tiles)) return false;

    if (num_cols == 0) num_cols = (start_col < grid_cols) ? (grid_cols - start_col) : 1;
    if (num_rows == 0) num_rows = (start_row < grid_rows) ? (grid_rows - start_row) : 1;

    DiffPushConstants pc = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<uint32_t>(stride_words),
        grid_cols,
        grid_rows,
        static_cast<uint32_t>(tile_size),
        update_reference ? 1u : 0u,
        start_col,
        start_row
    };

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr };
    if (vkBeginCommandBuffer(head.cmd_buffer[0], &beginInfo) != VK_SUCCESS) return false;

    // Reset mask and counter on GPU
    vkCmdFillBuffer(head.cmd_buffer[0], head.buf_diff_mask[0].buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(head.cmd_buffer[0], head.buf_diff_list[0].buffer, 0, sizeof(uint32_t), 0);

    VkMemoryBarrier mb_clear = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    vkCmdPipelineBarrier(head.cmd_buffer[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb_clear, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(head.cmd_buffer[0], VK_PIPELINE_BIND_POINT_COMPUTE, m_diff_pipeline);
    vkCmdBindDescriptorSets(head.cmd_buffer[0], VK_PIPELINE_BIND_POINT_COMPUTE, m_diff_pipeline_layout, 0, 1, &head.diff_desc_set[0], 0, nullptr);
    vkCmdPushConstants(head.cmd_buffer[0], m_diff_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    vkCmdDispatch(head.cmd_buffer[0], num_cols, num_rows, 1);

    VkMemoryBarrier mb_host = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_SHADER_WRITE_BIT,
                                VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(head.cmd_buffer[0], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb_host, 0, nullptr, 0, nullptr);

    if (vkEndCommandBuffer(head.cmd_buffer[0]) != VK_SUCCESS) return false;

    vkResetFences(m_device, 1, &head.fence[0]);
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &head.cmd_buffer[0], 0, nullptr };
    if (vkQueueSubmit(m_compute_queue, 1, &submitInfo, head.fence[0]) != VK_SUCCESS) return false;
    if (vkWaitForFences(m_device, 1, &head.fence[0], VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;

    return true;
}

uint32_t VulkanConverter::GetDirtyTileCount(int head_id) const {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    if (!m_heads[head_id].buf_diff_list[0].mapped) return 0;
    return *static_cast<const uint32_t*>(m_heads[head_id].buf_diff_list[0].mapped);
}

const uint32_t* VulkanConverter::GetDirtyTileIndices(int head_id) const {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    if (!m_heads[head_id].buf_diff_list[0].mapped) return nullptr;
    return static_cast<const uint32_t*>(m_heads[head_id].buf_diff_list[0].mapped) + 1;
}

const uint32_t* VulkanConverter::GetDirtyBitmask(int head_id) const {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    if (!m_heads[head_id].buf_diff_mask[0].mapped) return nullptr;
    return static_cast<const uint32_t*>(m_heads[head_id].buf_diff_mask[0].mapped);
}

bool VulkanConverter::DetectDirtyTiles(
    const uint8_t* curr_fb,
    int fb_stride,
    int width,
    int height,
    int tile_size,
    std::vector<TileCoordinate>& out_dirty_tiles,
    int head_id
) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return false;
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    auto& head = m_heads[head_id];

    uint32_t grid_cols = (width + tile_size - 1) / tile_size;
    uint32_t grid_rows = (height + tile_size - 1) / tile_size;
    uint32_t total_tiles = grid_cols * grid_rows;
    size_t fb_size = static_cast<size_t>(fb_stride) * height;

    if (!EnsureDiffBuffers(head_id, fb_size, total_tiles)) return false;

    if (curr_fb && curr_fb != head.buf_input[0].mapped && curr_fb != head.buf_input[1].mapped) {
        std::memcpy(head.buf_input[0].mapped, curr_fb, fb_size);
    }

    int stride_words = fb_stride / 4;
    if (!DispatchTileDifferencing(width, height, stride_words, tile_size, true, 0, 0, 0, 0, head_id)) {
        return false;
    }

    uint32_t count = GetDirtyTileCount(head_id);
    out_dirty_tiles.clear();
    if (count == 0) return true;

    out_dirty_tiles.reserve(count);
    const uint32_t* indices = GetDirtyTileIndices(head_id);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t tile_id = indices[i];
        uint32_t col = tile_id % grid_cols;
        uint32_t row = tile_id / grid_cols;

        TileCoordinate t;
        t.x = static_cast<uint16_t>(col * tile_size);
        t.y = static_cast<uint16_t>(row * tile_size);
        t.width  = static_cast<uint16_t>(std::min(static_cast<int>(tile_size), width - static_cast<int>(t.x)));
        t.height = static_cast<uint16_t>(std::min(static_cast<int>(tile_size), height - static_cast<int>(t.y)));
        out_dirty_tiles.push_back(t);
    }

    return true;
}

bool VulkanConverter::FilterDirtyTilesGpu(
    const uint8_t* curr_fb,
    int fb_stride,
    int width,
    int height,
    int tile_size,
    const std::vector<TileCoordinate>& candidate_tiles,
    std::vector<TileCoordinate>& out_changed_tiles,
    int head_id
) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return false;
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    auto& head = m_heads[head_id];

    uint32_t grid_cols = (width + tile_size - 1) / tile_size;
    uint32_t grid_rows = (height + tile_size - 1) / tile_size;
    uint32_t total_tiles = grid_cols * grid_rows;
    size_t fb_size = static_cast<size_t>(fb_stride) * height;

    if (!EnsureDiffBuffers(head_id, fb_size, total_tiles)) return false;

    if (curr_fb && curr_fb != head.buf_input[0].mapped && curr_fb != head.buf_input[1].mapped) {
        std::memcpy(head.buf_input[0].mapped, curr_fb, fb_size);
    }

    int stride_words = fb_stride / 4;
    if (!DispatchTileDifferencing(width, height, stride_words, tile_size, true, 0, 0, 0, 0, head_id)) {
        return false;
    }

    out_changed_tiles.clear();
    uint32_t count = GetDirtyTileCount(head_id);
    if (count == 0) return true;

    const uint32_t* bitmask = GetDirtyBitmask(head_id);
    out_changed_tiles.reserve(candidate_tiles.size());

    for (const auto& tile : candidate_tiles) {
        uint32_t col = tile.x / tile_size;
        uint32_t row = tile.y / tile_size;
        uint32_t tile_id = row * grid_cols + col;

        if (tile_id < total_tiles) {
            if ((bitmask[tile_id / 32] >> (tile_id % 32)) & 1u) {
                out_changed_tiles.push_back(tile);
            }
        } else {
            out_changed_tiles.push_back(tile);
        }
    }

    return true;
}

void VulkanConverter::UpdateCompDescriptors(int head_id) {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    auto& head = m_heads[head_id];

    if (head.comp_desc_set[0] == VK_NULL_HANDLE ||
        head.buf_input[0].buffer == VK_NULL_HANDLE ||
        head.buf_input[1].buffer == VK_NULL_HANDLE ||
        head.buf_diff_list[0].buffer == VK_NULL_HANDLE ||
        head.buf_diff_list[1].buffer == VK_NULL_HANDLE ||
        head.buf_packet_meta[0].buffer == VK_NULL_HANDLE ||
        head.buf_packet_meta[1].buffer == VK_NULL_HANDLE ||
        head.buf_packet_out[0].buffer == VK_NULL_HANDLE ||
        head.buf_packet_out[1].buffer == VK_NULL_HANDLE) {
        return;
    }

    for (int b = 0; b < 2; b++) {
        VkDescriptorBufferInfo dbi[4] = {
            { head.buf_input[b].buffer,       0, head.buf_input[b].size },
            { head.buf_diff_list[b].buffer,   0, head.buf_diff_list[b].size },
            { head.buf_packet_meta[b].buffer, 0, head.buf_packet_meta[b].size },
            { head.buf_packet_out[b].buffer,  0, head.buf_packet_out[b].size }
        };

        VkWriteDescriptorSet writes[4] = {};
        for (int i = 0; i < 4; i++) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = head.comp_desc_set[b];
            writes[i].dstBinding = i;
            writes[i].dstArrayElement = 0;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo = &dbi[i];
        }
        vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
    }
}

bool VulkanConverter::EnsurePacketBuffers(int head_id, size_t max_capacity) {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    auto& head = m_heads[head_id];
    bool updated = false;

    for (int b = 0; b < 2; b++) {
        // Allocate PacketMeta buffer (64 bytes for uint32 atomic counter)
        if (head.buf_packet_meta[b].buffer == VK_NULL_HANDLE) {
            size_t meta_size = 64;
            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, meta_size,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
            if (vkCreateBuffer(m_device, &bci, nullptr, &head.buf_packet_meta[b].buffer) != VK_SUCCESS) return false;

            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(m_device, head.buf_packet_meta[b].buffer, &req);
            uint32_t memType = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memType };
            if (vkAllocateMemory(m_device, &ai, nullptr, &head.buf_packet_meta[b].memory) != VK_SUCCESS) return false;
            vkBindBufferMemory(m_device, head.buf_packet_meta[b].buffer, head.buf_packet_meta[b].memory, 0);
            vkMapMemory(m_device, head.buf_packet_meta[b].memory, 0, meta_size, 0, &head.buf_packet_meta[b].mapped);
            head.buf_packet_meta[b].size = meta_size;
            updated = true;
        }

        // Allocate PacketOutput buffer (Host Cached for max read throughput)
        if (head.buf_packet_out[b].size < max_capacity || head.buf_packet_out[b].buffer == VK_NULL_HANDLE) {
            size_t alloc_size = std::max(max_capacity, size_t(36 * 1024 * 1024)); // Default to 36MB for 4K
            DestroyBuffer(head.buf_packet_out[b]);

            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, alloc_size,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
            if (vkCreateBuffer(m_device, &bci, nullptr, &head.buf_packet_out[b].buffer) != VK_SUCCESS) return false;

            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(m_device, head.buf_packet_out[b].buffer, &req);
            uint32_t memType = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memType };
            if (vkAllocateMemory(m_device, &ai, nullptr, &head.buf_packet_out[b].memory) != VK_SUCCESS) return false;
            vkBindBufferMemory(m_device, head.buf_packet_out[b].buffer, head.buf_packet_out[b].memory, 0);
            vkMapMemory(m_device, head.buf_packet_out[b].memory, 0, alloc_size, 0, &head.buf_packet_out[b].mapped);
            head.buf_packet_out[b].size = alloc_size;
            head.packet_capacity = alloc_size;
            updated = true;
        }

        // Allocate Indirect Dispatch buffer (VkDispatchIndirectCommand { uint32_t x, y, z; })
        if (head.buf_indirect[b].buffer == VK_NULL_HANDLE) {
            size_t ind_size = 64;
            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, ind_size,
                                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
            if (vkCreateBuffer(m_device, &bci, nullptr, &head.buf_indirect[b].buffer) != VK_SUCCESS) return false;

            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(m_device, head.buf_indirect[b].buffer, &req);
            uint32_t memType = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memType };
            if (vkAllocateMemory(m_device, &ai, nullptr, &head.buf_indirect[b].memory) != VK_SUCCESS) return false;
            vkBindBufferMemory(m_device, head.buf_indirect[b].buffer, head.buf_indirect[b].memory, 0);
            vkMapMemory(m_device, head.buf_indirect[b].memory, 0, ind_size, 0, &head.buf_indirect[b].mapped);
            head.buf_indirect[b].size = ind_size;

            uint32_t* ind_ptr = static_cast<uint32_t*>(head.buf_indirect[b].mapped);
            ind_ptr[0] = 0; // x = 0
            ind_ptr[1] = 1; // y = 1
            ind_ptr[2] = 1; // z = 1
            updated = true;
        }
    }

    if (updated) {
        UpdateCompDescriptors(head_id);
    }
    return true;
}

const uint8_t* VulkanConverter::GetMappedPacketBuffer(int head_id) const {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    return static_cast<const uint8_t*>(m_heads[head_id].buf_packet_out[0].mapped);
}

std::atomic<bool>* VulkanConverter::GetPacketCompletionFlag(int head_id, int slot) {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    if (slot < 0 || slot >= 2) return nullptr;
    return &m_heads[head_id].packet_usb_in_flight[slot];
}

int VulkanConverter::GetCompletedSlot(int head_id) const {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    return m_heads[head_id].last_completed_slot;
}

const uint8_t* VulkanConverter::EncodeFramePacketsGpu(
    const uint8_t* curr_fb,
    int fb_stride,
    int width,
    int height,
    int tile_size,
    uint32_t frame_index,
    uint32_t& out_total_packet_bytes,
    const std::vector<DirtyRect>& dirty_rects,
    bool pipelined,
    uint8_t head_id
) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return nullptr;
    if (head_id >= MAX_HEADS) head_id = 0;
    auto& head = m_heads[head_id];

    uint32_t grid_cols = (width + tile_size - 1) / tile_size;
    uint32_t grid_rows = (height + tile_size - 1) / tile_size;
    uint32_t total_tiles = grid_cols * grid_rows;
    size_t fb_size = static_cast<size_t>(fb_stride) * height;

    if (!EnsureDiffBuffers(head_id, fb_size, total_tiles)) return nullptr;

    size_t worst_case_bytes = 16 + static_cast<size_t>(total_tiles) * (14 + tile_size * tile_size * 4);
    if (!EnsurePacketBuffers(head_id, worst_case_bytes)) return nullptr;

    int slot = (curr_fb == head.buf_input[1].mapped) ? 1 : (curr_fb == head.buf_input[0].mapped) ? 0 : (frame_index % 2);

    const uint8_t* ret_packet = nullptr;
    out_total_packet_bytes = 0;

    // In pipelined mode, retire the previously submitted frame from the other slot for THIS head
    if (pipelined && head.in_flight) {
        int comp_slot = head.in_flight_slot;
        if (vkWaitForFences(m_device, 1, &head.fence[comp_slot], VK_TRUE, UINT64_MAX) == VK_SUCCESS) {
            VkMappedMemoryRange ranges[3] = {};
            ranges[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            ranges[0].memory = head.buf_diff_list[comp_slot].memory;
            ranges[0].offset = 0;
            ranges[0].size = VK_WHOLE_SIZE;
            ranges[1].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            ranges[1].memory = head.buf_packet_meta[comp_slot].memory;
            ranges[1].offset = 0;
            ranges[1].size = VK_WHOLE_SIZE;
            ranges[2].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            ranges[2].memory = head.buf_packet_out[comp_slot].memory;
            ranges[2].offset = 0;
            ranges[2].size = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(m_device, 3, ranges);

            uint32_t comp_dirty_count = *reinterpret_cast<volatile uint32_t*>(head.buf_diff_list[comp_slot].mapped);
            uint32_t comp_packet_bytes = *reinterpret_cast<volatile uint32_t*>(head.buf_packet_meta[comp_slot].mapped);

            if (comp_dirty_count > 0 && comp_packet_bytes > sizeof(protocol::FrameSectionHeader)) {
                protocol::FrameSectionHeader frame_hdr = {};
                frame_hdr.magic         = protocol::DL_FRAME_MAGIC;
                frame_hdr.frame_index   = head.in_flight_frame_index;
                frame_hdr.screen_width  = static_cast<uint16_t>(head.in_flight_width);
                frame_hdr.screen_height = static_cast<uint16_t>(head.in_flight_height);
                frame_hdr.tile_count    = static_cast<uint16_t>(comp_dirty_count);
                frame_hdr.head_id       = head_id;
                frame_hdr.reserved      = 0;

                std::memcpy(head.buf_packet_out[comp_slot].mapped, &frame_hdr, sizeof(frame_hdr));

                VkMappedMemoryRange flush_range = {};
                flush_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                flush_range.memory = head.buf_packet_out[comp_slot].memory;
                flush_range.offset = 0;
                flush_range.size = sizeof(frame_hdr);
                vkFlushMappedMemoryRanges(m_device, 1, &flush_range);

                ret_packet = static_cast<const uint8_t*>(head.buf_packet_out[comp_slot].mapped);
                out_total_packet_bytes = comp_packet_bytes;
                head.last_completed_slot = comp_slot;
            }
        }
        head.in_flight = false;
    }

    uint32_t start_col = 0;
    uint32_t start_row = 0;
    uint32_t num_cols = grid_cols;
    uint32_t num_rows = grid_rows;
    bool has_bounds = false;
    bool use_multi_dispatch = false;

    struct TileRange {
        int c1, c2; // inclusive [c1, c2]
        int r1, r2; // inclusive [r1, r2]
    };
    std::vector<TileRange> disjoint_boxes;

    if (!dirty_rects.empty()) {
        std::vector<TileRange> raw_boxes;
        raw_boxes.reserve(dirty_rects.size());

        for (const auto& r : dirty_rects) {
            if (r.x2 <= r.x1 || r.y2 <= r.y1) continue;
            int c1 = std::clamp(r.x1 / tile_size, 0, static_cast<int>(grid_cols) - 1);
            int c2 = std::clamp((r.x2 - 1) / tile_size, 0, static_cast<int>(grid_cols) - 1);
            int r1 = std::clamp(r.y1 / tile_size, 0, static_cast<int>(grid_rows) - 1);
            int r2 = std::clamp((r.y2 - 1) / tile_size, 0, static_cast<int>(grid_rows) - 1);
            if (c1 <= c2 && r1 <= r2) {
                raw_boxes.push_back({c1, c2, r1, r2});
            }
        }

        // Iteratively merge touching or overlapping tile boxes until mutually disjoint
        for (const auto& box : raw_boxes) {
            TileRange current = box;
            bool merged = true;
            while (merged) {
                merged = false;
                for (size_t i = 0; i < disjoint_boxes.size(); ++i) {
                    const auto& b = disjoint_boxes[i];
                    // Overlapping or touching boxes
                    if (!(current.c2 < b.c1 || current.c1 > b.c2 || current.r2 < b.r1 || current.r1 > b.r2)) {
                        current.c1 = std::min(current.c1, b.c1);
                        current.c2 = std::max(current.c2, b.c2);
                        current.r1 = std::min(current.r1, b.r1);
                        current.r2 = std::max(current.r2, b.r2);
                        disjoint_boxes.erase(disjoint_boxes.begin() + i);
                        merged = true;
                        break;
                    }
                }
            }
            disjoint_boxes.push_back(current);
        }

        if (!disjoint_boxes.empty()) {
            uint32_t sum_disjoint_tiles = 0;
            int union_c1 = static_cast<int>(grid_cols) - 1, union_c2 = 0;
            int union_r1 = static_cast<int>(grid_rows) - 1, union_r2 = 0;

            for (const auto& b : disjoint_boxes) {
                sum_disjoint_tiles += static_cast<uint32_t>((b.c2 - b.c1 + 1) * (b.r2 - b.r1 + 1));
                union_c1 = std::min(union_c1, b.c1);
                union_c2 = std::max(union_c2, b.c2);
                union_r1 = std::min(union_r1, b.r1);
                union_r2 = std::max(union_r2, b.r2);
            }

            uint32_t union_tiles = static_cast<uint32_t>((union_c2 - union_c1 + 1) * (union_r2 - union_r1 + 1));

            // Use multi-dispatch when multiple disjoint boxes exist and union bounding box adds >25% wasted tiles
            if (disjoint_boxes.size() > 1 && union_tiles > static_cast<uint32_t>(sum_disjoint_tiles * 1.25f)) {
                use_multi_dispatch = true;
                has_bounds = true;
            } else {
                start_col = static_cast<uint32_t>(union_c1);
                start_row = static_cast<uint32_t>(union_r1);
                num_cols  = static_cast<uint32_t>(union_c2 - union_c1 + 1);
                num_rows  = static_cast<uint32_t>(union_r2 - union_r1 + 1);
                has_bounds = true;
            }
        }
    }

    // Wait on slot fence before recording to guarantee GPU is done with previous frame on this slot
    vkWaitForFences(m_device, 1, &head.fence[slot], VK_TRUE, UINT64_MAX);
    vkResetFences(m_device, 1, &head.fence[slot]);

    // Ensure any in-flight asynchronous zero-copy USB DMA transfer on this slot has completed
    while (head.packet_usb_in_flight[slot].load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    if (curr_fb != head.buf_input[slot].mapped && curr_fb) {
        if (use_multi_dispatch) {
            // Coalesce row spans across all disjoint boxes
            std::vector<std::pair<int, int>> row_spans;
            row_spans.reserve(disjoint_boxes.size());
            for (const auto& b : disjoint_boxes) {
                row_spans.push_back({b.r1, b.r2});
            }
            std::sort(row_spans.begin(), row_spans.end());
            std::vector<std::pair<int, int>> merged_row_spans;
            for (const auto& span : row_spans) {
                if (merged_row_spans.empty() || merged_row_spans.back().second < span.first) {
                    merged_row_spans.push_back(span);
                } else {
                    merged_row_spans.back().second = std::max(merged_row_spans.back().second, span.second);
                }
            }
            for (const auto& span : merged_row_spans) {
                uint32_t start_y = static_cast<uint32_t>(span.first * tile_size);
                uint32_t end_y = std::min(static_cast<uint32_t>((span.second + 1) * tile_size), static_cast<uint32_t>(height));
                size_t row_offset = static_cast<size_t>(start_y) * fb_stride;
                size_t copy_bytes = static_cast<size_t>(end_y - start_y) * fb_stride;
                std::memcpy(static_cast<uint8_t*>(head.buf_input[slot].mapped) + row_offset,
                            curr_fb + row_offset, copy_bytes);
            }
        } else if (has_bounds) {
            uint32_t start_y = start_row * tile_size;
            uint32_t end_y = std::min((start_row + num_rows) * tile_size, static_cast<uint32_t>(height));
            size_t row_offset = static_cast<size_t>(start_y) * fb_stride;
            size_t copy_bytes = static_cast<size_t>(end_y - start_y) * fb_stride;
            std::memcpy(static_cast<uint8_t*>(head.buf_input[slot].mapped) + row_offset,
                        curr_fb + row_offset, copy_bytes);
        } else {
            std::memcpy(head.buf_input[slot].mapped, curr_fb, fb_size);
        }
    }

    int stride_words = fb_stride / 4;

    // Reset command buffer for slot
    vkResetCommandBuffer(head.cmd_buffer[slot], 0);
    VkCommandBufferBeginInfo beginInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr
    };
    if (vkBeginCommandBuffer(head.cmd_buffer[slot], &beginInfo) != VK_SUCCESS) return nullptr;

    // 1. Clear GPU state buffers on the hardware timeline
    vkCmdFillBuffer(head.cmd_buffer[slot], head.buf_diff_mask[slot].buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(head.cmd_buffer[slot], head.buf_diff_list[slot].buffer, 0, sizeof(uint32_t), 0);
    vkCmdFillBuffer(head.cmd_buffer[slot], head.buf_packet_meta[slot].buffer, 0, sizeof(uint32_t), 16);
    vkCmdFillBuffer(head.cmd_buffer[slot], head.buf_indirect[slot].buffer, sizeof(uint32_t), sizeof(uint32_t) * 2, 1);

    VkMemoryBarrier mb_clear = {
        VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
    };
    vkCmdPipelineBarrier(head.cmd_buffer[slot],
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb_clear, 0, nullptr, 0, nullptr);

    // 2. Dispatch Differencing compute kernel restricted to damaged regions
    vkCmdBindPipeline(head.cmd_buffer[slot], VK_PIPELINE_BIND_POINT_COMPUTE, m_diff_pipeline);
    vkCmdBindDescriptorSets(head.cmd_buffer[slot], VK_PIPELINE_BIND_POINT_COMPUTE, m_diff_pipeline_layout, 0, 1, &head.diff_desc_set[slot], 0, nullptr);

    if (use_multi_dispatch) {
        for (const auto& b : disjoint_boxes) {
            DiffPushConstants diff_pc = {
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height),
                static_cast<uint32_t>(stride_words),
                grid_cols,
                grid_rows,
                static_cast<uint32_t>(tile_size),
                1u, // update reference frame
                static_cast<uint32_t>(b.c1),
                static_cast<uint32_t>(b.r1)
            };
            uint32_t b_cols = static_cast<uint32_t>(b.c2 - b.c1 + 1);
            uint32_t b_rows = static_cast<uint32_t>(b.r2 - b.r1 + 1);
            vkCmdPushConstants(head.cmd_buffer[slot], m_diff_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(diff_pc), &diff_pc);
            vkCmdDispatch(head.cmd_buffer[slot], b_cols, b_rows, 1);
        }
    } else {
        DiffPushConstants diff_pc = {
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
            static_cast<uint32_t>(stride_words),
            grid_cols,
            grid_rows,
            static_cast<uint32_t>(tile_size),
            1u, // update reference frame
            start_col,
            start_row
        };
        vkCmdPushConstants(head.cmd_buffer[slot], m_diff_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(diff_pc), &diff_pc);
        vkCmdDispatch(head.cmd_buffer[slot], num_cols, num_rows, 1);
    }

    // 3. Pipeline Barrier: Differencing -> Copy dirty_count to indirect buffer
    VkBufferMemoryBarrier b_copy = {};
    b_copy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b_copy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b_copy.buffer = head.buf_diff_list[slot].buffer;
    b_copy.offset = 0;
    b_copy.size = sizeof(uint32_t);

    vkCmdPipelineBarrier(head.cmd_buffer[slot],
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &b_copy, 0, nullptr);

    VkBufferCopy copyRegion = { 0, 0, sizeof(uint32_t) };
    vkCmdCopyBuffer(head.cmd_buffer[slot], head.buf_diff_list[slot].buffer, head.buf_indirect[slot].buffer, 1, &copyRegion);

    // 4. Pipeline Barrier: Transfer & Diff write -> Indirect dispatch read & Compression read
    VkBufferMemoryBarrier b_after[2] = {};
    b_after[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b_after[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b_after[0].dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    b_after[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b_after[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b_after[0].buffer = head.buf_indirect[slot].buffer;
    b_after[0].offset = 0;
    b_after[0].size = sizeof(uint32_t) * 3;

    b_after[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b_after[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b_after[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b_after[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b_after[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b_after[1].buffer = head.buf_diff_list[slot].buffer;
    b_after[1].offset = 0;
    b_after[1].size = VK_WHOLE_SIZE;

    vkCmdPipelineBarrier(head.cmd_buffer[slot],
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 2, b_after, 0, nullptr);

    // 5. Indirect Dispatch Compression Kernel directly from GPU buffer
    CompPushConstants comp_pc = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<uint32_t>(stride_words),
        grid_cols,
        grid_rows,
        static_cast<uint32_t>(tile_size),
        0u
    };
    vkCmdBindPipeline(head.cmd_buffer[slot], VK_PIPELINE_BIND_POINT_COMPUTE, m_comp_pipeline);
    vkCmdBindDescriptorSets(head.cmd_buffer[slot], VK_PIPELINE_BIND_POINT_COMPUTE, m_comp_pipeline_layout, 0, 1, &head.comp_desc_set[slot], 0, nullptr);
    vkCmdPushConstants(head.cmd_buffer[slot], m_comp_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(comp_pc), &comp_pc);

    vkCmdDispatchIndirect(head.cmd_buffer[slot], head.buf_indirect[slot].buffer, 0);

    // 6. Barrier: Shader write -> Host read
    VkMemoryBarrier mb_host = {
        VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_HOST_READ_BIT
    };
    vkCmdPipelineBarrier(head.cmd_buffer[slot],
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb_host, 0, nullptr, 0, nullptr);

    if (vkEndCommandBuffer(head.cmd_buffer[slot]) != VK_SUCCESS) return nullptr;

    VkSubmitInfo submitInfo = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr,
        0, nullptr, nullptr,
        1, &head.cmd_buffer[slot],
        0, nullptr
    };
    if (vkQueueSubmit(m_compute_queue, 1, &submitInfo, head.fence[slot]) != VK_SUCCESS) return nullptr;

    if (pipelined) {
        head.in_flight = true;
        head.in_flight_slot = slot;
        head.in_flight_frame_index = frame_index;
        head.in_flight_width = width;
        head.in_flight_height = height;
        return ret_packet;
    }

    // Synchronous mode (pipelined == false): wait immediately for THIS slot
    if (vkWaitForFences(m_device, 1, &head.fence[slot], VK_TRUE, UINT64_MAX) != VK_SUCCESS) return nullptr;

    // Invalidate mapped memory to guarantee CPU cache coherency
    VkMappedMemoryRange ranges[3] = {};
    ranges[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    ranges[0].memory = head.buf_diff_list[slot].memory;
    ranges[0].offset = 0;
    ranges[0].size = VK_WHOLE_SIZE;
    ranges[1].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    ranges[1].memory = head.buf_packet_meta[slot].memory;
    ranges[1].offset = 0;
    ranges[1].size = VK_WHOLE_SIZE;
    ranges[2].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    ranges[2].memory = head.buf_packet_out[slot].memory;
    ranges[2].offset = 0;
    ranges[2].size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(m_device, 3, ranges);

    uint32_t dirty_count = *reinterpret_cast<volatile uint32_t*>(head.buf_diff_list[slot].mapped);
    if (dirty_count == 0) {
        out_total_packet_bytes = 0;
        return nullptr;
    }

    out_total_packet_bytes = *reinterpret_cast<volatile uint32_t*>(head.buf_packet_meta[slot].mapped);

    // Write 16-byte FrameSectionHeader at start of packet buffer
    protocol::FrameSectionHeader frame_hdr = {};
    frame_hdr.magic         = protocol::DL_FRAME_MAGIC;
    frame_hdr.frame_index   = frame_index;
    frame_hdr.screen_width  = static_cast<uint16_t>(width);
    frame_hdr.screen_height = static_cast<uint16_t>(height);
    frame_hdr.tile_count    = static_cast<uint16_t>(dirty_count);
    frame_hdr.head_id       = head_id;
    frame_hdr.reserved      = 0;

    std::memcpy(head.buf_packet_out[slot].mapped, &frame_hdr, sizeof(frame_hdr));

    VkMappedMemoryRange flush_range = {};
    flush_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    flush_range.memory = head.buf_packet_out[slot].memory;
    flush_range.offset = 0;
    flush_range.size = sizeof(frame_hdr);
    vkFlushMappedMemoryRanges(m_device, 1, &flush_range);

    head.last_completed_slot = slot;
    return static_cast<const uint8_t*>(head.buf_packet_out[slot].mapped);
}

const uint8_t* VulkanConverter::FlushFramePacketsGpu(uint32_t& out_total_packet_bytes, uint8_t head_id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    out_total_packet_bytes = 0;
    if (!m_initialized) return nullptr;
    if (head_id >= MAX_HEADS) head_id = 0;
    auto& head = m_heads[head_id];
    if (!head.in_flight) return nullptr;

    int comp_slot = head.in_flight_slot;
    if (vkWaitForFences(m_device, 1, &head.fence[comp_slot], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        head.in_flight = false;
        return nullptr;
    }

    VkMappedMemoryRange ranges[3] = {};
    ranges[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    ranges[0].memory = head.buf_diff_list[comp_slot].memory;
    ranges[0].offset = 0;
    ranges[0].size = VK_WHOLE_SIZE;
    ranges[1].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    ranges[1].memory = head.buf_packet_meta[comp_slot].memory;
    ranges[1].offset = 0;
    ranges[1].size = VK_WHOLE_SIZE;
    ranges[2].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    ranges[2].memory = head.buf_packet_out[comp_slot].memory;
    ranges[2].offset = 0;
    ranges[2].size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(m_device, 3, ranges);

    uint32_t comp_dirty_count = *reinterpret_cast<volatile uint32_t*>(head.buf_diff_list[comp_slot].mapped);
    uint32_t comp_packet_bytes = *reinterpret_cast<volatile uint32_t*>(head.buf_packet_meta[comp_slot].mapped);

    const uint8_t* ret_packet = nullptr;
    if (comp_dirty_count > 0 && comp_packet_bytes > sizeof(protocol::FrameSectionHeader)) {
        protocol::FrameSectionHeader frame_hdr = {};
        frame_hdr.magic         = protocol::DL_FRAME_MAGIC;
        frame_hdr.frame_index   = head.in_flight_frame_index;
        frame_hdr.screen_width  = static_cast<uint16_t>(head.in_flight_width);
        frame_hdr.screen_height = static_cast<uint16_t>(head.in_flight_height);
        frame_hdr.tile_count    = static_cast<uint16_t>(comp_dirty_count);
        frame_hdr.head_id       = head_id;
        frame_hdr.reserved      = 0;

        std::memcpy(head.buf_packet_out[comp_slot].mapped, &frame_hdr, sizeof(frame_hdr));

        VkMappedMemoryRange flush_range = {};
        flush_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        flush_range.memory = head.buf_packet_out[comp_slot].memory;
        flush_range.offset = 0;
        flush_range.size = sizeof(frame_hdr);
        vkFlushMappedMemoryRanges(m_device, 1, &flush_range);

        ret_packet = static_cast<const uint8_t*>(head.buf_packet_out[comp_slot].mapped);
        out_total_packet_bytes = comp_packet_bytes;
        head.last_completed_slot = comp_slot;
    }
    head.in_flight = false;
    return ret_packet;
}

} // namespace dl_turbo
