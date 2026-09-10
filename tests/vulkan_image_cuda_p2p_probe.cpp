#include <cuda_runtime.h>
#include <vulkan/vulkan.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;

struct Context {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocation_size = 0;
    int exported_fd = -1;
};

const char* vk_name(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    default: return "VK_OTHER";
    }
}

bool check_vk(VkResult result, const char* stage) {
    if (result == VK_SUCCESS) return true;
    std::cerr << stage << ": " << vk_name(result) << " code="
              << static_cast<int>(result) << "\n";
    return false;
}

bool check_cuda(cudaError_t result, const char* stage) {
    if (result == cudaSuccess) return true;
    std::cerr << stage << ": " << cudaGetErrorName(result) << " - "
              << cudaGetErrorString(result) << "\n";
    return false;
}

bool has_extension(VkPhysicalDevice physical, const char* wanted) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, wanted) == 0) return true;
    }
    return false;
}

std::vector<VkPhysicalDevice> nvidia_devices(VkInstance instance) {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS)
        return {};
    std::vector<VkPhysicalDevice> all(count);
    if (vkEnumeratePhysicalDevices(instance, &count, all.data()) != VK_SUCCESS)
        return {};

    std::vector<VkPhysicalDevice> result;
    std::array<std::array<uint8_t, VK_UUID_SIZE>, 16> uuids{};
    std::array<std::array<uint32_t, 4>, 16> pci{};
    for (VkPhysicalDevice physical : all) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (properties.vendorID != 0x10de ||
            properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            continue;

        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        VkPhysicalDeviceIDProperties id{};
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDevicePCIBusInfoPropertiesEXT pci_info{};
        pci_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
        properties2.pNext = &id;
        id.pNext = &pci_info;
        vkGetPhysicalDeviceProperties2(physical, &properties2);

        bool duplicate = false;
        for (size_t i = 0; i < result.size(); ++i) {
            if (std::memcmp(uuids[i].data(), id.deviceUUID, VK_UUID_SIZE) == 0 ||
                (pci[i][0] == pci_info.pciDomain && pci[i][1] == pci_info.pciBus &&
                 pci[i][2] == pci_info.pciDevice && pci[i][3] == pci_info.pciFunction)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate || result.size() == uuids.size()) continue;
        std::memcpy(uuids[result.size()].data(), id.deviceUUID, VK_UUID_SIZE);
        pci[result.size()] = {pci_info.pciDomain, pci_info.pciBus,
                              pci_info.pciDevice, pci_info.pciFunction};
        std::cerr << "image_cuda_p2p physical=" << result.size()
                  << " name=" << properties.deviceName
                  << " pci=" << pci_info.pciDomain << ':' << pci_info.pciBus << ':'
                  << pci_info.pciDevice << '.' << pci_info.pciFunction << '\n';
        result.push_back(physical);
    }
    return result;
}

bool create_context(VkPhysicalDevice physical, Context* context) {
    context->physical = physical;
    if (!has_extension(physical, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) ||
        !has_extension(physical, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        std::cerr << "missing external memory extensions\n";
        return false;
    }
    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queues.data());
    for (uint32_t i = 0; i < queue_count; ++i) {
        if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            context->queue_family = i;
            break;
        }
    }
    if (context->queue_family == UINT32_MAX) return false;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = context->queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    const char* extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 2;
    device_info.ppEnabledExtensionNames = extensions;
    if (!check_vk(vkCreateDevice(physical, &device_info, nullptr, &context->device),
                  "vkCreateDevice")) return false;
    vkGetDeviceQueue(context->device, context->queue_family, 0, &context->queue);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context->queue_family;
    return check_vk(vkCreateCommandPool(context->device, &pool_info, nullptr,
                                         &context->command_pool), "vkCreateCommandPool");
}

uint32_t find_memory_type(VkPhysicalDevice physical, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    }
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

bool create_exportable_image(Context* context) {
    VkExternalMemoryImageCreateInfo external_image{};
    external_image.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &external_image;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    image_info.extent = {kWidth, kHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check_vk(vkCreateImage(context->device, &image_info, nullptr, &context->image),
                  "vkCreateImage")) return false;

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context->device, context->image, &requirements);
    context->allocation_size = requirements.size;
    const uint32_t memory_type = find_memory_type(context->physical, requirements.memoryTypeBits);
    if (memory_type == UINT32_MAX) return false;
    VkExportMemoryAllocateInfo export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.pNext = &export_info;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type;
    if (!check_vk(vkAllocateMemory(context->device, &allocate_info, nullptr,
                                   &context->memory), "vkAllocateMemory")) return false;
    if (!check_vk(vkBindImageMemory(context->device, context->image, context->memory, 0),
                  "vkBindImageMemory")) return false;

    auto get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(context->device, "vkGetMemoryFdKHR"));
    if (!get_fd) return false;
    VkMemoryGetFdInfoKHR fd_info{};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.memory = context->memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    return check_vk(get_fd(context->device, &fd_info, &context->exported_fd),
                    "vkGetMemoryFdKHR");
}

bool submit(Context* context, const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context->command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (!check_vk(vkAllocateCommandBuffers(context->device, &allocation, &command),
                  "vkAllocateCommandBuffers")) return false;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check_vk(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer")) return false;
    record(command);
    if (!check_vk(vkEndCommandBuffer(command), "vkEndCommandBuffer")) return false;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (!check_vk(vkCreateFence(context->device, &fence_info, nullptr, &fence), "vkCreateFence"))
        return false;
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command;
    VkResult result = vkQueueSubmit(context->queue, 1, &submit_info, fence);
    if (result == VK_SUCCESS)
        result = vkWaitForFences(context->device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(context->device, fence, nullptr);
    vkFreeCommandBuffers(context->device, context->command_pool, 1, &command);
    return check_vk(result, "submit/wait");
}

bool clear_to_general(Context* context) {
    return submit(context, [context](VkCommandBuffer command) {
        VkImageMemoryBarrier to_dst{};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.srcAccessMask = 0;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.image = context->image;
        to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_dst.subresourceRange.levelCount = 1;
        to_dst.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &to_dst);
        VkClearColorValue clear{{0.25f, 0.5f, 0.75f, 1.0f}};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(command, context->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &range);
        VkImageMemoryBarrier to_general = to_dst;
        to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_general.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &to_general);
    });
}

bool readback(Context* context, std::array<uint8_t, 8>* bytes) {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = 8;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check_vk(vkCreateBuffer(context->device, &buffer_info, nullptr, &buffer),
                  "vkCreateBuffer(readback)")) return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context->device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(context->physical, &properties);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (!check_vk(vkAllocateMemory(context->device, &allocate, nullptr, &memory),
                  "vkAllocateMemory(readback)")) return false;
    if (!check_vk(vkBindBufferMemory(context->device, buffer, memory, 0),
                  "vkBindBufferMemory(readback)")) return false;

    bool result = submit(context, [context, buffer](VkCommandBuffer command) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.image = context->image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(command, context->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffer, 1, &copy);
    });
    if (result) {
        void* mapped = nullptr;
        result = check_vk(vkMapMemory(context->device, memory, 0, 8, 0, &mapped),
                          "vkMapMemory(readback)");
        if (result) {
            std::memcpy(bytes->data(), mapped, bytes->size());
            vkUnmapMemory(context->device, memory);
        }
    }
    vkDestroyBuffer(context->device, buffer, nullptr);
    vkFreeMemory(context->device, memory, nullptr);
    return result;
}

void destroy_context(Context* context) {
    if (context->exported_fd >= 0) close(context->exported_fd);
    if (context->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(context->device);
        if (context->image) vkDestroyImage(context->device, context->image, nullptr);
        if (context->memory) vkFreeMemory(context->device, context->memory, nullptr);
        if (context->command_pool) vkDestroyCommandPool(context->device, context->command_pool, nullptr);
        vkDestroyDevice(context->device, nullptr);
    }
    *context = {};
}

bool cuda_copy(Context* source, Context* destination, int source_index, int destination_index) {
    if (!check_cuda(cudaSetDevice(source_index), "cudaSetDevice(source)")) return false;
    int peer = 0;
    if (!check_cuda(cudaDeviceCanAccessPeer(&peer, destination_index, source_index),
                    "cudaDeviceCanAccessPeer") || !peer) {
        std::cerr << "CUDA peer access unavailable\n";
        return false;
    }

    cudaExternalMemoryHandleDesc source_handle{};
    source_handle.type = cudaExternalMemoryHandleTypeOpaqueFd;
    source_handle.handle.fd = source->exported_fd;
    source_handle.size = source->allocation_size;
    cudaExternalMemory_t source_memory = nullptr;
    if (!check_cuda(cudaImportExternalMemory(&source_memory, &source_handle),
                    "cudaImportExternalMemory(source image)")) return false;
    source->exported_fd = -1;
    cudaExternalMemoryBufferDesc source_buffer{};
    source_buffer.size = source->allocation_size;
    void* source_mapped = nullptr;
    if (!check_cuda(cudaExternalMemoryGetMappedBuffer(&source_mapped, source_memory,
                                                       &source_buffer),
                    "cudaExternalMemoryGetMappedBuffer(source image)")) {
        cudaDestroyExternalMemory(source_memory);
        return false;
    }

    if (!check_cuda(cudaSetDevice(destination_index), "cudaSetDevice(destination)")) {
        cudaDestroyExternalMemory(source_memory);
        return false;
    }
    cudaExternalMemoryHandleDesc destination_handle{};
    destination_handle.type = cudaExternalMemoryHandleTypeOpaqueFd;
    destination_handle.handle.fd = destination->exported_fd;
    destination_handle.size = destination->allocation_size;
    cudaExternalMemory_t destination_memory = nullptr;
    if (!check_cuda(cudaImportExternalMemory(&destination_memory, &destination_handle),
                    "cudaImportExternalMemory(destination image)")) {
        cudaDestroyExternalMemory(source_memory);
        return false;
    }
    destination->exported_fd = -1;
    cudaExternalMemoryBufferDesc destination_buffer{};
    destination_buffer.size = destination->allocation_size;
    void* destination_mapped = nullptr;
    if (!check_cuda(cudaExternalMemoryGetMappedBuffer(&destination_mapped, destination_memory,
                                                       &destination_buffer),
                    "cudaExternalMemoryGetMappedBuffer(destination image)")) {
        cudaDestroyExternalMemory(destination_memory);
        cudaDestroyExternalMemory(source_memory);
        return false;
    }
    if (source->allocation_size != destination->allocation_size) {
        std::cerr << "image allocation sizes differ source=" << source->allocation_size
                  << " destination=" << destination->allocation_size << "\n";
        cudaDestroyExternalMemory(destination_memory);
        cudaDestroyExternalMemory(source_memory);
        return false;
    }
    cudaError_t result = cudaDeviceEnablePeerAccess(source_index, 0);
    if (result != cudaSuccess && result != cudaErrorPeerAccessAlreadyEnabled) {
        check_cuda(result, "cudaDeviceEnablePeerAccess");
        cudaDestroyExternalMemory(destination_memory);
        cudaDestroyExternalMemory(source_memory);
        return false;
    }
    cudaGetLastError();
    result = cudaMemcpyPeer(destination_mapped, destination_index, source_mapped,
                            source_index, source->allocation_size);
    bool ok = check_cuda(result, "cudaMemcpyPeer(image allocation)");
    if (ok) ok = check_cuda(cudaDeviceSynchronize(), "cudaDeviceSynchronize(destination)");
    cudaDestroyExternalMemory(destination_memory);
    cudaDestroyExternalMemory(source_memory);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    int source_index = 0;
    int destination_index = 1;
    if (argc == 3) {
        source_index = std::atoi(argv[1]);
        destination_index = std::atoi(argv[2]);
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " [source-nvidia-ordinal destination-nvidia-ordinal]\n";
        return 2;
    }
    if (source_index < 0 || destination_index < 0 || source_index == destination_index) {
        std::cerr << "source and destination must be different non-negative ordinals\n";
        return 2;
    }
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (!check_vk(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance")) return 2;
    const auto physical = nvidia_devices(instance);
    if (static_cast<size_t>(std::max(source_index, destination_index)) >= physical.size()) {
        std::cerr << "need two unique NVIDIA devices\n";
        vkDestroyInstance(instance, nullptr);
        return 3;
    }
    Context source, destination;
    bool ok = create_context(physical[source_index], &source) &&
              create_context(physical[destination_index], &destination);
    if (ok) ok = create_exportable_image(&source) && create_exportable_image(&destination);
    if (ok) ok = clear_to_general(&source) && clear_to_general(&destination);
    const VkDeviceSize source_size = source.allocation_size;
    const VkDeviceSize destination_size = destination.allocation_size;
    if (ok) ok = cuda_copy(&source, &destination, source_index, destination_index);
    std::array<uint8_t, 8> bytes{};
    if (ok) ok = readback(&destination, &bytes);
    const std::array<uint8_t, 8> expected = {0x00, 0x34, 0x00, 0x38,
                                              0x00, 0x3a, 0x00, 0x3c};
    const bool readback_ok = bytes == expected;
    ok = ok && readback_ok;
    std::cout << "{\"source\":" << source_index
              << ",\"destination\":" << destination_index
              << ",\"source_allocation_size\":" << source_size
              << ",\"destination_allocation_size\":" << destination_size
              << ",\"cuda_image_allocation_p2p\":" << (ok ? "true" : "false")
              << ",\"readback_ok\":" << (readback_ok ? "true" : "false") << "}\n";
    destroy_context(&destination);
    destroy_context(&source);
    vkDestroyInstance(instance, nullptr);
    return ok ? 0 : 1;
}
