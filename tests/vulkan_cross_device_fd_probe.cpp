#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <unistd.h>

namespace {

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 360;

struct DeviceContext {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;
};

const char *result_name(VkResult result)
{
    switch (result)
    {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        default: return "VK_OTHER";
    }
}

bool has_extension(VkPhysicalDevice physical, const char *name)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;
    for (const auto &extension : extensions)
        if (!std::strcmp(extension.extensionName, name)) return true;
    return false;
}

VkExternalMemoryHandleTypeFlagBits selected_handle_type()
{
    const char *value = std::getenv("MGPU_VK_EXTERNAL_MEMORY_HANDLE");
    if (value && !std::strcmp(value, "dma-buf"))
        return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
}

const char *handle_type_name(VkExternalMemoryHandleTypeFlagBits handle_type)
{
    return handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
        ? "dma-buf" : "opaque-fd";
}

std::array<uint8_t, VK_UUID_SIZE> device_uuid(VkPhysicalDevice physical)
{
    VkPhysicalDeviceIDProperties id_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &id_properties;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    std::array<uint8_t, VK_UUID_SIZE> uuid{};
    std::copy(std::begin(id_properties.deviceUUID), std::end(id_properties.deviceUUID), uuid.begin());
    return uuid;
}

uint32_t find_type(VkPhysicalDevice physical, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

bool create_context(VkPhysicalDevice physical, DeviceContext *context)
{
    context->physical = physical;
    if (!has_extension(physical, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) ||
            !has_extension(physical, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))
        return false;

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
    for (uint32_t i = 0; i < count; ++i)
    {
        if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            context->queue_family = i;
            break;
        }
    }
    if (context->queue_family == UINT32_MAX) return false;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = context->queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char *extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 2;
    device_info.ppEnabledExtensionNames = extensions;
    VkResult result = vkCreateDevice(physical, &device_info, nullptr, &context->device);
    if (result != VK_SUCCESS)
    {
        std::fprintf(stderr, "vkCreateDevice result=%s code=%d\n", result_name(result), result);
        return false;
    }
    vkGetDeviceQueue(context->device, context->queue_family, 0, &context->queue);
    return true;
}

VkImage create_image(VkDevice device, VkExternalMemoryHandleTypeFlagBits handle_type)
{
    VkExternalMemoryImageCreateInfo external_info{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external_info.handleTypes = handle_type;
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.pNext = &external_info;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    image_info.extent = {kWidth, kHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VkResult result = vkCreateImage(device, &image_info, nullptr, &image);
    std::fprintf(stderr, "vkCreateImage result=%s code=%d\n", result_name(result), result);
    return result == VK_SUCCESS ? image : VK_NULL_HANDLE;
}

} // namespace

int main()
{
    const VkExternalMemoryHandleTypeFlagBits handle_type = selected_handle_type();
    std::fprintf(stderr, "external_memory_handle=%s\n", handle_type_name(handle_type));
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&instance_info, nullptr, &instance);
    if (result != VK_SUCCESS)
    {
        std::fprintf(stderr, "vkCreateInstance result=%s code=%d\n", result_name(result), result);
        return 2;
    }

    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (result != VK_SUCCESS || count < 2)
    {
        std::fprintf(stderr, "physical_device_count=%u result=%s\n", count, result_name(result));
        vkDestroyInstance(instance, nullptr);
        return 3;
    }
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance, &count, physicals.data());
    std::vector<VkPhysicalDevice> nvidia;
    std::vector<std::array<uint8_t, VK_UUID_SIZE>> nvidia_uuids;
    for (VkPhysicalDevice physical : physicals)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (properties.vendorID == 0x10de && properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            auto uuid = device_uuid(physical);
            if (std::find(nvidia_uuids.begin(), nvidia_uuids.end(), uuid) != nvidia_uuids.end())
                continue;
            std::fprintf(stderr, "nvidia[%zu] name=%s pci_device=0x%x uuid=%02x:%02x:%02x:%02x\n",
                    nvidia.size(), properties.deviceName, properties.deviceID,
                    uuid[0], uuid[1], uuid[2], uuid[3]);
            nvidia.push_back(physical);
            nvidia_uuids.push_back(uuid);
        }
    }
    if (nvidia.size() < 2)
    {
        std::fprintf(stderr, "nvidia_device_count=%zu\n", nvidia.size());
        vkDestroyInstance(instance, nullptr);
        return 4;
    }

    DeviceContext source, destination;
    if (!create_context(nvidia[0], &source) || !create_context(nvidia[1], &destination))
    {
        vkDestroyDevice(source.device, nullptr);
        vkDestroyDevice(destination.device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return 5;
    }
    VkImage source_image = create_image(source.device, handle_type);
    VkImage destination_image = create_image(destination.device, handle_type);
    if (!source_image || !destination_image) return 6;

    VkMemoryRequirements source_requirements{};
    vkGetImageMemoryRequirements(source.device, source_image, &source_requirements);
    uint32_t source_type = find_type(source.physical, source_requirements.memoryTypeBits);
    VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    export_info.handleTypes = handle_type;
    VkMemoryAllocateInfo source_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    source_allocate.pNext = &export_info;
    source_allocate.allocationSize = source_requirements.size;
    source_allocate.memoryTypeIndex = source_type;
    VkDeviceMemory source_memory = VK_NULL_HANDLE;
    result = vkAllocateMemory(source.device, &source_allocate, nullptr, &source_memory);
    std::fprintf(stderr, "source_allocate result=%s code=%d size=%llu type=%u\n",
            result_name(result), result, (unsigned long long)source_requirements.size, source_type);
    if (result != VK_SUCCESS) return 7;
    result = vkBindImageMemory(source.device, source_image, source_memory, 0);
    std::fprintf(stderr, "source_bind result=%s code=%d\n", result_name(result), result);

    auto source_get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
            vkGetDeviceProcAddr(source.device, "vkGetMemoryFdKHR"));
    auto destination_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(destination.device, "vkGetMemoryFdPropertiesKHR"));
    auto destination_get_requirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
            vkGetDeviceProcAddr(destination.device, "vkGetImageMemoryRequirements"));
    if (!source_get_fd || !destination_fd_properties || !destination_get_requirements) return 8;
    VkMemoryGetFdInfoKHR get_fd_info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    get_fd_info.memory = source_memory;
    get_fd_info.handleType = handle_type;
    int fd = -1;
    result = source_get_fd(source.device, &get_fd_info, &fd);
    std::fprintf(stderr, "source_export_fd result=%s code=%d fd=%d\n", result_name(result), result, fd);
    if (result != VK_SUCCESS) return 9;

    VkMemoryFdPropertiesKHR fd_properties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    result = destination_fd_properties(destination.device,
            handle_type, fd, &fd_properties);
    std::fprintf(stderr, "destination_fd_properties result=%s code=%d bits=0x%x\n",
            result_name(result), result, fd_properties.memoryTypeBits);
    VkMemoryRequirements destination_requirements{};
    destination_get_requirements(destination.device, destination_image, &destination_requirements);
    uint32_t destination_type = find_type(destination.physical, destination_requirements.memoryTypeBits &
            (fd_properties.memoryTypeBits ? fd_properties.memoryTypeBits : UINT32_MAX));
    VkImportMemoryFdInfoKHR import_info{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    import_info.handleType = handle_type;
    import_info.fd = fd;
    VkMemoryDedicatedAllocateInfo dedicated_import{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    if (std::getenv("MGPU_VK_DEDICATED_IMPORT")) {
        dedicated_import.image = destination_image;
        import_info.pNext = &dedicated_import;
        std::fprintf(stderr, "destination_import_dedicated=true\n");
    }
    VkMemoryAllocateInfo destination_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    destination_allocate.pNext = &import_info;
    destination_allocate.allocationSize = source_requirements.size;
    destination_allocate.memoryTypeIndex = destination_type;
    VkDeviceMemory destination_memory = VK_NULL_HANDLE;
    result = vkAllocateMemory(destination.device, &destination_allocate, nullptr, &destination_memory);
    std::fprintf(stderr, "destination_import_allocate result=%s code=%d size=%llu type=%u\n",
            result_name(result), result, (unsigned long long)source_requirements.size, destination_type);
    if (result != VK_SUCCESS)
    {
        close(fd);
        return 10;
    }
    result = vkBindImageMemory(destination.device, destination_image, destination_memory, 0);
    std::fprintf(stderr, "destination_import_bind result=%s code=%d\n", result_name(result), result);

    vkFreeMemory(destination.device, destination_memory, nullptr);
    vkDestroyImage(destination.device, destination_image, nullptr);
    vkDestroyImage(source.device, source_image, nullptr);
    vkFreeMemory(source.device, source_memory, nullptr);
    vkDestroyDevice(destination.device, nullptr);
    vkDestroyDevice(source.device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return result == VK_SUCCESS ? 0 : 11;
}
