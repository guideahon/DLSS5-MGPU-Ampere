#include <cuda_runtime_api.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

struct Options {
    uint32_t vulkan_gpu = 0;
    int cuda_device = 1;
    bool json = false;
};

struct Context {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkCommandPool command_pool = VK_NULL_HANDLE;
};

struct DirectionResult {
    bool attempted = false;
    bool export_ok = false;
    bool import_ok = false;
    bool sync_ok = false;
    std::string error;
};

bool parse_int(const char* value, int* output) {
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (!value || *value == '\0' || !end || *end != '\0') return false;
    *output = static_cast<int>(parsed);
    return true;
}

bool parse_u32(const char* value, uint32_t* output) {
    int parsed = 0;
    if (!parse_int(value, &parsed) || parsed < 0) return false;
    *output = static_cast<uint32_t>(parsed);
    return true;
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions,
                   const char* name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [name](const VkExtensionProperties& extension) {
                           return std::strcmp(extension.extensionName, name) == 0;
                       });
}

std::string vk_result(VkResult result) {
    std::ostringstream output;
    output << "VK_RESULT(" << static_cast<int>(result) << ")";
    return output.str();
}

std::string cuda_result(cudaError_t result) {
    std::ostringstream output;
    output << cudaGetErrorName(result) << "(" << cudaGetErrorString(result) << ")";
    return output.str();
}

void usage(const char* program) {
    std::cout << "Usage: " << program
              << " [--vulkan-gpu N] [--cuda-device N] [--json]\n";
}

bool parse_options(int argc, char** argv, Options* options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--json") {
            options->json = true;
            continue;
        }
        if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        }
        if (index + 1 >= argc) return false;
        if (argument == "--vulkan-gpu") {
            if (!parse_u32(argv[++index], &options->vulkan_gpu)) return false;
        } else if (argument == "--cuda-device") {
            if (!parse_int(argv[++index], &options->cuda_device)) return false;
        } else {
            return false;
        }
    }
    return options->cuda_device >= 0;
}

void destroy_context(Context* context) {
    if (context->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(context->device);
        if (context->command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(context->device, context->command_pool, nullptr);
        vkDestroyDevice(context->device, nullptr);
    }
    if (context->instance != VK_NULL_HANDLE)
        vkDestroyInstance(context->instance, nullptr);
    *context = {};
}

bool create_context(uint32_t requested_gpu, Context* context, std::string* error) {
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "DLSS5-MGPU external semaphore probe";
    application.applicationVersion = 1;
    application.pEngineName = "DLSS5-MGPU";
    application.engineVersion = 1;
    application.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    if (vkCreateInstance(&instance_info, nullptr, &context->instance) != VK_SUCCESS) {
        if (error) *error = "vkCreateInstance";
        return false;
    }

    uint32_t physical_count = 0;
    if (vkEnumeratePhysicalDevices(context->instance, &physical_count, nullptr) != VK_SUCCESS) {
        if (error) *error = "vkEnumeratePhysicalDevices(count)";
        return false;
    }
    std::vector<VkPhysicalDevice> physicals(physical_count);
    if (vkEnumeratePhysicalDevices(context->instance, &physical_count, physicals.data()) != VK_SUCCESS) {
        if (error) *error = "vkEnumeratePhysicalDevices";
        return false;
    }
    std::vector<VkPhysicalDevice> unique;
    std::vector<std::string> uuids;
    for (VkPhysicalDevice physical : physicals) {
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        VkPhysicalDeviceIDProperties identity{};
        identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        properties.pNext = &identity;
        vkGetPhysicalDeviceProperties2(physical, &properties);
        std::ostringstream uuid;
        for (uint8_t value : identity.deviceUUID)
            uuid << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned int>(value);
        if (std::find(uuids.begin(), uuids.end(), uuid.str()) == uuids.end()) {
            uuids.push_back(uuid.str());
            unique.push_back(physical);
        }
    }
    if (requested_gpu >= unique.size()) {
        if (error) *error = "Vulkan GPU index is outside unique physical devices";
        return false;
    }
    context->physical = unique[requested_gpu];

    uint32_t extension_count = 0;
    if (vkEnumerateDeviceExtensionProperties(context->physical, nullptr,
                                             &extension_count, nullptr) != VK_SUCCESS) {
        if (error) *error = "vkEnumerateDeviceExtensionProperties(count)";
        return false;
    }
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (vkEnumerateDeviceExtensionProperties(context->physical, nullptr,
                                             &extension_count, extensions.data()) != VK_SUCCESS) {
        if (error) *error = "vkEnumerateDeviceExtensionProperties";
        return false;
    }
    if (!has_extension(extensions, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) ||
        !has_extension(extensions, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
        if (error) *error = "VK_KHR_external_semaphore(_fd) unavailable";
        return false;
    }

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context->physical, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(context->physical, &queue_count, queues.data());
    bool queue_found = false;
    for (uint32_t index = 0; index < queue_count; ++index) {
        if (queues[index].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                                        VK_QUEUE_TRANSFER_BIT)) {
            context->queue_family = index;
            queue_found = true;
            break;
        }
    }
    if (!queue_found) {
        if (error) *error = "no Vulkan queue family";
        return false;
    }

    const char* device_extensions[] = {
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
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
    device_info.ppEnabledExtensionNames = device_extensions;
    if (vkCreateDevice(context->physical, &device_info, nullptr, &context->device) != VK_SUCCESS) {
        if (error) *error = "vkCreateDevice";
        return false;
    }
    vkGetDeviceQueue(context->device, context->queue_family, 0, &context->queue);
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context->queue_family;
    if (vkCreateCommandPool(context->device, &pool_info, nullptr,
                            &context->command_pool) != VK_SUCCESS) {
        if (error) *error = "vkCreateCommandPool";
        return false;
    }
    return true;
}

bool make_empty_command(Context& context, VkCommandBuffer* result) {
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = context.command_pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(context.device, &allocation, result) != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(*result, &begin) == VK_SUCCESS &&
           vkEndCommandBuffer(*result) == VK_SUCCESS;
}

bool create_exported_semaphore(Context& context, VkSemaphore* semaphore,
                               int* fd, std::string* error) {
    VkExportSemaphoreCreateInfo export_info{};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore_info.pNext = &export_info;
    VkResult result = vkCreateSemaphore(context.device, &semaphore_info, nullptr, semaphore);
    if (result != VK_SUCCESS) {
        if (error) *error = "vkCreateSemaphore " + vk_result(result);
        return false;
    }
    auto get_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(context.device, "vkGetSemaphoreFdKHR"));
    if (!get_fd) {
        if (error) *error = "vkGetSemaphoreFdKHR unavailable";
        return false;
    }
    VkSemaphoreGetFdInfoKHR fd_info{};
    fd_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    fd_info.semaphore = *semaphore;
    fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    result = get_fd(context.device, &fd_info, fd);
    if (result != VK_SUCCESS) {
        if (error) *error = "vkGetSemaphoreFdKHR " + vk_result(result);
        return false;
    }
    return true;
}

DirectionResult vulkan_signal_cuda_wait(Context& context, int cuda_device) {
    DirectionResult result;
    result.attempted = true;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    int fd = -1;
    if (!create_exported_semaphore(context, &semaphore, &fd, &result.error))
        return result;
    result.export_ok = true;

    cudaError_t cuda_status = cudaSetDevice(cuda_device);
    cudaExternalSemaphore_t external = nullptr;
    if (cuda_status == cudaSuccess) {
        cudaExternalSemaphoreHandleDesc import_desc{};
        import_desc.type = cudaExternalSemaphoreHandleTypeOpaqueFd;
        import_desc.handle.fd = fd;
        cuda_status = cudaImportExternalSemaphore(&external, &import_desc);
    }
    if (cuda_status != cudaSuccess) {
        if (fd >= 0) close(fd);
        result.error = "cudaImportExternalSemaphore(wait) " + cuda_result(cuda_status);
        vkDestroySemaphore(context.device, semaphore, nullptr);
        return result;
    }
    result.import_ok = true;

    VkCommandBuffer command = VK_NULL_HANDLE;
    const bool command_ok = make_empty_command(context, &command);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = command_ok ? 1u : 0u;
    submit.pCommandBuffers = command_ok ? &command : nullptr;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &semaphore;
    VkResult vk_status = command_ok ? vkQueueSubmit(context.queue, 1, &submit, VK_NULL_HANDLE)
                                    : VK_ERROR_INITIALIZATION_FAILED;
    cudaStream_t stream = nullptr;
    if (vk_status == VK_SUCCESS)
        cuda_status = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    cudaExternalSemaphoreWaitParams wait{};
    if (vk_status == VK_SUCCESS && cuda_status == cudaSuccess)
        cuda_status = cudaWaitExternalSemaphoresAsync(&external, &wait, 1, stream);
    if (vk_status == VK_SUCCESS && cuda_status == cudaSuccess)
        cuda_status = cudaStreamSynchronize(stream);
    result.sync_ok = vk_status == VK_SUCCESS && cuda_status == cudaSuccess;
    if (!result.sync_ok) {
        result.error = "signal Vulkan -> wait CUDA vk=" + vk_result(vk_status) +
                       " cuda=" + cuda_result(cuda_status);
    }
    if (stream) cudaStreamDestroy(stream);
    if (external) cudaDestroyExternalSemaphore(external);
    if (command) vkFreeCommandBuffers(context.device, context.command_pool, 1, &command);
    vkDestroySemaphore(context.device, semaphore, nullptr);
    return result;
}

DirectionResult cuda_signal_vulkan_wait(Context& context, int cuda_device) {
    DirectionResult result;
    result.attempted = true;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    int fd = -1;
    if (!create_exported_semaphore(context, &semaphore, &fd, &result.error))
        return result;
    result.export_ok = true;
    cudaError_t cuda_status = cudaSetDevice(cuda_device);
    cudaExternalSemaphore_t external = nullptr;
    if (cuda_status == cudaSuccess) {
        cudaExternalSemaphoreHandleDesc import_desc{};
        import_desc.type = cudaExternalSemaphoreHandleTypeOpaqueFd;
        import_desc.handle.fd = fd;
        cuda_status = cudaImportExternalSemaphore(&external, &import_desc);
    }
    if (cuda_status != cudaSuccess) {
        if (fd >= 0) close(fd);
        result.error = "cudaImportExternalSemaphore(signal) " + cuda_result(cuda_status);
        vkDestroySemaphore(context.device, semaphore, nullptr);
        return result;
    }
    result.import_ok = true;
    cudaStream_t stream = nullptr;
    if (cuda_status == cudaSuccess)
        cuda_status = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking);
    cudaExternalSemaphoreSignalParams signal{};
    if (cuda_status == cudaSuccess)
        cuda_status = cudaSignalExternalSemaphoresAsync(&external, &signal, 1, stream);
    if (cuda_status == cudaSuccess) cuda_status = cudaStreamSynchronize(stream);

    VkCommandBuffer command = VK_NULL_HANDLE;
    const bool command_ok = make_empty_command(context, &command);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &semaphore;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = command_ok ? 1u : 0u;
    submit.pCommandBuffers = command_ok ? &command : nullptr;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkResult vk_status = VK_ERROR_INITIALIZATION_FAILED;
    if (cuda_status == cudaSuccess && command_ok &&
        vkCreateFence(context.device, &fence_info, nullptr, &fence) == VK_SUCCESS)
        vk_status = vkQueueSubmit(context.queue, 1, &submit, fence);
    if (vk_status == VK_SUCCESS)
        vk_status = vkWaitForFences(context.device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
    result.sync_ok = cuda_status == cudaSuccess && vk_status == VK_SUCCESS;
    if (!result.sync_ok) {
        result.error = "signal CUDA -> wait Vulkan vk=" + vk_result(vk_status) +
                       " cuda=" + cuda_result(cuda_status);
    }
    if (fence) vkDestroyFence(context.device, fence, nullptr);
    if (stream) cudaStreamDestroy(stream);
    if (external) cudaDestroyExternalSemaphore(external);
    if (command) vkFreeCommandBuffers(context.device, context.command_pool, 1, &command);
    vkDestroySemaphore(context.device, semaphore, nullptr);
    return result;
}

void print_direction(const char* name, const DirectionResult& result) {
    std::cout << name << " attempted=" << (result.attempted ? "true" : "false")
              << " export=" << (result.export_ok ? "true" : "false")
              << " import=" << (result.import_ok ? "true" : "false")
              << " sync=" << (result.sync_ok ? "true" : "false");
    if (!result.error.empty()) std::cout << " error=" << result.error;
    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return 2;
    }
    int cuda_count = 0;
    const cudaError_t cuda_status = cudaGetDeviceCount(&cuda_count);
    if (cuda_status != cudaSuccess || options.cuda_device >= cuda_count) {
        std::cerr << "CUDA device unavailable: " << cuda_result(cuda_status) << '\n';
        return 1;
    }
    Context context;
    std::string context_error;
    if (!create_context(options.vulkan_gpu, &context, &context_error)) {
        if (options.json) {
            std::cout << "{\"available\":false,\"error\":\""
                      << context_error << "\"}\n";
        } else {
            std::cerr << "Vulkan semaphore context unavailable: " << context_error << '\n';
        }
        destroy_context(&context);
        return 1;
    }
    const DirectionResult vulkan_to_cuda = vulkan_signal_cuda_wait(context, options.cuda_device);
    const DirectionResult cuda_to_vulkan = cuda_signal_vulkan_wait(context, options.cuda_device);
    const bool success = vulkan_to_cuda.sync_ok && cuda_to_vulkan.sync_ok;
    if (options.json) {
        std::cout << "{\"available\":true,\"vulkan_gpu\":" << options.vulkan_gpu
                  << ",\"cuda_device\":" << options.cuda_device
                  << ",\"vulkan_to_cuda\":" << (vulkan_to_cuda.sync_ok ? "true" : "false")
                  << ",\"cuda_to_vulkan\":" << (cuda_to_vulkan.sync_ok ? "true" : "false")
                  << ",\"vulkan_to_cuda_error\":\"" << vulkan_to_cuda.error
                  << "\",\"cuda_to_vulkan_error\":\"" << cuda_to_vulkan.error
                  << "\"}\n";
    } else {
        print_direction("vulkan_signal_cuda_wait", vulkan_to_cuda);
        print_direction("cuda_signal_vulkan_wait", cuda_to_vulkan);
    }
    destroy_context(&context);
    return success ? 0 : 1;
}
