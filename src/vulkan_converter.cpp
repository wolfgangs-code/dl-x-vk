#include "vulkan_converter.hpp"
#include "common.hpp"
#include <cstring>
#include <algorithm>

namespace dl_turbo {

static const uint32_t s_rgb_to_yuv420_spv[] =
#include "shaders/rgb_to_yuv420_spv.inc"
;

struct PushConstants {
    uint32_t width;
    uint32_t height;
    uint32_t src_stride;
    uint32_t dst_y_stride;
    uint32_t dst_uv_stride;
};

VulkanConverter& VulkanConverter::Instance() {
    static VulkanConverter s_instance;
    return s_instance;
}

VulkanConverter::VulkanConverter() {
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
    if (m_buf_input.size >= src_size &&
        m_buf_y.size >= y_size &&
        m_buf_u.size >= uv_size &&
        m_buf_v.size >= uv_size) {
        return true;
    }

    // Allocate 25% extra capacity to avoid frequent reallocations
    size_t alloc_src = std::max(src_size, m_buf_input.size * 2);
    size_t alloc_y   = std::max(y_size, m_buf_y.size * 2);
    size_t alloc_uv  = std::max(uv_size, m_buf_u.size * 2);

    DestroyBuffer(m_buf_input);
    DestroyBuffer(m_buf_y);
    DestroyBuffer(m_buf_u);
    DestroyBuffer(m_buf_v);

    struct BufferSpec {
        VulkanBuffer& buf;
        size_t size;
        bool is_output;
    } specs[4] = {
        { m_buf_input, alloc_src, false },
        { m_buf_y,     alloc_y,   true  },
        { m_buf_u,     alloc_uv,  true  },
        { m_buf_v,     alloc_uv,  true  }
    };

    for (int i = 0; i < 4; i++) {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = specs[i].size;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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

    // Update descriptor sets
    VkDescriptorBufferInfo bufDescs[4] = {
        { m_buf_input.buffer, 0, m_buf_input.size },
        { m_buf_y.buffer,     0, m_buf_y.size },
        { m_buf_u.buffer,     0, m_buf_u.size },
        { m_buf_v.buffer,     0, m_buf_v.size }
    };

    VkWriteDescriptorSet writes[4] = {};
    for (int i = 0; i < 4; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_desc_set;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &bufDescs[i];
    }
    vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);

    LOG_DEBUG("Vulkan buffers allocated: in=%zu MB, y=%zu MB, uv=%zu MB",
              m_buf_input.size / (1024*1024), m_buf_y.size / (1024*1024), m_buf_u.size / (1024*1024));
    return true;
}

bool VulkanConverter::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return true;

    // 1. Instance
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "dl-x-vk";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "DisplayLinkTurbo";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instInfo = {};
    instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instInfo.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&instInfo, nullptr, &m_instance) != VK_SUCCESS) {
        LOG_WARN("Could not initialize Vulkan instance; falling back to CPU AVX2");
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

    // Prefer integrated/discrete GPU with compute support and 8-bit storage
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

    // 4. Shader Module
    VkShaderModuleCreateInfo moduleInfo = {};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = sizeof(s_rgb_to_yuv420_spv);
    moduleInfo.pCode = s_rgb_to_yuv420_spv;

    if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &m_shader_module) != VK_SUCCESS) {
        LOG_ERROR("Failed to create Vulkan SPIR-V shader module");
        Cleanup();
        return false;
    }

    // 5. Descriptor Set Layout
    VkDescriptorSetLayoutBinding bindings[4] = {};
    for (int i = 0; i < 4; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_desc_layout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create descriptor set layout");
        Cleanup();
        return false;
    }

    // 6. Pipeline Layout with Push Constants
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_desc_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipeline_layout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create compute pipeline layout");
        Cleanup();
        return false;
    }

    // 7. Compute Pipeline
    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = m_shader_module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_pipeline_layout;

    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS) {
        LOG_ERROR("Failed to create Vulkan compute pipeline");
        Cleanup();
        return false;
    }

    // 8. Descriptor Pool & Set
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 4;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_desc_pool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create descriptor pool");
        Cleanup();
        return false;
    }

    VkDescriptorSetAllocateInfo allocSetInfo = {};
    allocSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocSetInfo.descriptorPool = m_desc_pool;
    allocSetInfo.descriptorSetCount = 1;
    allocSetInfo.pSetLayouts = &m_desc_layout;

    if (vkAllocateDescriptorSets(m_device, &allocSetInfo, &m_desc_set) != VK_SUCCESS) {
        LOG_ERROR("Failed to allocate descriptor set");
        Cleanup();
        return false;
    }

    // 9. Command Pool & Buffer & Fence
    VkCommandPoolCreateInfo cmdPoolInfo = {};
    cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.queueFamilyIndex = m_compute_queue_family;
    cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(m_device, &cmdPoolInfo, nullptr, &m_cmd_pool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create command pool");
        Cleanup();
        return false;
    }

    VkCommandBufferAllocateInfo cmdAllocInfo = {};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.commandPool = m_cmd_pool;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(m_device, &cmdAllocInfo, &m_cmd_buffer) != VK_SUCCESS) {
        LOG_ERROR("Failed to allocate command buffer");
        Cleanup();
        return false;
    }

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_fence) != VK_SUCCESS) {
        LOG_ERROR("Failed to create Vulkan fence");
        Cleanup();
        return false;
    }

    m_initialized = true;
    LOG_INFO("Vulkan SPIR-V compute acceleration initialized successfully");
    return true;
}

void VulkanConverter::Cleanup() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

        DestroyBuffer(m_buf_input);
        DestroyBuffer(m_buf_y);
        DestroyBuffer(m_buf_u);
        DestroyBuffer(m_buf_v);

        if (m_fence != VK_NULL_HANDLE) {
            vkDestroyFence(m_device, m_fence, nullptr);
            m_fence = VK_NULL_HANDLE;
        }
        if (m_cmd_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_cmd_pool, nullptr);
            m_cmd_pool = VK_NULL_HANDLE;
        }
        if (m_desc_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_desc_pool, nullptr);
            m_desc_pool = VK_NULL_HANDLE;
        }
        if (m_pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
            m_pipeline = VK_NULL_HANDLE;
        }
        if (m_pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
            m_pipeline_layout = VK_NULL_HANDLE;
        }
        if (m_desc_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, m_desc_layout, nullptr);
            m_desc_layout = VK_NULL_HANDLE;
        }
        if (m_shader_module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, m_shader_module, nullptr);
            m_shader_module = VK_NULL_HANDLE;
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

uint8_t* VulkanConverter::GetMappedInputBuffer(size_t required_bytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return nullptr;
    if (!EnsureBuffers(required_bytes, required_bytes / 4, required_bytes / 16)) return nullptr;
    return static_cast<uint8_t*>(m_buf_input.mapped);
}

void VulkanConverter::GetMappedOutputPlanes(uint8_t*& y_plane, uint8_t*& u_plane, uint8_t*& v_plane) {
    std::lock_guard<std::mutex> lock(m_mutex);
    y_plane = static_cast<uint8_t*>(m_buf_y.mapped);
    u_plane = static_cast<uint8_t*>(m_buf_u.mapped);
    v_plane = static_cast<uint8_t*>(m_buf_v.mapped);
}

bool VulkanConverter::DispatchCompute(int width, int height, int src_stride, int dst_y_stride, int dst_uv_stride) {
    PushConstants pc = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<uint32_t>(src_stride),
        static_cast<uint32_t>(dst_y_stride),
        static_cast<uint32_t>(dst_uv_stride)
    };

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(m_cmd_buffer, &beginInfo) != VK_SUCCESS) return false;

    vkCmdBindPipeline(m_cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(m_cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_desc_set, 0, nullptr);
    vkCmdPushConstants(m_cmd_buffer, m_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    // Each thread processes 2x2 pixels; workgroup is 16x16
    uint32_t group_x = ((width + 1) / 2 + 15) / 16;
    uint32_t group_y = ((height + 1) / 2 + 15) / 16;
    vkCmdDispatch(m_cmd_buffer, group_x, group_y, 1);

    if (vkEndCommandBuffer(m_cmd_buffer) != VK_SUCCESS) return false;

    vkResetFences(m_device, 1, &m_fence);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_cmd_buffer;

    if (vkQueueSubmit(m_compute_queue, 1, &submitInfo, m_fence) != VK_SUCCESS) return false;
    if (vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;

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
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return false;

    size_t src_size = static_cast<size_t>(src_stride) * height;
    size_t y_size   = static_cast<size_t>(dst_y_stride) * height;
    size_t uv_size  = static_cast<size_t>(dst_uv_stride) * ((height + 1) / 2);

    if (!EnsureBuffers(src_size, y_size, uv_size)) return false;

    // Copy input RGB frame into host-visible GPU memory
    std::memcpy(m_buf_input.mapped, src_argb, src_size);

    if (!DispatchCompute(width, height, src_stride, dst_y_stride, dst_uv_stride)) {
        return false;
    }

    // Copy planar YUV420 output from cached host memory
    std::memcpy(dst_y, m_buf_y.mapped, y_size);
    std::memcpy(dst_u, m_buf_u.mapped, uv_size);
    std::memcpy(dst_v, m_buf_v.mapped, uv_size);

    return true;
}

} // namespace dl_turbo
