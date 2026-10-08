#include "vulkan_converter.hpp"
#include "common.hpp"
#include <cstring>
#include <algorithm>
#include <iostream>

namespace dl_turbo {

// Include SPIR-V binary array compiled from tile_differencing.comp
static const uint32_t s_tile_differencing_spv[] =
#include "shaders/tile_differencing_spv.inc"
;

struct DiffPushConstants {
    uint32_t width;
    uint32_t height;
    uint32_t stride_words;
    uint32_t grid_cols;
    uint32_t grid_rows;
    uint32_t tile_size;
    uint32_t update_reference;
    uint32_t start_col;
    uint32_t start_row;
};

VulkanConverter& VulkanConverter::Instance() {
    static VulkanConverter s_instance;
    return s_instance;
}

VulkanConverter::VulkanConverter() = default;

VulkanConverter::~VulkanConverter() {
    Cleanup();
}

uint32_t VulkanConverter::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physical_device, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & preferred) == preferred &&
            (memProperties.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }

    return 0;
}

void VulkanConverter::DestroyBuffer(VulkanBuffer& buf) {
    if (buf.mapped) {
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

bool VulkanConverter::EnsureDiffBuffers(int head_id, size_t fb_size, uint32_t total_tiles) {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    auto& head = m_heads[head_id];

    uint32_t mask_words = (total_tiles + 31) / 32;
    size_t mask_size = std::max(size_t(256), static_cast<size_t>(mask_words) * sizeof(uint32_t));
    size_t list_size = std::max(size_t(256), (1 + static_cast<size_t>(total_tiles)) * sizeof(uint32_t));

    // Input double-buffers
    for (int b = 0; b < 2; b++) {
        if (head.buf_input[b].size < fb_size) {
            DestroyBuffer(head.buf_input[b]);
            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, fb_size,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
            if (vkCreateBuffer(m_device, &bci, nullptr, &head.buf_input[b].buffer) != VK_SUCCESS) return false;
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(m_device, head.buf_input[b].buffer, &req);
            uint32_t memType = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memType };
            if (vkAllocateMemory(m_device, &ai, nullptr, &head.buf_input[b].memory) != VK_SUCCESS) return false;
            vkBindBufferMemory(m_device, head.buf_input[b].buffer, head.buf_input[b].memory, 0);
            vkMapMemory(m_device, head.buf_input[b].memory, 0, fb_size, 0, &head.buf_input[b].mapped);
            head.buf_input[b].size = fb_size;
        }
    }

    // Reference frame (Device-Local GPU memory)
    if (head.buf_diff_ref.size < fb_size) {
        DestroyBuffer(head.buf_diff_ref);
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, fb_size,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
        if (vkCreateBuffer(m_device, &bci, nullptr, &head.buf_diff_ref.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_device, head.buf_diff_ref.buffer, &req);
        uint32_t memType = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, memType };
        if (vkAllocateMemory(m_device, &ai, nullptr, &head.buf_diff_ref.memory) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, head.buf_diff_ref.buffer, head.buf_diff_ref.memory, 0);
        head.buf_diff_ref.size = fb_size;
    }

    // Bitmask & dirty list buffers (double-buffered)
    for (int b = 0; b < 2; b++) {
        if (head.buf_diff_mask[b].size < mask_size) {
            DestroyBuffer(head.buf_diff_mask[b]);
            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, mask_size,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
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
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
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

    // Update descriptor sets for both double-buffered slots
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

    return true;
}

bool VulkanConverter::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_initialized) return true;

    // 1. Instance
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "libevdi_turbo";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 2, 0);
    appInfo.pEngineName = "DisplayLinkTurbo";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 2, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instInfo = {};
    instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instInfo.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&instInfo, nullptr, &m_instance) != VK_SUCCESS) {
        LOG_WARN("Could not initialize Vulkan instance");
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
        LOG_WARN("Failed to create Vulkan logical device");
        Cleanup();
        return false;
    }

    vkGetDeviceQueue(m_device, m_compute_queue_family, 0, &m_compute_queue);

    // 4. Tile Differencing Pipeline
    VkShaderModuleCreateInfo diffModuleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                                sizeof(s_tile_differencing_spv), s_tile_differencing_spv };
    if (vkCreateShaderModule(m_device, &diffModuleInfo, nullptr, &m_diff_shader_module) != VK_SUCCESS) {
        LOG_ERROR("Failed to create tile differencing shader module");
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

    // 5. Descriptor Pool & Command Pool
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 }
    };
    VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 8, 1, poolSizes };
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_desc_pool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create descriptor pool");
        Cleanup();
        return false;
    }

    VkCommandPoolCreateInfo cmdPoolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, m_compute_queue_family };
    if (vkCreateCommandPool(m_device, &cmdPoolInfo, nullptr, &m_cmd_pool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create command pool");
        Cleanup();
        return false;
    }

    // 6. Allocate Per-Head Descriptor Sets, Command Buffers, and Fences
    for (int h = 0; h < MAX_HEADS; h++) {
        VkDescriptorSetLayout layouts[2] = { m_diff_desc_layout, m_diff_desc_layout };
        VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, m_desc_pool, 2, layouts };
        if (vkAllocateDescriptorSets(m_device, &allocInfo, m_heads[h].diff_desc_set) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate diff descriptor sets for head %d", h);
            Cleanup();
            return false;
        }

        VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, m_cmd_pool,
                                             VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2 };
        if (vkAllocateCommandBuffers(m_device, &cbai, m_heads[h].cmd_buffer) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate command buffers for head %d", h);
            Cleanup();
            return false;
        }

        for (int b = 0; b < 2; b++) {
            VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0 };
            if (vkCreateFence(m_device, &fci, nullptr, &m_heads[h].fence[b]) != VK_SUCCESS) {
                LOG_ERROR("Failed to create fence for head %d slot %d", h, b);
                Cleanup();
                return false;
            }
        }
    }

    m_initialized = true;
    LOG_INFO("Vulkan SPIR-V compute acceleration (Multi-Head Differencing) initialized successfully (%d Heads)", MAX_HEADS);
    return true;
}

void VulkanConverter::Cleanup() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return;

    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

        for (int h = 0; h < MAX_HEADS; h++) {
            for (int b = 0; b < 2; b++) {
                DestroyBuffer(m_heads[h].buf_input[b]);
                DestroyBuffer(m_heads[h].buf_diff_mask[b]);
                DestroyBuffer(m_heads[h].buf_diff_list[b]);
                if (m_heads[h].fence[b] != VK_NULL_HANDLE) {
                    vkDestroyFence(m_device, m_heads[h].fence[b], nullptr);
                    m_heads[h].fence[b] = VK_NULL_HANDLE;
                }
            }
            DestroyBuffer(m_heads[h].buf_diff_ref);
        }

        if (m_diff_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_diff_pipeline, nullptr);
            m_diff_pipeline = VK_NULL_HANDLE;
        }
        if (m_diff_pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, m_diff_pipeline_layout, nullptr);
            m_diff_pipeline_layout = VK_NULL_HANDLE;
        }
        if (m_diff_desc_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_diff_desc_layout, nullptr);
            m_diff_desc_layout = VK_NULL_HANDLE;
        }
        if (m_diff_shader_module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_diff_shader_module, nullptr);
            m_diff_shader_module = VK_NULL_HANDLE;
        }
        if (m_desc_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_desc_pool, nullptr);
            m_desc_pool = VK_NULL_HANDLE;
        }
        if (m_cmd_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_cmd_pool, nullptr);
            m_cmd_pool = VK_NULL_HANDLE;
        }

        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    m_initialized = false;
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

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                           VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr };
    if (vkBeginCommandBuffer(head.cmd_buffer[0], &beginInfo) != VK_SUCCESS) return false;

    // Zero out the atomic dirty count
    vkCmdFillBuffer(head.cmd_buffer[0], head.buf_diff_list[0].buffer, 0, sizeof(uint32_t), 0);

    VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    vkCmdPipelineBarrier(head.cmd_buffer[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(head.cmd_buffer[0], VK_PIPELINE_BIND_POINT_COMPUTE, m_diff_pipeline);
    vkCmdBindDescriptorSets(head.cmd_buffer[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_diff_pipeline_layout, 0, 1, &head.diff_desc_set[0], 0, nullptr);
    vkCmdPushConstants(head.cmd_buffer[0], m_diff_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(DiffPushConstants), &pc);

    // 8x8 workgroup size
    uint32_t gx = (num_cols + 7) / 8;
    uint32_t gy = (num_rows + 7) / 8;
    vkCmdDispatch(head.cmd_buffer[0], gx, gy, 1);

    if (vkEndCommandBuffer(head.cmd_buffer[0]) != VK_SUCCESS) return false;

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &head.cmd_buffer[0], 0, nullptr };
    vkResetFences(m_device, 1, &head.fence[0]);
    if (vkQueueSubmit(m_compute_queue, 1, &submitInfo, head.fence[0]) != VK_SUCCESS) return false;
    vkWaitForFences(m_device, 1, &head.fence[0], VK_TRUE, UINT64_MAX);

    return true;
}

bool VulkanConverter::DetectDirtyTiles(
    const uint8_t* curr_fb,
    int fb_stride,
    int width,
    int height,
    int tile_size,
    std::vector<TileCoordinate>& out_dirty_tiles,
    int head_id,
    int clip_x1,
    int clip_y1,
    int clip_x2,
    int clip_y2
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

    // Constrain clipping coordinates
    if (clip_x2 <= clip_x1 || clip_y2 <= clip_y1) {
        clip_x1 = 0; clip_y1 = 0;
        clip_x2 = width; clip_y2 = height;
    } else {
        clip_x1 = std::max(0, clip_x1);
        clip_y1 = std::max(0, clip_y1);
        clip_x2 = std::min(width, clip_x2);
        clip_y2 = std::min(height, clip_y2);
    }

    uint32_t start_col = clip_x1 / tile_size;
    uint32_t start_row = clip_y1 / tile_size;
    uint32_t end_col = (clip_x2 + tile_size - 1) / tile_size;
    uint32_t end_row = (clip_y2 + tile_size - 1) / tile_size;
    uint32_t num_cols = (end_col > start_col) ? (end_col - start_col) : 1;
    uint32_t num_rows = (end_row > start_row) ? (end_row - start_row) : 1;

    // Sub-region memory copy: only copy the rows within [clip_y1, clip_y2)
    if (curr_fb && curr_fb != head.buf_input[0].mapped && curr_fb != head.buf_input[1].mapped) {
        if (clip_x1 == 0 && clip_x2 == width) {
            size_t copy_offset = static_cast<size_t>(clip_y1) * fb_stride;
            size_t copy_bytes = static_cast<size_t>(clip_y2 - clip_y1) * fb_stride;
            std::memcpy(static_cast<uint8_t*>(head.buf_input[0].mapped) + copy_offset,
                        curr_fb + copy_offset,
                        copy_bytes);
        } else {
            size_t row_bytes = static_cast<size_t>(clip_x2 - clip_x1) * 4;
            for (int y = clip_y1; y < clip_y2; ++y) {
                size_t offset = static_cast<size_t>(y) * fb_stride + (clip_x1 * 4);
                std::memcpy(static_cast<uint8_t*>(head.buf_input[0].mapped) + offset,
                            curr_fb + offset,
                            row_bytes);
            }
        }
    }

    int stride_words = fb_stride / 4;
    if (!DispatchTileDifferencing(width, height, stride_words, tile_size, true,
                                 start_col, start_row, num_cols, num_rows, head_id)) {
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

uint32_t VulkanConverter::GetDirtyTileCount(int head_id) const {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    const auto& head = m_heads[head_id];
    if (!head.buf_diff_list[0].mapped) return 0;
    return *reinterpret_cast<const uint32_t*>(head.buf_diff_list[0].mapped);
}

const uint32_t* VulkanConverter::GetDirtyTileIndices(int head_id) const {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    const auto& head = m_heads[head_id];
    if (!head.buf_diff_list[0].mapped) return nullptr;
    return reinterpret_cast<const uint32_t*>(head.buf_diff_list[0].mapped) + 1;
}

const uint32_t* VulkanConverter::GetDirtyBitmask(int head_id) const {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    const auto& head = m_heads[head_id];
    if (!head.buf_diff_mask[0].mapped) return nullptr;
    return reinterpret_cast<const uint32_t*>(head.buf_diff_mask[0].mapped);
}

} // namespace dl_turbo
