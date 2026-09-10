#include <cuda_runtime_api.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

struct Options {
    uint32_t vulkan_gpu = 0;
    int cuda_source = 0;
    int cuda_destination = 1;
    VkDeviceSize bytes = 1920ULL * 1080ULL * 4ULL;
};

struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocation_size = 0;
    int exported_fd = -1;
    PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;
};

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "  --vulkan-gpu N      Vulkan physical device, default 0\n"
              << "  --cuda-source N     CUDA source device, default 0\n"
              << "  --cuda-destination N CUDA destination device, default 1\n"
              << "  --bytes N           buffer size, default 8294400\n"
              << "  --help              show help\n";
}

bool parse_u32(const char* value, uint32_t* result) {
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (!value || *value == '\0' || !end || *end != '\0') return false;
    *result = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_int(const char* value, int* result) {
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (!value || *value == '\0' || !end || *end != '\0') return false;
    *result = static_cast<int>(parsed);
    return true;
}

bool parse_size(const char* value, VkDeviceSize* result) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (!value || *value == '\0' || !end || *end != '\0') return false;
    *result = static_cast<VkDeviceSize>(parsed);
    return true;
}

bool parse_options(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (i + 1 >= argc) return false;
        if (arg == "--vulkan-gpu") {
            if (!parse_u32(argv[++i], &options->vulkan_gpu)) return false;
        } else if (arg == "--cuda-source") {
            if (!parse_int(argv[++i], &options->cuda_source)) return false;
        } else if (arg == "--cuda-destination") {
            if (!parse_int(argv[++i], &options->cuda_destination)) return false;
        } else if (arg == "--bytes") {
            if (!parse_size(argv[++i], &options->bytes)) return false;
        } else {
            return false;
        }
    }
    return options->bytes > 0 && options->cuda_source >= 0 &&
           options->cuda_destination >= 0 &&
           options->cuda_source != options->cuda_destination;
}

const char* vk_result_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    default: return "VK_OTHER_ERROR";
    }
}

bool vk_check(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    std::cerr << operation << " failed: " << vk_result_name(result)
              << " (" << result << ")\n";
    return false;
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions,
                   const char* name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [name](const VkExtensionProperties& extension) {
                           return std::strcmp(extension.extensionName, name) == 0;
                       });
}

std::string uuid_string(const uint8_t* uuid, std::size_t size);

struct PhysicalIdentity {
    std::string key;
    VkPhysicalDeviceProperties2 properties{};
    VkPhysicalDeviceIDProperties id{};
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci{};
};

PhysicalIdentity get_physical_identity(VkPhysicalDevice physical) {
    PhysicalIdentity identity;
    identity.properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    identity.id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    identity.pci.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
    identity.pci.pNext = &identity.id;
    identity.properties.pNext = &identity.pci;
    vkGetPhysicalDeviceProperties2(physical, &identity.properties);

    const bool has_uuid = std::any_of(
        std::begin(identity.id.deviceUUID), std::end(identity.id.deviceUUID),
        [](uint8_t value) { return value != 0; });
    if (has_uuid) {
        identity.key = "uuid:" + uuid_string(identity.id.deviceUUID, VK_UUID_SIZE);
    } else {
        std::ostringstream key;
        key << "pci:" << identity.pci.pciDomain << ':' << identity.pci.pciBus
            << ':' << identity.pci.pciDevice << ':' << identity.pci.pciFunction;
        identity.key = key.str();
    }
    return identity;
}

std::string uuid_string(const uint8_t* uuid, std::size_t size) {
    std::ostringstream out;
    for (std::size_t i = 0; i < size; ++i) {
        if (i != 0) out << ':';
        out << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(uuid[i]);
    }
    return out.str();
}

void print_cuda_devices() {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess) return;
    std::cout << "CUDA device map:\n";
    for (int index = 0; index < count; ++index) {
        cudaDeviceProp properties{};
        if (cudaGetDeviceProperties(&properties, index) != cudaSuccess) continue;
        std::cout << "  CUDA " << index << ": " << properties.name
                  << " PCI " << std::hex << std::setfill('0')
                  << std::setw(4) << properties.pciDomainID << ':'
                  << std::setw(2) << properties.pciBusID << ':'
                  << std::setw(2) << properties.pciDeviceID << std::dec
                  << " UUID=" << uuid_string(
                         reinterpret_cast<const uint8_t*>(properties.uuid.bytes),
                         sizeof(properties.uuid.bytes)) << "\n";
    }
}

std::optional<uint32_t> find_device_local_memory(VkPhysicalDevice physical,
                                                  VkMemoryRequirements requirements) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        const bool required = (requirements.memoryTypeBits & (1u << index)) != 0;
        const bool device_local =
            (properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        if (required && device_local) return index;
    }
    return std::nullopt;
}

bool create_vulkan_context(uint32_t physical_index, VkDeviceSize bytes,
                           VulkanContext* context) {
    uint32_t extension_count = 0;
    if (!vk_check(vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr),
                  "vkEnumerateInstanceExtensionProperties(count)")) return false;
    std::vector<VkExtensionProperties> instance_extensions(extension_count);
    if (!vk_check(vkEnumerateInstanceExtensionProperties(nullptr, &extension_count,
                                                         instance_extensions.data()),
                  "vkEnumerateInstanceExtensionProperties")) return false;

    std::vector<const char*> enabled_instance_extensions;
    if (has_extension(instance_extensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
        enabled_instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }
    if (has_extension(instance_extensions, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME)) {
        enabled_instance_extensions.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "DLSS5-MGPU Vulkan CUDA probe";
    app.applicationVersion = 1;
    app.pEngineName = "DLSS5-MGPU";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(enabled_instance_extensions.size());
    instance_info.ppEnabledExtensionNames = enabled_instance_extensions.data();
    if (!vk_check(vkCreateInstance(&instance_info, nullptr, &context->instance),
                  "vkCreateInstance")) return false;

    uint32_t raw_physical_count = 0;
    if (!vk_check(vkEnumeratePhysicalDevices(context->instance, &raw_physical_count, nullptr),
                  "vkEnumeratePhysicalDevices(count)")) return false;
    std::vector<VkPhysicalDevice> raw_physical_devices(raw_physical_count);
    if (!vk_check(vkEnumeratePhysicalDevices(context->instance, &raw_physical_count,
                                             raw_physical_devices.data()),
                  "vkEnumeratePhysicalDevices")) return false;

    std::vector<VkPhysicalDevice> physical_devices;
    std::vector<std::string> physical_keys;
    for (VkPhysicalDevice physical : raw_physical_devices) {
        const auto identity = get_physical_identity(physical);
        if (std::find(physical_keys.begin(), physical_keys.end(), identity.key) ==
            physical_keys.end()) {
            physical_keys.push_back(identity.key);
            physical_devices.push_back(physical);
        }
    }
    const uint32_t physical_count = static_cast<uint32_t>(physical_devices.size());
    if (physical_index >= physical_count) {
        std::cerr << "Unique Vulkan GPU index is outside the device list\n";
        return false;
    }
    context->physical = physical_devices[physical_index];

    std::cout << "Vulkan device map:\n";
    for (uint32_t index = 0; index < physical_count; ++index) {
        const auto identity = get_physical_identity(physical_devices[index]);
        std::cout << "  Vulkan " << index << ": " << identity.properties.properties.deviceName
                  << " UUID=" << uuid_string(identity.id.deviceUUID, VK_UUID_SIZE);
        if (identity.pci.pciDomain != 0 || identity.pci.pciBus != 0 || identity.pci.pciDevice != 0) {
            std::cout << " PCI=" << std::hex << identity.pci.pciDomain << ':'
                      << identity.pci.pciBus << ':' << identity.pci.pciDevice << '.'
                      << identity.pci.pciFunction << std::dec;
        }
        std::cout << '\n';
    }
    print_cuda_devices();

    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    VkPhysicalDeviceIDProperties id_info{};
    id_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci_info{};
    pci_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
    pci_info.pNext = &id_info;
    properties2.pNext = &pci_info;
    vkGetPhysicalDeviceProperties2(context->physical, &properties2);
    std::cout << "Vulkan GPU " << physical_index << ": "
              << properties2.properties.deviceName << "\n"
              << "  API " << VK_VERSION_MAJOR(properties2.properties.apiVersion) << '.'
              << VK_VERSION_MINOR(properties2.properties.apiVersion) << '.'
              << VK_VERSION_PATCH(properties2.properties.apiVersion) << "\n"
              << "  vendor=0x" << std::hex << properties2.properties.vendorID
              << " device=0x" << properties2.properties.deviceID << std::dec << "\n"
              << "  UUID=" << uuid_string(id_info.deviceUUID, VK_UUID_SIZE) << "\n";
    if (has_extension(instance_extensions, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)) {
        std::cout << "  PCI=" << pci_info.pciDomain << ':' << pci_info.pciBus << ':'
                  << pci_info.pciDevice << '.' << pci_info.pciFunction << "\n";
    }

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context->physical, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(context->physical, &queue_count, queues.data());
    bool found_queue = false;
    for (uint32_t index = 0; index < queue_count; ++index) {
        if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 ||
            (queues[index].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0) {
            context->queue_family = index;
            found_queue = true;
            break;
        }
    }
    if (!found_queue) {
        std::cerr << "No graphics or transfer queue found\n";
        return false;
    }

    uint32_t device_extension_count = 0;
    if (!vk_check(vkEnumerateDeviceExtensionProperties(context->physical, nullptr,
                                                       &device_extension_count, nullptr),
                  "vkEnumerateDeviceExtensionProperties(count)")) return false;
    std::vector<VkExtensionProperties> device_extensions(device_extension_count);
    if (!vk_check(vkEnumerateDeviceExtensionProperties(context->physical, nullptr,
                                                       &device_extension_count,
                                                       device_extensions.data()),
                  "vkEnumerateDeviceExtensionProperties")) return false;

    if (!has_extension(device_extensions, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) {
        std::cerr << "Vulkan device lacks VK_KHR_external_memory\n";
        return false;
    }
    if (!has_extension(device_extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        std::cerr << "Vulkan device lacks VK_KHR_external_memory_fd\n";
        return false;
    }

    const char* enabled_device_extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = context->queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 2;
    device_info.ppEnabledExtensionNames = enabled_device_extensions;
    if (!vk_check(vkCreateDevice(context->physical, &device_info, nullptr,
                                 &context->device),
                  "vkCreateDevice")) return false;
    vkGetDeviceQueue(context->device, context->queue_family, 0, &context->queue);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context->queue_family;
    if (!vk_check(vkCreateCommandPool(context->device, &pool_info, nullptr,
                                      &context->command_pool),
                  "vkCreateCommandPool")) return false;

    VkPhysicalDeviceExternalBufferInfo external_info{};
    external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    external_info.flags = 0;
    external_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalBufferProperties external_properties{};
    external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    vkGetPhysicalDeviceExternalBufferProperties(context->physical, &external_info,
                                                &external_properties);
    const auto features = external_properties.externalMemoryProperties.externalMemoryFeatures;
    if ((features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0) {
        std::cerr << "Vulkan device cannot export an opaque-fd buffer\n";
        return false;
    }

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = bytes;
    buffer_info.usage = external_info.usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!vk_check(vkCreateBuffer(context->device, &buffer_info, nullptr, &context->buffer),
                  "vkCreateBuffer")) return false;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context->device, context->buffer, &requirements);
    const auto memory_type = find_device_local_memory(context->physical, requirements);
    if (!memory_type) {
        std::cerr << "No device-local memory type supports the exportable buffer\n";
        return false;
    }

    VkExportMemoryAllocateInfo export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &export_info;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = *memory_type;
    if (!vk_check(vkAllocateMemory(context->device, &allocate_info, nullptr,
                                   &context->memory),
                  "vkAllocateMemory(exportable)")) return false;
    context->allocation_size = requirements.size;
    if (!vk_check(vkBindBufferMemory(context->device, context->buffer,
                                     context->memory, 0),
                  "vkBindBufferMemory")) return false;

    context->get_memory_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(context->device, "vkGetMemoryFdKHR"));
    if (!context->get_memory_fd) {
        std::cerr << "vkGetMemoryFdKHR is unavailable\n";
        return false;
    }
    return true;
}

bool fill_and_wait(VulkanContext* context) {
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (!vk_check(vkAllocateCommandBuffers(context->device, &allocation, &command),
                  "vkAllocateCommandBuffers")) return false;

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vk_check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer")) return false;
    vkCmdFillBuffer(command, context->buffer, 0, context->allocation_size, 0xA5A5A5A5u);
    if (!vk_check(vkEndCommandBuffer(command), "vkEndCommandBuffer")) return false;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (!vk_check(vkCreateFence(context->device, &fence_info, nullptr, &fence),
                  "vkCreateFence")) return false;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    const VkResult submit_result = vkQueueSubmit(context->queue, 1, &submit, fence);
    if (!vk_check(submit_result, "vkQueueSubmit") ||
        !vk_check(vkWaitForFences(context->device, 1, &fence, VK_TRUE,
                                  UINT64_MAX),
                  "vkWaitForFences")) {
        vkDestroyFence(context->device, fence, nullptr);
        return false;
    }
    vkDestroyFence(context->device, fence, nullptr);
    vkFreeCommandBuffers(context->device, context->command_pool, 1, &command);
    return true;
}

void destroy_context(VulkanContext* context) {
    if (context->exported_fd >= 0) close(context->exported_fd);
    if (context->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(context->device);
        if (context->buffer != VK_NULL_HANDLE) vkDestroyBuffer(context->device, context->buffer, nullptr);
        if (context->memory != VK_NULL_HANDLE) vkFreeMemory(context->device, context->memory, nullptr);
        if (context->command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(context->device, context->command_pool, nullptr);
        vkDestroyDevice(context->device, nullptr);
    }
    if (context->instance != VK_NULL_HANDLE) vkDestroyInstance(context->instance, nullptr);
    *context = {};
}

bool cuda_import_and_copy(VulkanContext& context, const Options& options) {
    cudaError_t status = cudaSetDevice(options.cuda_source);
    if (status != cudaSuccess) {
        std::cerr << "cudaSetDevice(source): " << cudaGetErrorString(status) << '\n';
        return false;
    }

    cudaExternalMemoryHandleDesc handle{};
    handle.type = cudaExternalMemoryHandleTypeOpaqueFd;
    handle.handle.fd = context.exported_fd;
    handle.size = context.allocation_size;
    cudaExternalMemory_t imported = nullptr;
    status = cudaImportExternalMemory(&imported, &handle);
    if (status != cudaSuccess) {
        std::cerr << "cudaImportExternalMemory: " << cudaGetErrorName(status)
                  << " - " << cudaGetErrorString(status) << '\n';
        return false;
    }
    // CUDA takes ownership of an imported opaque-fd handle on successful import.
    // Do not close the same descriptor again from VulkanContext cleanup.
    context.exported_fd = -1;

    cudaExternalMemoryBufferDesc mapped_desc{};
    mapped_desc.offset = 0;
    mapped_desc.size = options.bytes;
    unsigned char* mapped = nullptr;
    status = cudaExternalMemoryGetMappedBuffer(
        reinterpret_cast<void**>(&mapped), imported, &mapped_desc);
    if (status != cudaSuccess) {
        std::cerr << "cudaExternalMemoryGetMappedBuffer: " << cudaGetErrorString(status) << '\n';
        cudaDestroyExternalMemory(imported);
        return false;
    }

    unsigned char* destination = nullptr;
    status = cudaSetDevice(options.cuda_destination);
    if (status != cudaSuccess ||
        (status = cudaMalloc(reinterpret_cast<void**>(&destination), options.bytes)) != cudaSuccess) {
        std::cerr << "cudaMalloc(destination): " << cudaGetErrorString(status) << '\n';
        cudaDestroyExternalMemory(imported);
        return false;
    }

    int peer_possible = 0;
    status = cudaDeviceCanAccessPeer(&peer_possible, options.cuda_destination,
                                     options.cuda_source);
    if (status != cudaSuccess || !peer_possible) {
        std::cerr << "CUDA peer access is unavailable for the selected devices\n";
        cudaFree(destination);
        cudaDestroyExternalMemory(imported);
        return false;
    }
    cudaSetDevice(options.cuda_destination);
    status = cudaDeviceEnablePeerAccess(options.cuda_source, 0);
    if (status != cudaSuccess && status != cudaErrorPeerAccessAlreadyEnabled) {
        std::cerr << "cudaDeviceEnablePeerAccess: " << cudaGetErrorString(status) << '\n';
        cudaFree(destination);
        cudaDestroyExternalMemory(imported);
        return false;
    }
    cudaGetLastError();

    cudaStream_t stream = nullptr;
    status = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    const auto start = std::chrono::steady_clock::now();
    if (status == cudaSuccess) {
        status = cudaMemcpyPeerAsync(destination, options.cuda_destination,
                                     mapped, options.cuda_source, options.bytes, stream);
    }
    if (status == cudaSuccess) status = cudaStreamSynchronize(stream);
    const auto end = std::chrono::steady_clock::now();
    if (status != cudaSuccess) {
        std::cerr << "cudaMemcpyPeerAsync(imported Vulkan memory): "
                  << cudaGetErrorString(status) << '\n';
        if (stream) cudaStreamDestroy(stream);
        cudaFree(destination);
        cudaDestroyExternalMemory(imported);
        return false;
    }

    const double seconds = std::chrono::duration<double>(end - start).count();
    std::vector<unsigned char> host(options.bytes);
    status = cudaMemcpy(host.data(), destination, options.bytes, cudaMemcpyDeviceToHost);
    if (status != cudaSuccess) {
        std::cerr << "cudaMemcpy(validation): " << cudaGetErrorString(status) << '\n';
        cudaStreamDestroy(stream);
        cudaFree(destination);
        cudaDestroyExternalMemory(imported);
        return false;
    }
    const bool valid = std::all_of(host.begin(), host.end(),
                                   [](unsigned char value) { return value == 0xA5; });
    std::cout << "CUDA imported Vulkan allocation successfully\n"
              << "  allocation_size=" << context.allocation_size << " bytes\n"
              << "  validation=" << (valid ? "ok" : "FAIL") << "\n"
              << "  P2P copy=" << std::fixed << std::setprecision(3)
              << (static_cast<double>(options.bytes) / seconds / 1.0e9)
              << " GB/s\n";

    cudaStreamDestroy(stream);
    cudaFree(destination);
    cudaDestroyExternalMemory(imported);
    return valid;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        print_usage(argv[0]);
        return 2;
    }

    int cuda_count = 0;
    cudaError_t cuda_status = cudaGetDeviceCount(&cuda_count);
    if (cuda_status != cudaSuccess) {
        std::cerr << "cudaGetDeviceCount: " << cudaGetErrorString(cuda_status) << '\n';
        return 1;
    }
    if (options.cuda_source >= cuda_count || options.cuda_destination >= cuda_count) {
        std::cerr << "CUDA GPU index is outside the device list\n";
        return 2;
    }

    VulkanContext context;
    const bool created = create_vulkan_context(options.vulkan_gpu, options.bytes, &context);
    bool success = false;
    if (created && fill_and_wait(&context)) {
        VkMemoryGetFdInfoKHR fd_info{};
        fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fd_info.memory = context.memory;
        fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (vk_check(context.get_memory_fd(context.device, &fd_info, &context.exported_fd),
                     "vkGetMemoryFdKHR")) {
            success = cuda_import_and_copy(context, options);
        }
    }
    destroy_context(&context);
    return success ? 0 : 1;
}
