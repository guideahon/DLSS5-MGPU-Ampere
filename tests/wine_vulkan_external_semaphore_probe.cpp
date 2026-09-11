#include <vulkan/vulkan.h>

#include <cstdio>
#include <io.h>
#include <string>
#include <vector>

int main()
{
    uint32_t instance_count = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &instance_count, nullptr);
    std::printf("instance_extensions_result=%d count=%u\n", result, instance_count);
    if (result != VK_SUCCESS)
        return 1;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "dlss5-wine-vulkan-external-semaphore-probe";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    result = vkCreateInstance(&instance_info, nullptr, &instance);
    std::printf("create_instance_result=%d\n", result);
    if (result != VK_SUCCESS)
        return 2;

    uint32_t physical_count = 0;
    result = vkEnumeratePhysicalDevices(instance, &physical_count, nullptr);
    std::printf("physical_devices_result=%d count=%u\n", result, physical_count);
    if (result != VK_SUCCESS || physical_count == 0)
        return 3;
    std::vector<VkPhysicalDevice> physical(physical_count);
    vkEnumeratePhysicalDevices(instance, &physical_count, physical.data());

    uint32_t selected_physical = UINT32_MAX;
    bool saw_extension = false;
    for (uint32_t i = 0; i < physical_count; ++i) {
        uint32_t extension_count = 0;
        result = vkEnumerateDeviceExtensionProperties(physical[i], nullptr,
                &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (result == VK_SUCCESS)
            vkEnumerateDeviceExtensionProperties(physical[i], nullptr,
                    &extension_count, extensions.data());
        bool has_extension = false;
        for (const auto &extension : extensions) {
            if (std::string(extension.extensionName) ==
                    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)
                has_extension = true;
        }
        std::printf("physical=%u device_extensions_result=%d count=%u "
                    "external_semaphore_fd=%s\n", i, result, extension_count,
                    has_extension ? "yes" : "no");
        saw_extension |= has_extension;
        if (selected_physical == UINT32_MAX && has_extension)
            selected_physical = i;
    }

    bool saw_proc = false;
    bool exported_fd = false;
    VkDevice device = VK_NULL_HANDLE;
    if (selected_physical != UINT32_MAX) {
        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical[selected_physical],
                &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical[selected_physical],
                &queue_count, queues.data());
        uint32_t queue_family = UINT32_MAX;
        for (uint32_t i = 0; i < queue_count; ++i)
            if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                queue_family = i;
                break;
            }
        if (queue_family == UINT32_MAX)
            for (uint32_t i = 0; i < queue_count; ++i)
                if (queues[i].queueCount) { queue_family = i; break; }

        if (queue_family != UINT32_MAX) {
            float priority = 1.0f;
            VkDeviceQueueCreateInfo queue_info{};
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info.queueFamilyIndex = queue_family;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &priority;
            const char *extension_name = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
            VkDeviceCreateInfo device_info{};
            device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            device_info.queueCreateInfoCount = 1;
            device_info.pQueueCreateInfos = &queue_info;
            device_info.enabledExtensionCount = 1;
            device_info.ppEnabledExtensionNames = &extension_name;
            result = vkCreateDevice(physical[selected_physical], &device_info,
                    nullptr, &device);
            std::printf("create_device_result=%d selected_physical=%u\n",
                    result, selected_physical);
            if (result == VK_SUCCESS) {
                auto get_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
                        vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
                saw_proc = get_fd != nullptr;
                if (get_fd) {
                    VkExportSemaphoreCreateInfo export_info{};
                    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
                    export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
                    VkSemaphoreCreateInfo semaphore_info{};
                    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                    semaphore_info.pNext = &export_info;
                    VkSemaphore semaphore = VK_NULL_HANDLE;
                    VkResult create_semaphore = vkCreateSemaphore(device,
                            &semaphore_info, nullptr, &semaphore);
                    int fd = -1;
                    VkSemaphoreGetFdInfoKHR fd_info{};
                    fd_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
                    fd_info.semaphore = semaphore;
                    fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
                    VkResult export_result = create_semaphore == VK_SUCCESS
                            ? get_fd(device, &fd_info, &fd) : create_semaphore;
                    exported_fd = export_result == VK_SUCCESS && fd >= 0;
                    std::printf("create_semaphore_result=%d export_fd_result=%d fd=%d\n",
                            create_semaphore, export_result, fd);
                    if (fd >= 0) _close(fd);
                    if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
                }
            }
        }
    }

    std::printf("vkGetSemaphoreFdKHR_proc=%s exported_fd=%s\n",
            saw_proc ? "yes" : "no", exported_fd ? "yes" : "no");
    if (device) vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return saw_extension && saw_proc && exported_fd ? 0 : 4;
}
