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

void VulkanConverter::UpdateImportedDescriptorSet(int head_id, ImportedBuffer& imp, size_t fb_size, size_t mask_size, size_t list_size) {
    if (imp.desc_set == VK_NULL_HANDLE || imp.buffer == VK_NULL_HANDLE) return;
    auto& head = m_heads[head_id];
    if (head.buf_diff_ref.buffer == VK_NULL_HANDLE ||
        head.buf_diff_mask[0].buffer == VK_NULL_HANDLE ||
        head.buf_diff_list[0].buffer == VK_NULL_HANDLE) return;

    VkDescriptorBufferInfo dbi[4] = {
        { imp.buffer, imp.page_offset, imp.fb_size },
        { head.buf_diff_ref.buffer, 0, fb_size },
        { head.buf_diff_mask[0].buffer, 0, mask_size },
        { head.buf_diff_list[0].buffer, 0, list_size }
    };

    VkWriteDescriptorSet writes[4] = {};
    for (int i = 0; i < 4; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = imp.desc_set;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &dbi[i];
    }
    vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);
    imp.desc_set_valid = true;
}

bool VulkanConverter::EnsureDiffBuffers(int head_id, size_t fb_size, uint32_t total_tiles) {
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    auto& head = m_heads[head_id];

    uint32_t mask_words = (total_tiles + 31) / 32;
    size_t mask_size = std::max(size_t(256), static_cast<size_t>(mask_words) * sizeof(uint32_t));
    size_t list_size = std::max(size_t(256), (1 + static_cast<size_t>(total_tiles)) * sizeof(uint32_t));

    bool buffers_changed = false;

    // Fallback input double-buffers (used if host memory import is unavailable)
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
            buffers_changed = true;
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
        buffers_changed = true;
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
            buffers_changed = true;
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
            buffers_changed = true;
        }
    }

    head.diff_total_tiles = total_tiles;

    // Update fallback descriptor sets
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

    // If underlying GPU diff buffers changed or were created, update all registered imported buffers
    if (buffers_changed) {
        for (auto& imp : head.imported_buffers) {
            UpdateImportedDescriptorSet(head_id, imp, fb_size, mask_size, list_size);
        }
    }

    return true;
}

bool VulkanConverter::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_initialized) return true;

    // 1. Instance Extensions
    std::vector<const char*> instExts;
    uint32_t instExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr);
    if (instExtCount > 0) {
        std::vector<VkExtensionProperties> instExtProps(instExtCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, instExtProps.data());
        for (const auto& ext : instExtProps) {
            if (strcmp(ext.extensionName, "VK_KHR_external_memory_capabilities") == 0) {
                instExts.push_back("VK_KHR_external_memory_capabilities");
                break;
            }
        }
    }

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
    instInfo.enabledExtensionCount = static_cast<uint32_t>(instExts.size());
    instInfo.ppEnabledExtensionNames = instExts.empty() ? nullptr : instExts.data();

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

    // 3. Check for VK_EXT_external_memory_host device extension
    std::vector<const char*> devExts;
    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(m_physical_device, nullptr, &devExtCount, nullptr);
    bool supports_ext_mem_host = false;
    if (devExtCount > 0) {
        std::vector<VkExtensionProperties> devExtProps(devExtCount);
        vkEnumerateDeviceExtensionProperties(m_physical_device, nullptr, &devExtCount, devExtProps.data());
        for (const auto& ext : devExtProps) {
            if (strcmp(ext.extensionName, "VK_EXT_external_memory_host") == 0) {
                devExts.push_back("VK_EXT_external_memory_host");
                supports_ext_mem_host = true;
                break;
            }
        }
    }

    // 4. Logical Device
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
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(devExts.size());
    deviceCreateInfo.ppEnabledExtensionNames = devExts.empty() ? nullptr : devExts.data();

    if (vkCreateDevice(m_physical_device, &deviceCreateInfo, nullptr, &m_device) != VK_SUCCESS) {
        LOG_WARN("Failed to create Vulkan logical device");
        Cleanup();
        return false;
    }

    vkGetDeviceQueue(m_device, m_compute_queue_family, 0, &m_compute_queue);

    // 5. Initialize VK_EXT_external_memory_host function pointer
    if (supports_ext_mem_host) {
        m_vkGetMemoryHostPointerPropertiesEXT = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
            vkGetDeviceProcAddr(m_device, "vkGetMemoryHostPointerPropertiesEXT")
        );
        if (m_vkGetMemoryHostPointerPropertiesEXT) {
            m_has_external_memory_host = true;

            VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps = {};
            hostProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 props2 = {};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &hostProps;
            vkGetPhysicalDeviceProperties2(m_physical_device, &props2);
            if (hostProps.minImportedHostPointerAlignment > 0) {
                m_min_imported_host_pointer_alignment = hostProps.minImportedHostPointerAlignment;
            }

            LOG_INFO("Vulkan Zero-Copy host memory import enabled! (minImportedHostPointerAlignment=%zu)",
                     m_min_imported_host_pointer_alignment);
        } else {
            LOG_WARN("Could not load vkGetMemoryHostPointerPropertiesEXT function pointer");
        }
    } else {
        LOG_WARN("VK_EXT_external_memory_host not supported on device; using fallback staging buffer");
    }

    // 6. Tile Differencing Pipeline
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

    // 7. Descriptor Pool (supports dynamic allocation and free for imported buffers)
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 }
    };
    VkDescriptorPoolCreateInfo poolInfo = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,
        VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        32, 1, poolSizes
    };
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

    // 8. Allocate Per-Head Fallback Descriptor Sets, Command Buffers, and Fences
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
            UnregisterAllExternalBuffers(h);
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

    m_has_external_memory_host = false;
    m_vkGetMemoryHostPointerPropertiesEXT = nullptr;
    m_initialized = false;
}

ImportedBuffer* VulkanConverter::FindImportedBuffer(int head_id, const void* host_ptr) {
    if (head_id < 0 || head_id >= MAX_HEADS) return nullptr;
    auto& head = m_heads[head_id];
    for (auto& b : head.imported_buffers) {
        if (b.host_ptr == host_ptr) {
            return &b;
        }
    }
    return nullptr;
}

bool VulkanConverter::IsBufferImported(int head_id, const void* host_ptr) const {
    if (head_id < 0 || head_id >= MAX_HEADS) return false;
    const auto& head = m_heads[head_id];
    for (const auto& b : head.imported_buffers) {
        if (b.host_ptr == host_ptr) {
            return true;
        }
    }
    return false;
}

bool VulkanConverter::RegisterExternalBuffer(int head_id, int buffer_id, void* host_ptr, size_t fb_size) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized || !m_has_external_memory_host || !host_ptr || fb_size == 0) {
        return false;
    }
    if (head_id < 0 || head_id >= MAX_HEADS) head_id = 0;
    auto& head = m_heads[head_id];

    // Check if already registered
    for (const auto& imp : head.imported_buffers) {
        if (imp.buffer_id == buffer_id && imp.host_ptr == host_ptr && imp.fb_size == fb_size) {
            return true;
        }
    }

    // If buffer_id exists with a different pointer, unregister it first
    if (buffer_id >= 0) {
        UnregisterExternalBuffer(head_id, buffer_id);
    }

    uintptr_t host_addr = reinterpret_cast<uintptr_t>(host_ptr);
    size_t align = m_min_imported_host_pointer_alignment;
    if (align == 0) align = 4096;

    uintptr_t page_addr = host_addr & ~(align - 1);
    uint32_t page_offset = static_cast<uint32_t>(host_addr - page_addr);
    size_t alloc_size = (fb_size + page_offset + align - 1) & ~(align - 1);

    // Verify storage buffer offset alignment (typically 4 bytes on AMD APU)
    if (page_offset % 4 != 0) {
        LOG_WARN("[Vulkan] Host pointer %p offset %u is not 4-byte aligned", host_ptr, page_offset);
        return false;
    }

    VkMemoryHostPointerPropertiesEXT hostProps = {
        VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT, nullptr, 0
    };
    VkResult res = m_vkGetMemoryHostPointerPropertiesEXT(
        m_device,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        reinterpret_cast<void*>(page_addr),
        &hostProps
    );
    if (res != VK_SUCCESS) {
        LOG_WARN("[Vulkan] vkGetMemoryHostPointerPropertiesEXT failed for ptr %p: %d", host_ptr, res);
        return false;
    }

    VkExternalMemoryBufferCreateInfo extBufInfo = {
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT
    };

    VkBufferCreateInfo bci = {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &extBufInfo, 0,
        alloc_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, nullptr
    };

    VkBuffer vk_buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &vk_buf) != VK_SUCCESS) {
        LOG_WARN("[Vulkan] Failed to create external buffer for ptr %p", host_ptr);
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, vk_buf, &req);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physical_device, &memProps);
    uint32_t memTypeIndex = UINT32_MAX;

    // Prefer HOST_COHERENT | HOST_CACHED
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & hostProps.memoryTypeBits) & (1 << i)) {
            auto flags = memProps.memoryTypes[i].propertyFlags;
            if ((flags & (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
                memTypeIndex = i;
                break;
            }
        }
    }
    if (memTypeIndex == UINT32_MAX) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((req.memoryTypeBits & hostProps.memoryTypeBits) & (1 << i)) {
                if (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
                    memTypeIndex = i;
                    break;
                }
            }
        }
    }
    if (memTypeIndex == UINT32_MAX) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((req.memoryTypeBits & hostProps.memoryTypeBits) & (1 << i)) {
                memTypeIndex = i;
                break;
            }
        }
    }

    if (memTypeIndex == UINT32_MAX) {
        LOG_WARN("[Vulkan] No suitable memory type found for external host pointer %p", host_ptr);
        vkDestroyBuffer(m_device, vk_buf, nullptr);
        return false;
    }

    VkImportMemoryHostPointerInfoEXT importInfo = {
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        reinterpret_cast<void*>(page_addr)
    };

    VkMemoryAllocateInfo ai = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &importInfo,
        req.size, memTypeIndex
    };

    VkDeviceMemory vk_mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(m_device, &ai, nullptr, &vk_mem) != VK_SUCCESS) {
        LOG_WARN("[Vulkan] vkAllocateMemory failed for external host pointer %p", host_ptr);
        vkDestroyBuffer(m_device, vk_buf, nullptr);
        return false;
    }

    if (vkBindBufferMemory(m_device, vk_buf, vk_mem, 0) != VK_SUCCESS) {
        LOG_WARN("[Vulkan] vkBindBufferMemory failed for external buffer %p", host_ptr);
        vkFreeMemory(m_device, vk_mem, nullptr);
        vkDestroyBuffer(m_device, vk_buf, nullptr);
        return false;
    }

    // Allocate dedicated descriptor set from pool
    VkDescriptorSetAllocateInfo dsai = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
        m_desc_pool, 1, &m_diff_desc_layout
    };
    VkDescriptorSet desc_set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_device, &dsai, &desc_set) != VK_SUCCESS) {
        LOG_WARN("[Vulkan] Failed to allocate descriptor set for imported buffer %p", host_ptr);
        vkFreeMemory(m_device, vk_mem, nullptr);
        vkDestroyBuffer(m_device, vk_buf, nullptr);
        return false;
    }

    ImportedBuffer imp;
    imp.buffer_id = buffer_id;
    imp.host_ptr = host_ptr;
    imp.page_addr = page_addr;
    imp.page_offset = page_offset;
    imp.alloc_size = alloc_size;
    imp.fb_size = fb_size;
    imp.buffer = vk_buf;
    imp.memory = vk_mem;
    imp.desc_set = desc_set;

    // If GPU differencing buffers already exist, bind descriptor set now
    if (head.buf_diff_ref.buffer != VK_NULL_HANDLE &&
        head.buf_diff_mask[0].buffer != VK_NULL_HANDLE &&
        head.buf_diff_list[0].buffer != VK_NULL_HANDLE) {
        UpdateImportedDescriptorSet(head_id, imp, head.buf_diff_ref.size,
                                    head.buf_diff_mask[0].size, head.buf_diff_list[0].size);
    }

    head.imported_buffers.push_back(imp);
    LOG_INFO("[Vulkan] Imported host buffer (head=%d, id=%d, ptr=%p, offset=%u, size=%zu) into Vulkan zero-copy memory!",
             head_id, buffer_id, host_ptr, page_offset, fb_size);
    return true;
}

void VulkanConverter::UnregisterExternalBuffer(int head_id, int buffer_id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (head_id < 0 || head_id >= MAX_HEADS) return;
    auto& head = m_heads[head_id];

    auto it = std::find_if(head.imported_buffers.begin(), head.imported_buffers.end(),
        [buffer_id](const ImportedBuffer& b) { return b.buffer_id == buffer_id; });

    if (it != head.imported_buffers.end()) {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
            if (it->desc_set != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(m_device, m_desc_pool, 1, &it->desc_set);
            }
            if (it->buffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(m_device, it->buffer, nullptr);
            }
            if (it->memory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, it->memory, nullptr);
            }
        }
        head.imported_buffers.erase(it);
        LOG_INFO("[Vulkan] Unregistered external buffer id=%d on head %d", buffer_id, head_id);
    }
}

void VulkanConverter::UnregisterAllExternalBuffers(int head_id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (head_id < 0 || head_id >= MAX_HEADS) return;
    auto& head = m_heads[head_id];

    if (m_device != VK_NULL_HANDLE && !head.imported_buffers.empty()) {
        vkDeviceWaitIdle(m_device);
        for (auto& it : head.imported_buffers) {
            if (it.desc_set != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(m_device, m_desc_pool, 1, &it.desc_set);
            }
            if (it.buffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(m_device, it.buffer, nullptr);
            }
            if (it.memory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, it.memory, nullptr);
            }
        }
    }
    head.imported_buffers.clear();
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
    int head_id,
    VkDescriptorSet custom_desc_set
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

    VkDescriptorSet desc_set = (custom_desc_set != VK_NULL_HANDLE) ? custom_desc_set : head.diff_desc_set[0];
    vkCmdBindDescriptorSets(head.cmd_buffer[0], VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_diff_pipeline_layout, 0, 1, &desc_set, 0, nullptr);
    vkCmdPushConstants(head.cmd_buffer[0], m_diff_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(DiffPushConstants), &pc);

    // Each workgroup (32x4 invocations = 128 threads) tests exactly one 32x32 tile
    uint32_t gx = num_cols;
    uint32_t gy = num_rows;
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

    // Check if curr_fb is an imported host memory buffer (Vector 2: Zero-Copy)
    ImportedBuffer* imp = FindImportedBuffer(head_id, curr_fb);
    if (!imp && m_has_external_memory_host && curr_fb) {
        // Try on-demand import if not already registered
        if (RegisterExternalBuffer(head_id, -1, const_cast<uint8_t*>(curr_fb), fb_size)) {
            imp = FindImportedBuffer(head_id, curr_fb);
        }
    }

    VkDescriptorSet desc_set_to_use = VK_NULL_HANDLE;

    if (imp && imp->desc_set != VK_NULL_HANDLE) {
        if (!imp->desc_set_valid) {
            uint32_t mask_words = (total_tiles + 31) / 32;
            size_t mask_size = std::max(size_t(256), static_cast<size_t>(mask_words) * sizeof(uint32_t));
            size_t list_size = std::max(size_t(256), (1 + static_cast<size_t>(total_tiles)) * sizeof(uint32_t));
            UpdateImportedDescriptorSet(head_id, *imp, fb_size, mask_size, list_size);
        }
        if (imp->desc_set_valid) {
            // ZERO-COPY PATH: Host memory mapped directly via VK_EXT_external_memory_host!
            // No std::memcpy row copy!
            desc_set_to_use = imp->desc_set;
            m_zero_copy_dispatches.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (desc_set_to_use == VK_NULL_HANDLE) {
        // FALLBACK PATH: CPU sub-region memory copy into staging buffer
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
        desc_set_to_use = head.diff_desc_set[0];
        m_fallback_dispatches.fetch_add(1, std::memory_order_relaxed);
    }

    int stride_words = fb_stride / 4;
    if (!DispatchTileDifferencing(width, height, stride_words, tile_size, true,
                                 start_col, start_row, num_cols, num_rows, head_id, desc_set_to_use)) {
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
