#include <vulkan/vulkan.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Args {
    int fd = -1;
    VkDeviceSize allocation_size = 0;
    uint32_t destination = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dxgi_format = 0;
    VkDeviceSize resource_offset = 0;
};

const char* result_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    default: return "VK_OTHER";
    }
}

bool parse_u64(const char* value, uint64_t* result) {
    if (!value || !*value) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value, &end, 0);
    if (errno || !end || *end) return false;
    *result = parsed;
    return true;
}

bool parse_args(int argc, char** argv, Args* args) {
    uint64_t value = 0;
    if (argc < 8 || !parse_u64(argv[1], &value)) return false;
    args->fd = static_cast<int>(value);
    if (!parse_u64(argv[2], &value)) return false;
    args->allocation_size = static_cast<VkDeviceSize>(value);
    if (!parse_u64(argv[3], &value)) return false;
    args->destination = static_cast<uint32_t>(value);
    if (!parse_u64(argv[4], &value)) return false;
    args->width = static_cast<uint32_t>(value);
    if (!parse_u64(argv[5], &value)) return false;
    args->height = static_cast<uint32_t>(value);
    if (!parse_u64(argv[6], &value)) return false;
    args->dxgi_format = static_cast<uint32_t>(value);
    if (!parse_u64(argv[7], &value)) return false;
    args->resource_offset = static_cast<VkDeviceSize>(value);
    return args->fd >= 0 && args->allocation_size > 0 && args->width > 0 && args->height > 0;
}

VkFormat convert_format(uint32_t dxgi_format) {
    // The current bridge probe creates the output as DXGI_FORMAT_R16G16B16A16_FLOAT.
    // Keep the conversion explicit so an unsupported host format fails closed.
    if (dxgi_format == 10) return VK_FORMAT_R16G16B16A16_SFLOAT;
    return VK_FORMAT_UNDEFINED;
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
}

int fail(const char* stage, VkResult result, const char* detail = nullptr) {
    std::cerr << "vulkan_image_import stage=" << stage
              << " result=" << result_name(result)
              << " code=" << static_cast<int>(result);
    if (detail) std::cerr << " detail=" << detail;
    std::cerr << "\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, &args)) {
        std::cerr << "usage: " << argv[0]
                  << " FD ALLOCATION_SIZE DESTINATION WIDTH HEIGHT DXGI_FORMAT OFFSET\n";
        return 2;
    }

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instance_info, nullptr, &instance);
    if (result != VK_SUCCESS) return fail("create_instance", result);

    uint32_t physical_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &physical_count, nullptr);
    if (result != VK_SUCCESS || physical_count == 0) {
        vkDestroyInstance(instance, nullptr);
        return fail("enumerate_physical_devices", result);
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    result = vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices.data());
    if (result != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return fail("enumerate_physical_devices", result);
    }
    std::vector<VkPhysicalDevice> nvidia;
    for (VkPhysicalDevice physical : physical_devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (properties.vendorID == 0x10de) nvidia.push_back(physical);
    }
    if (args.destination >= nvidia.size()) {
        vkDestroyInstance(instance, nullptr);
        return fail("select_destination", VK_ERROR_INITIALIZATION_FAILED,
                    "destination NVIDIA ordinal unavailable");
    }
    VkPhysicalDevice physical = nvidia[args.destination];
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::cerr << "vulkan_image_import destination=" << args.destination
              << " name=" << properties.deviceName << "\n";

    uint32_t extension_count = 0;
    result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr);
    if (result != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return fail("enumerate_extensions", result);
    }
    std::vector<VkExtensionProperties> extensions(extension_count);
    result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count,
                                                    extensions.data());
    if (result != VK_SUCCESS || !has_extension(extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        vkDestroyInstance(instance, nullptr);
        return fail("external_memory_fd_extension", VK_ERROR_EXTENSION_NOT_PRESENT);
    }

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_family_count, queue_families.data());
    uint32_t queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < queue_family_count; ++i) {
        if (queue_families[i].queueCount &&
            (queue_families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            queue_family = i;
            break;
        }
    }
    if (queue_family == UINT32_MAX) {
        vkDestroyInstance(instance, nullptr);
        return fail("select_queue_family", VK_ERROR_INITIALIZATION_FAILED);
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* device_extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    VkDevice device = VK_NULL_HANDLE;
    result = vkCreateDevice(physical, &device_info, nullptr, &device);
    if (result != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return fail("create_device", result);
    }

    const VkFormat format = convert_format(args.dxgi_format);
    if (format == VK_FORMAT_UNDEFINED) {
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return fail("convert_format", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {args.width, args.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExternalMemoryImageCreateInfo external_image_info{};
    external_image_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_image_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    image_info.pNext = &external_image_info;
    VkImage image = VK_NULL_HANDLE;
    result = vkCreateImage(device, &image_info, nullptr, &image);
    if (result != VK_SUCCESS) {
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return fail("create_image", result);
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    if (args.resource_offset + requirements.size > args.allocation_size) {
        vkDestroyImage(device, image, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return fail("memory_bounds", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    }
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t external_memory_type_bits = 0;
    auto get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
    if (get_memory_fd_properties) {
        VkMemoryFdPropertiesKHR fd_properties{};
        fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        const VkResult fd_properties_result = get_memory_fd_properties(
            device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, args.fd, &fd_properties);
        std::cerr << "vulkan_image_import fd_properties_result="
                  << result_name(fd_properties_result)
                  << " bits=0x" << std::hex << fd_properties.memoryTypeBits << std::dec << "\n";
        if (fd_properties_result == VK_SUCCESS)
            external_memory_type_bits = fd_properties.memoryTypeBits;
    }
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (!external_memory_type_bits || (external_memory_type_bits & (1u << i))) &&
            (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == UINT32_MAX) {
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (!external_memory_type_bits || (external_memory_type_bits & (1u << i)))) {
                memory_type = i;
                break;
            }
        }
    }
    if (memory_type == UINT32_MAX) {
        vkDestroyImage(device, image, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return fail("select_memory_type", VK_ERROR_MEMORY_MAP_FAILED);
    }
    VkImportMemoryFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = args.fd;
    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &import_info;
    allocate_info.allocationSize = args.allocation_size;
    allocate_info.memoryTypeIndex = memory_type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    result = vkAllocateMemory(device, &allocate_info, nullptr, &memory);
    if (result != VK_SUCCESS) {
        // Ownership of a successful import transfers to Vulkan. On failure the
        // descriptor remains ours, so close it to avoid leaking the probe FD.
        close(args.fd);
        vkDestroyImage(device, image, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return fail("import_memory", result);
    }
    result = vkBindImageMemory(device, image, memory, args.resource_offset);
    VkResult access_result = result;
    const char* access_stage = result == VK_SUCCESS ? "not_run" : "bind_image";
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (result == VK_SUCCESS) {
        do {
            VkBufferCreateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = 8;
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            access_result = vkCreateBuffer(device, &buffer_info, nullptr, &staging_buffer);
            if (access_result != VK_SUCCESS) {
                access_stage = "create_staging_buffer";
                break;
            }
            VkMemoryRequirements staging_requirements{};
            vkGetBufferMemoryRequirements(device, staging_buffer, &staging_requirements);
            uint32_t staging_type = UINT32_MAX;
            for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
                const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
                if ((staging_requirements.memoryTypeBits & (1u << i)) &&
                    (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                    (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    staging_type = i;
                    break;
                }
            }
            if (staging_type == UINT32_MAX) {
                access_result = VK_ERROR_MEMORY_MAP_FAILED;
                access_stage = "select_staging_memory";
                break;
            }
            VkMemoryAllocateInfo staging_allocate{};
            staging_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            staging_allocate.allocationSize = staging_requirements.size;
            staging_allocate.memoryTypeIndex = staging_type;
            access_result = vkAllocateMemory(device, &staging_allocate, nullptr, &staging_memory);
            if (access_result != VK_SUCCESS) {
                access_stage = "allocate_staging_memory";
                break;
            }
            access_result = vkBindBufferMemory(device, staging_buffer, staging_memory, 0);
            if (access_result != VK_SUCCESS) {
                access_stage = "bind_staging_buffer";
                break;
            }
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.queueFamilyIndex = queue_family;
            access_result = vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);
            if (access_result != VK_SUCCESS) {
                access_stage = "create_command_pool";
                break;
            }
            VkCommandBufferAllocateInfo command_allocate{};
            command_allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            command_allocate.commandPool = command_pool;
            command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            command_allocate.commandBufferCount = 1;
            access_result = vkAllocateCommandBuffers(device, &command_allocate, &command_buffer);
            if (access_result != VK_SUCCESS) {
                access_stage = "allocate_command_buffer";
                break;
            }
            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            access_result = vkBeginCommandBuffer(command_buffer, &begin_info);
            if (access_result != VK_SUCCESS) {
                access_stage = "begin_command_buffer";
                break;
            }
            VkImageMemoryBarrier to_transfer{};
            to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_transfer.srcAccessMask = 0;
            to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_transfer.image = image;
            to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            to_transfer.subresourceRange.levelCount = 1;
            to_transfer.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &to_transfer);
            VkClearColorValue clear_value{{0.25f, 0.5f, 0.75f, 1.0f}};
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;
            vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &clear_value, 1, &range);
            VkImageMemoryBarrier to_source = to_transfer;
            to_source.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &to_source);
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   staging_buffer, 1, &copy);
            access_result = vkEndCommandBuffer(command_buffer);
            if (access_result != VK_SUCCESS) {
                access_stage = "end_command_buffer";
                break;
            }
            VkQueue queue = VK_NULL_HANDLE;
            vkGetDeviceQueue(device, queue_family, 0, &queue);
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command_buffer;
            access_result = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
            if (access_result != VK_SUCCESS) {
                access_stage = "queue_submit";
                break;
            }
            access_result = vkQueueWaitIdle(queue);
            if (access_result != VK_SUCCESS) {
                access_stage = "queue_wait_idle";
                break;
            }
            access_result = vkMapMemory(device, staging_memory, 0, 8, 0, &mapped);
            if (access_result != VK_SUCCESS) {
                access_stage = "map_staging";
                break;
            }
            const unsigned char expected[8] = {0x00, 0x34, 0x00, 0x38,
                                                0x00, 0x3a, 0x00, 0x3c};
            if (std::memcmp(mapped, expected, sizeof(expected)) != 0) {
                access_result = VK_ERROR_UNKNOWN;
                access_stage = "validate_readback";
                break;
            }
            access_stage = "gpu_clear_copy_readback";
        } while (false);
    }
    if (mapped) vkUnmapMemory(device, staging_memory);
    std::cout << "{\"destination\":" << args.destination
              << ",\"width\":" << args.width
              << ",\"height\":" << args.height
              << ",\"dxgi_format\":" << args.dxgi_format
              << ",\"allocation_size\":" << args.allocation_size
              << ",\"requirements_size\":" << requirements.size
              << ",\"external_memory_type_bits\":" << external_memory_type_bits
              << ",\"resource_offset\":" << args.resource_offset
              << ",\"memory_type\":" << memory_type
              << ",\"bind_result\":\"" << result_name(result)
              << "\",\"bind_code\":" << static_cast<int>(result)
              << ",\"gpu_access_stage\":\"" << access_stage
              << "\",\"gpu_access_result\":\"" << result_name(access_result)
              << "\",\"gpu_access_code\":" << static_cast<int>(access_result) << "}\n";
    if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
    if (staging_buffer) vkDestroyBuffer(device, staging_buffer, nullptr);
    if (staging_memory) vkFreeMemory(device, staging_memory, nullptr);
    if (result == VK_SUCCESS && access_result == VK_SUCCESS) {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 0;
    }
    vkFreeMemory(device, memory, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 1;
}
