#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <vulkan.h>
#include <stdio.h>
#include <stdlib.h>

struct vkd3d_device_ext;
struct vkd3d_device_ext_vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(vkd3d_device_ext *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(vkd3d_device_ext *);
    ULONG (STDMETHODCALLTYPE *Release)(vkd3d_device_ext *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHandles)(vkd3d_device_ext *, VkInstance *, VkPhysicalDevice *, VkDevice *);
    BOOL (STDMETHODCALLTYPE *GetExtensionSupport)(vkd3d_device_ext *, UINT);
};
struct vkd3d_device_ext { const vkd3d_device_ext_vtbl *lpVtbl; };

static const GUID IID_ID3D12DeviceExt =
    {0x11ea7a1a, 0x0f6a, 0x49bf, {0xb6, 0x12, 0x3e, 0x30, 0xf8, 0xe2, 0x01, 0xdd}};

static void log_vk(const char *label, VkResult result)
{
    fprintf(stderr, "%s: %d\n", label, (int)result);
}

static IDXGIAdapter1 *find_3090(IDXGIFactory4 *factory)
{
    int wanted = 0;
    const char *wanted_text = getenv("MGPU_D3D12_ADAPTER_INDEX");
    if (wanted_text && *wanted_text)
        wanted = atoi(wanted_text);
    int found = 0;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                wcsstr(desc.Description, L"RTX 3090")) {
            if (found++ == wanted)
                return adapter;
        }
        adapter->Release();
    }
    return nullptr;
}

static bool spawn_cuda_helper(int fd, VkDeviceSize size, const char *helper,
        int ordinal, int destination_ordinal)
{
    if (!helper || !*helper)
        return false;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using spawn_fn = LONG (WINAPI *)(char * const[], int);
    auto spawn = ntdll ? reinterpret_cast<spawn_fn>(
            GetProcAddress(ntdll, "__wine_unix_spawnvp")) : nullptr;
    if (!spawn)
        return false;

    char fd_text[32], size_text[32], ordinal_text[16], destination_text[16];
    snprintf(fd_text, sizeof(fd_text), "%d", fd);
    snprintf(size_text, sizeof(size_text), "%llu", (unsigned long long)size);
    snprintf(ordinal_text, sizeof(ordinal_text), "%d", ordinal);
    snprintf(destination_text, sizeof(destination_text), "%d", destination_ordinal);
    char *argv[] = {const_cast<char *>(helper), fd_text, size_text,
            ordinal_text, destination_text, nullptr};
    SetEnvironmentVariableA("MGPU_INHERIT_FD", fd_text);
    LONG rc = spawn(argv, 1);
    SetEnvironmentVariableA("MGPU_INHERIT_FD", nullptr);
    fprintf(stderr, "CUDA helper ordinal=%d fd=%d spawn_rc=%ld\n",
            ordinal, fd, (long)rc);
    return rc == 0;
}

int main()
{
    IDXGIFactory4 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { fprintf(stderr, "CreateDXGIFactory1 failed 0x%08lx\n", (unsigned long)hr); return 2; }
    IDXGIAdapter1 *adapter = find_3090(factory);
    ID3D12Device *d3d12 = nullptr;
    if (!adapter || FAILED(hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&d3d12)))) {
        fprintf(stderr, "D3D12CreateDevice failed 0x%08lx\n", (unsigned long)hr);
        if (adapter) adapter->Release();
        factory->Release();
        return 3;
    }

    vkd3d_device_ext *ext = nullptr;
    hr = d3d12->QueryInterface(IID_ID3D12DeviceExt, (void **)&ext);
    if (FAILED(hr) || !ext) { fprintf(stderr, "ID3D12DeviceExt failed 0x%08lx\n", (unsigned long)hr); return 4; }
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkResult vk_result = VK_SUCCESS;
    hr = ext->lpVtbl->GetVulkanHandles(ext, &instance, &physical, &device);
    ext->lpVtbl->Release(ext);
    if (FAILED(hr) || !instance || !physical || !device) { fprintf(stderr, "GetVulkanHandles failed 0x%08lx\n", (unsigned long)hr); return 5; }

    HMODULE vulkan = LoadLibraryA("vulkan-1.dll");
    auto get_instance = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr")) : nullptr;
    auto get_device = get_instance ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance(instance, "vkGetDeviceProcAddr")) : nullptr;
    auto create_buffer = get_device ? reinterpret_cast<PFN_vkCreateBuffer>(get_device(device, "vkCreateBuffer")) : nullptr;
    auto destroy_buffer = get_device ? reinterpret_cast<PFN_vkDestroyBuffer>(get_device(device, "vkDestroyBuffer")) : nullptr;
    auto get_requirements = get_device ? reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(get_device(device, "vkGetBufferMemoryRequirements")) : nullptr;
    auto get_memory_properties = get_instance ? reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get_instance(instance, "vkGetPhysicalDeviceMemoryProperties")) : nullptr;
    auto allocate_memory = get_device ? reinterpret_cast<PFN_vkAllocateMemory>(get_device(device, "vkAllocateMemory")) : nullptr;
    auto free_memory = get_device ? reinterpret_cast<PFN_vkFreeMemory>(get_device(device, "vkFreeMemory")) : nullptr;
    auto bind_buffer = get_device ? reinterpret_cast<PFN_vkBindBufferMemory>(get_device(device, "vkBindBufferMemory")) : nullptr;
    auto get_memory_fd = get_device ? reinterpret_cast<PFN_vkGetMemoryFdKHR>(get_device(device, "vkGetMemoryFdKHR")) : nullptr;
    auto get_memory_fd_properties = get_device ? reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(get_device(device, "vkGetMemoryFdPropertiesKHR")) : nullptr;
    auto get_external_buffer_properties = get_instance ? reinterpret_cast<PFN_vkGetPhysicalDeviceExternalBufferProperties>(get_instance(instance, "vkGetPhysicalDeviceExternalBufferProperties")) : nullptr;
    auto get_properties2 = get_instance ? reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get_instance(instance, "vkGetPhysicalDeviceProperties2")) : nullptr;
    if (!create_buffer || !destroy_buffer || !get_requirements || !get_memory_properties ||
            !allocate_memory || !free_memory || !bind_buffer || !get_memory_fd ||
            !get_memory_fd_properties) {
        fprintf(stderr, "Vulkan function loading failed\n");
        return 6;
    }

    if (get_properties2) {
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDevicePCIBusInfoPropertiesEXT pci{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
        properties.pNext = &id;
        id.pNext = &pci;
        get_properties2(physical, &properties);
        fprintf(stderr, "physical=%s vendor=0x%x device=0x%x uuid=%02x:%02x:%02x:%02x pci=%u:%u:%u.%u\n",
                properties.properties.deviceName, properties.properties.vendorID,
                properties.properties.deviceID, id.deviceUUID[0], id.deviceUUID[1],
                id.deviceUUID[2], id.deviceUUID[3], pci.pciDomain, pci.pciBus,
                pci.pciDevice, pci.pciFunction);
    }

    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (get_external_buffer_properties) {
        VkPhysicalDeviceExternalBufferInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                nullptr, 0, usage, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        VkExternalBufferProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
                nullptr, {0, 0, 0}};
        get_external_buffer_properties(physical, &info, &properties);
        fprintf(stderr, "external buffer features=0x%x compatible=0x%x exportable=0x%x\n",
                properties.externalMemoryProperties.externalMemoryFeatures,
                properties.externalMemoryProperties.compatibleHandleTypes,
                properties.externalMemoryProperties.exportFromImportedHandleTypes);
    }

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
            65536, usage, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    VkBuffer buffer = VK_NULL_HANDLE;
    vk_result = create_buffer(device, &buffer_info, nullptr, &buffer);
    log_vk("vkCreateBuffer", vk_result);
    if (vk_result != VK_SUCCESS) return 7;

    VkMemoryRequirements requirements{};
    get_requirements(device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties{};
    get_memory_properties(physical, &memory_properties);
    fprintf(stderr, "memory types=%u heaps=%u\n",
            memory_properties.memoryTypeCount, memory_properties.memoryHeapCount);
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        fprintf(stderr, "  memory_type[%u] flags=0x%x heap=%u%s\n", i,
                memory_properties.memoryTypes[i].propertyFlags,
                memory_properties.memoryTypes[i].heapIndex,
                (requirements.memoryTypeBits & (1u << i)) ? " compatible" : "");
    }
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memory_type = i;
            break;
        }
    }
    fprintf(stderr, "requirements size=%llu alignment=%llu bits=0x%x type=%u\n",
            (unsigned long long)requirements.size, (unsigned long long)requirements.alignment,
            requirements.memoryTypeBits, memory_type);
    if (memory_type == UINT32_MAX) return 8;

    VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &export_info,
            requirements.size, memory_type};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    vk_result = allocate_memory(device, &allocate_info, nullptr, &memory);
    log_vk("vkAllocateMemory(exportable direct)", vk_result);
    if (vk_result != VK_SUCCESS) return 9;
    vk_result = bind_buffer(device, buffer, memory, 0);
    log_vk("vkBindBufferMemory", vk_result);

    int fd = -1;
    VkMemoryGetFdInfoKHR fd_info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr,
            memory, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    const VkResult fd_result = get_memory_fd(device, &fd_info, &fd);
    vk_result = fd_result;
    log_vk("vkGetMemoryFdKHR(direct)", fd_result);
    fprintf(stderr, "direct fd=%d\n", fd);
    uint32_t fd_bits = 0;
    bool helper_success = false;
    if (fd_result == VK_SUCCESS) {
        VkMemoryFdPropertiesKHR fd_properties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR, nullptr, 0};
        vk_result = get_memory_fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                fd, &fd_properties);
        log_vk("vkGetMemoryFdPropertiesKHR(direct)", vk_result);
        fd_bits = fd_properties.memoryTypeBits;
    }
    fprintf(stderr, "direct_fd_properties_bits=0x%x\n", fd_bits);
    if (fd_result == VK_SUCCESS && fd >= 0) {
        int source = 0;
        const char *source_text = getenv("MGPU_D3D12_ADAPTER_INDEX");
        if (source_text && *source_text)
            source = atoi(source_text);
        helper_success = spawn_cuda_helper(fd, requirements.size, getenv("MGPU_CUDA_IMPORT_HELPER"),
                source, source == 0 ? 1 : 0);
    }

    if (free_memory) free_memory(device, memory, nullptr);
    destroy_buffer(device, buffer, nullptr);
    d3d12->Release();
    adapter->Release();
    factory->Release();
    /* vkGetMemoryFdPropertiesKHR is unfortunately VK_ERROR_UNKNOWN on the
     * Wine Vulkan thunk, but CUDA accepts the actual inherited descriptor.
     * The end-to-end helper result is therefore the authoritative gate when
     * a helper was requested. */
    return helper_success || vk_result == VK_SUCCESS ? 0 : 10;
}
