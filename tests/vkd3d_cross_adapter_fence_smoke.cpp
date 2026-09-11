#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <vulkan.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

struct interop_device;
struct interop_device5_vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(interop_device *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(interop_device *);
    ULONG (STDMETHODCALLTYPE *Release)(interop_device *);
    HRESULT (STDMETHODCALLTYPE *GetDXGIAdapter)(interop_device *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *GetInstanceExtensions)(interop_device *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceExtensions)(interop_device *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceFeatures)(interop_device *, const void **);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHandles)(interop_device *, VkInstance *, VkPhysicalDevice *, VkDevice *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanQueueInfo)(interop_device *, ID3D12CommandQueue *, void **, UINT32 *);
    void (STDMETHODCALLTYPE *GetVulkanImageLayout)(interop_device *, ID3D12Resource *, D3D12_RESOURCE_STATES, int *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo)(interop_device *, ID3D12Resource *, UINT64 *, UINT64 *);
    HRESULT (STDMETHODCALLTYPE *LockCommandQueue)(interop_device *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *UnlockCommandQueue)(interop_device *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo1)(interop_device *, ID3D12Resource *, UINT64 *, UINT64 *, int *);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandQueue)(interop_device *, const D3D12_COMMAND_QUEUE_DESC *, UINT32, ID3D12CommandQueue **);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandAllocator)(interop_device *, D3D12_COMMAND_LIST_TYPE, UINT32, ID3D12CommandAllocator **);
    HRESULT (STDMETHODCALLTYPE *BeginVkCommandBufferInterop)(interop_device *, ID3D12CommandList *, void **);
    HRESULT (STDMETHODCALLTYPE *EndVkCommandBufferInterop)(interop_device *, ID3D12CommandList *);
    HRESULT (STDMETHODCALLTYPE *LockVulkanQueue)(interop_device *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *UnlockVulkanQueue)(interop_device *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHeapInfo)(interop_device *, ID3D12Heap *, UINT64 *, UINT64 *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *ExportVulkanHeapFd)(interop_device *, ID3D12Heap *, UINT32, INT *);
    HRESULT (STDMETHODCALLTYPE *ExportVulkanFenceFd)(interop_device *, ID3D12Fence *, UINT32, INT *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanPhysicalDeviceIdentity)(interop_device *, UINT8 *, UINT32 *, UINT32 *, UINT32 *, UINT32 *);
};
struct interop_device { const interop_device5_vtbl *lpVtbl; };

static const GUID IID_ID3D12DXVKInteropDevice5 =
    {0x5f7f64b7, 0x8e0d, 0x4aa8, {0x9e, 0x29, 0x4b, 0x2f, 0x1b, 0x3d, 0x7e, 0x61}};

struct handles {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    interop_device *interop = nullptr;
    UINT8 uuid[VK_UUID_SIZE]{};
    UINT32 domain = 0, bus = 0, device_id = 0, function = 0;
};

static void log_hr(const char *name, HRESULT hr)
{
    std::printf("%s=0x%08lx\n", name, (unsigned long)hr);
}

static bool inspect_handles(ID3D12Device *device, const char *label, handles *out)
{
    HRESULT hr = device->QueryInterface(IID_ID3D12DXVKInteropDevice5,
            reinterpret_cast<void **>(&out->interop));
    log_hr(label, hr);
    if (FAILED(hr) || !out->interop)
        return false;
    hr = out->interop->lpVtbl->GetVulkanHandles(out->interop, &out->instance,
            &out->physical, &out->device);
    log_hr("get_vulkan_handles", hr);
    if (FAILED(hr) || !out->physical || !out->device)
        return false;
    hr = out->interop->lpVtbl->GetVulkanPhysicalDeviceIdentity(out->interop,
            out->uuid, &out->domain, &out->bus, &out->device_id, &out->function);
    log_hr("get_physical_identity", hr);
    std::printf("%s_uuid=%02x:%02x:%02x:%02x pci=%u:%u:%u.%u\n", label,
            out->uuid[0], out->uuid[1], out->uuid[2], out->uuid[3],
            out->domain, out->bus, out->device_id, out->function);
    return SUCCEEDED(hr);
}

static bool wait_for_status(const char *path, const char *needle, unsigned timeout_ms)
{
    if (!path || !*path || !needle) return false;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream file(path);
        std::string contents((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        if (contents.find(needle) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static bool spawn_cuda_wait_helper(int fd, int ordinal, const char *helper,
                                   const char *status_log)
{
    if (fd < 0 || !helper || !*helper || !status_log || !*status_log)
        return false;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using Spawn = LONG (WINAPI *)(char *const[], int);
    auto spawn = ntdll ? reinterpret_cast<Spawn>(
        GetProcAddress(ntdll, "__wine_unix_spawnvp")) : nullptr;
    if (!spawn) return false;
    char fd_text[32], ordinal_text[32], value_text[32];
    std::snprintf(fd_text, sizeof(fd_text), "%d", fd);
    std::snprintf(ordinal_text, sizeof(ordinal_text), "%d", ordinal);
    std::snprintf(value_text, sizeof(value_text), "%d", 1);
    const std::string gate_path = std::string(status_log) + ".gate";
    std::remove(gate_path.c_str());
    char *argv[] = {const_cast<char *>(helper), fd_text, ordinal_text,
                    value_text, const_cast<char *>(status_log),
                    const_cast<char *>(gate_path.c_str()), nullptr};
    SetEnvironmentVariableA("MGPU_INHERIT_FD", fd_text);
    const LONG result = spawn(argv, 0);
    SetEnvironmentVariableA("MGPU_INHERIT_FD", nullptr);
    std::printf("cuda_fence_wait_spawn=%s rc=%ld ordinal=%d fd=%d\n",
                result == 0 ? "ok" : "fail", static_cast<long>(result),
                ordinal, fd);
    return result == 0;
}

int main()
{
    ID3D12Device *device_a = nullptr;
    ID3D12Device *device_b = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&device_a));
    log_hr("create_device_a", hr);
    if (FAILED(hr)) return 4;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&device_b));
    log_hr("create_device_b", hr);
    if (FAILED(hr)) return 5;

    handles a, b;
    bool handles_ok = inspect_handles(device_a, "query_interop_a", &a) &&
            inspect_handles(device_b, "query_interop_b", &b);
    bool physical_distinct = handles_ok && a.physical != b.physical &&
            a.device != b.device;
    bool identity_distinct = handles_ok &&
            (std::memcmp(a.uuid, b.uuid, VK_UUID_SIZE) != 0 ||
             a.domain != b.domain || a.bus != b.bus ||
             a.device_id != b.device_id || a.function != b.function);
    std::printf("physical_devices_distinct=%s\n", physical_distinct ? "yes" : "no");
    std::printf("physical_identity_distinct=%s\n", identity_distinct ? "yes" : "no");
    if (!physical_distinct || !identity_distinct) {
        std::printf("cross_adapter_fence_roundtrip=not_run\n");
        if (a.interop) a.interop->lpVtbl->Release(a.interop);
        if (b.interop) b.interop->lpVtbl->Release(b.interop);
        device_b->Release(); device_a->Release();
        return 6;
    }

    ID3D12Fence *fence = nullptr;
    hr = device_a->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
            IID_PPV_ARGS(&fence));
    log_hr("create_shared_fence_a", hr);
    if (FAILED(hr)) return 7;
    INT fd = -1;
    hr = a.interop->lpVtbl->ExportVulkanFenceFd(a.interop, fence,
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, &fd);
    log_hr("export_fence_fd_a", hr);
    std::printf("exported_fd_a=%d\n", fd);
    if (FAILED(hr) || fd < 0) return 8;

    const char *cuda_helper = std::getenv("MGPU_FENCE_CUDA_WAIT_HELPER");
    const char *cuda_status_log = std::getenv("MGPU_FENCE_CUDA_WAIT_LOG");
    const int cuda_ordinal = std::getenv("MGPU_FENCE_CUDA_WAIT_ORDINAL")
        ? std::atoi(std::getenv("MGPU_FENCE_CUDA_WAIT_ORDINAL")) : 1;
    const bool cuda_wait_requested = cuda_helper && *cuda_helper &&
        cuda_status_log && *cuda_status_log;
    bool cuda_wait_passed = !cuda_wait_requested;
    ID3D12Fence *cuda_fence = nullptr;
    ID3D12CommandQueue *cuda_queue = nullptr;
    const bool cuda_gpu_signal = std::getenv("MGPU_FENCE_CUDA_GPU_SIGNAL") &&
        std::atoi(std::getenv("MGPU_FENCE_CUDA_GPU_SIGNAL")) != 0;
    INT cuda_fd = -1;
    if (cuda_wait_requested) {
        std::remove(cuda_status_log);
        hr = device_a->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                IID_PPV_ARGS(&cuda_fence));
        log_hr("create_cuda_fence_a", hr);
        if (SUCCEEDED(hr) && cuda_gpu_signal) {
            D3D12_COMMAND_QUEUE_DESC queue_desc{};
            queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = device_a->CreateCommandQueue(&queue_desc,
                    IID_PPV_ARGS(&cuda_queue));
            log_hr("create_cuda_queue_a", hr);
        }
        if (SUCCEEDED(hr))
            hr = a.interop->lpVtbl->ExportVulkanFenceFd(a.interop, cuda_fence,
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, &cuda_fd);
        log_hr("export_fence_fd_cuda", hr);
        if (SUCCEEDED(hr) && cuda_fd >= 0 &&
            spawn_cuda_wait_helper(cuda_fd, cuda_ordinal, cuda_helper,
                                   cuda_status_log) &&
            wait_for_status(cuda_status_log, "ready", 5000)) {
            std::printf("cuda_fence_wait_ready=yes\n");
            if (cuda_gpu_signal) {
                hr = cuda_queue->Signal(cuda_fence, 1);
                log_hr("signal_cuda_queue_1", hr);
            } else {
                hr = cuda_fence->Signal(1);
                log_hr("signal_cuda_fence_1", hr);
            }
        } else if (SUCCEEDED(hr)) {
            hr = cuda_fence->Signal(1);
            log_hr("signal_cuda_fence_1", hr);
        }
        if (SUCCEEDED(hr)) {
            const std::string gate_path = std::string(cuda_status_log) + ".gate";
            std::ofstream gate(gate_path, std::ios::app);
            gate << "go\n";
            gate.flush();
            cuda_wait_passed = SUCCEEDED(hr) &&
                wait_for_status(cuda_status_log, "done rc=0", 5000);
            std::printf("cuda_fence_wait=%s\n",
                    cuda_wait_passed ? "pass" : "fail");
        } else {
            std::printf("cuda_fence_wait_ready=no\n");
        }
    }

    HMODULE vulkan = LoadLibraryA("vulkan-1.dll");
    auto get_instance = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(vulkan, "vkGetInstanceProcAddr")) : nullptr;
    auto get_device = get_instance ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            get_instance(b.instance, "vkGetDeviceProcAddr")) : nullptr;
    auto create_semaphore = get_device ? reinterpret_cast<PFN_vkCreateSemaphore>(
            get_device(b.device, "vkCreateSemaphore")) : nullptr;
    auto import_fd = get_device ? reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
            get_device(b.device, "vkImportSemaphoreFdKHR")) : nullptr;
    auto get_counter = get_device ? reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
            get_device(b.device, "vkGetSemaphoreCounterValue")) : nullptr;
    auto wait_semaphores = get_device ? reinterpret_cast<PFN_vkWaitSemaphores>(
            get_device(b.device, "vkWaitSemaphores")) : nullptr;
    auto destroy_semaphore = get_device ? reinterpret_cast<PFN_vkDestroySemaphore>(
            get_device(b.device, "vkDestroySemaphore")) : nullptr;
    std::printf("gpu_b_import_proc=%s counter_proc=%s wait_proc=%s\n",
            import_fd ? "yes" : "no", get_counter ? "yes" : "no",
            wait_semaphores ? "yes" : "no");
    if (!create_semaphore || !import_fd || !get_counter || !wait_semaphores || !destroy_semaphore)
        return 9;

    VkSemaphoreTypeCreateInfo type_info{};
    type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore_info.pNext = &type_info;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkResult result = create_semaphore(b.device, &semaphore_info, nullptr, &semaphore);
    std::printf("create_import_semaphore_b=%d\n", (int)result);
    if (result != VK_SUCCESS)
        return 10;
    VkImportSemaphoreFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    import_info.semaphore = semaphore;
    import_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = fd;
    result = import_fd(b.device, &import_info);
    std::printf("import_fence_fd_on_b=%d\n", (int)result);
    if (result != VK_SUCCESS) {
        destroy_semaphore(b.device, semaphore, nullptr);
        std::printf("cross_adapter_fence_roundtrip=fail\n");
        return 11;
    }
    fd = -1;
    uint64_t before = UINT64_MAX;
    result = get_counter(b.device, semaphore, &before);
    std::printf("counter_b_before_signal=%d value=%llu\n", (int)result,
            (unsigned long long)before);
    hr = fence->Signal(1);
    log_hr("signal_fence_a_1", hr);
    uint64_t after = 0;
    result = get_counter(b.device, semaphore, &after);
    std::printf("counter_b_after_signal=%d value=%llu\n", (int)result,
            (unsigned long long)after);
    uint64_t value = 1;
    VkSemaphoreWaitInfo wait_info{};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &semaphore;
    wait_info.pValues = &value;
    result = wait_semaphores(b.device, &wait_info, 1000000000ull);
    std::printf("wait_fence_on_b=%d\n", (int)result);
    bool passed = SUCCEEDED(hr) && result == VK_SUCCESS && before == 0 && after >= 1 &&
        cuda_wait_passed;
    std::printf("cuda_fence_wait_requested=%s\n",
                cuda_wait_requested ? "yes" : "no");
    std::printf("cross_adapter_fence_roundtrip=%s\n", passed ? "pass" : "fail");
    destroy_semaphore(b.device, semaphore, nullptr);
    if (cuda_queue) cuda_queue->Release();
    if (cuda_fence) cuda_fence->Release();
    fence->Release();
    a.interop->lpVtbl->Release(a.interop);
    b.interop->lpVtbl->Release(b.interop);
    device_b->Release(); device_a->Release();
    return passed ? 0 : 12;
}
