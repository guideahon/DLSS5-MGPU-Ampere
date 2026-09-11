#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nvsdk_ngx.h"

using Microsoft::WRL::ComPtr;

struct Vkd3dInteropDevice;
struct Vkd3dInteropVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Vkd3dInteropDevice*, REFIID, void**);
    ULONG (STDMETHODCALLTYPE *AddRef)(Vkd3dInteropDevice*);
    ULONG (STDMETHODCALLTYPE *Release)(Vkd3dInteropDevice*);
    HRESULT (STDMETHODCALLTYPE *GetDXGIAdapter)(Vkd3dInteropDevice*, REFIID, void**);
    HRESULT (STDMETHODCALLTYPE *GetInstanceExtensions)(Vkd3dInteropDevice*, UINT*, const char**);
    HRESULT (STDMETHODCALLTYPE *GetDeviceExtensions)(Vkd3dInteropDevice*, UINT*, const char**);
    HRESULT (STDMETHODCALLTYPE *GetDeviceFeatures)(Vkd3dInteropDevice*, const void**);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHandles)(Vkd3dInteropDevice*, void**, void**, void**);
    HRESULT (STDMETHODCALLTYPE *GetVulkanQueueInfo)(Vkd3dInteropDevice*, ID3D12CommandQueue*, void**, UINT32*);
    void (STDMETHODCALLTYPE *GetVulkanImageLayout)(Vkd3dInteropDevice*, ID3D12Resource*, D3D12_RESOURCE_STATES, int*);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo)(Vkd3dInteropDevice*, ID3D12Resource*, UINT64*, UINT64*);
    HRESULT (STDMETHODCALLTYPE *LockCommandQueue)(Vkd3dInteropDevice*, ID3D12CommandQueue*);
    HRESULT (STDMETHODCALLTYPE *UnlockCommandQueue)(Vkd3dInteropDevice*, ID3D12CommandQueue*);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo1)(Vkd3dInteropDevice*, ID3D12Resource*, UINT64*, UINT64*, int*);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandQueue)(Vkd3dInteropDevice*, const D3D12_COMMAND_QUEUE_DESC*, UINT32, ID3D12CommandQueue**);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandAllocator)(Vkd3dInteropDevice*, D3D12_COMMAND_LIST_TYPE, UINT32, ID3D12CommandAllocator**);
    HRESULT (STDMETHODCALLTYPE *BeginVkCommandBufferInterop)(Vkd3dInteropDevice*, ID3D12CommandList*, void**);
    HRESULT (STDMETHODCALLTYPE *EndVkCommandBufferInterop)(Vkd3dInteropDevice*, ID3D12CommandList*);
    HRESULT (STDMETHODCALLTYPE *LockVulkanQueue)(Vkd3dInteropDevice*, ID3D12CommandQueue*);
    HRESULT (STDMETHODCALLTYPE *UnlockVulkanQueue)(Vkd3dInteropDevice*, ID3D12CommandQueue*);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHeapInfo)(Vkd3dInteropDevice*, ID3D12Heap*, UINT64*, UINT64*, UINT32*);
    HRESULT (STDMETHODCALLTYPE *ExportVulkanHeapFd)(Vkd3dInteropDevice*, ID3D12Heap*, UINT32, INT*);
    HRESULT (STDMETHODCALLTYPE *ExportVulkanFenceFd)(Vkd3dInteropDevice*, ID3D12Fence*, UINT32, INT*);
    HRESULT (STDMETHODCALLTYPE *GetVulkanPhysicalDeviceIdentity)(Vkd3dInteropDevice*, UINT8*, UINT32*, UINT32*, UINT32*, UINT32*);
    HRESULT (STDMETHODCALLTYPE *ExportVulkanResourceFd)(Vkd3dInteropDevice*, ID3D12Resource*, UINT32, INT*, UINT64*, UINT64*);
};
struct Vkd3dInteropDevice { const Vkd3dInteropVtbl* lpVtbl; };

static const GUID IID_ID3D12DXVKInteropDevice6 =
    {0x6a4b7d2e, 0x2c52, 0x4e11, {0x9c, 0x86, 0x2f, 0x0a, 0xf5, 0xf8, 0xb0, 0xc3}};

static const GUID IID_ID3D12DXVKInteropDevice4 =
    {0xb4eb6e34, 0x0a3a, 0x4a91, {0x9f, 0x21, 0x0f, 0x5a, 0x5c, 0x6f, 0x54, 0xd4}};

using NgxInit = NVSDK_NGX_Result (WINAPI *)(unsigned long long, const wchar_t*, ID3D12Device*,
                                             NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using NgxShutdown = NVSDK_NGX_Result (WINAPI *)(ID3D12Device*);
using NgxAllocateParameters = NVSDK_NGX_Result (WINAPI *)(NVSDK_NGX_Parameter**);
using NgxDestroyParameters = NVSDK_NGX_Result (WINAPI *)(NVSDK_NGX_Parameter*);
using NgxCreateFeature = NVSDK_NGX_Result (WINAPI *)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                     NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using NgxEvaluateFeature = NVSDK_NGX_Result (WINAPI *)(ID3D12GraphicsCommandList*,
                                                       const NVSDK_NGX_Handle*,
                                                       const NVSDK_NGX_Parameter*,
                                                       PFN_NVSDK_NGX_ProgressCallback);
using NgxReleaseFeature = NVSDK_NGX_Result (WINAPI *)(NVSDK_NGX_Handle*);

template <typename T>
static T resolve_ngx(HMODULE module, const char* name) {
    return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
}

static void set_ngx_resource(NVSDK_NGX_Parameter* parameters, const char* name,
                             ID3D12Resource* resource) {
    using SetResourceFn = void (*)(NVSDK_NGX_Parameter*, const char*, ID3D12Resource*);
    auto** table = *reinterpret_cast<void***>(parameters);
    reinterpret_cast<SetResourceFn>(table[0])(parameters, name, resource);
}

static UINT64 align_up(UINT64 value, UINT64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

static UINT64 fnv1a(const unsigned char* data, UINT64 size) {
    UINT64 hash = 1469598103934665603ULL;
    for (UINT64 index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static IDXGIAdapter1* find_3090(IDXGIFactory4* factory, int ordinal) {
    int found = 0;
    for (UINT index = 0; ; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if (!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            wcsstr(description.Description, L"RTX 3090") != nullptr &&
            found++ == ordinal)
            return adapter;
        adapter->Release();
    }
    return nullptr;
}

static bool wait_queue(ID3D12Device* device, ID3D12CommandQueue* queue,
                       ID3D12GraphicsCommandList* list, HRESULT* close_out) {
    HRESULT close_hr = list->Close();
    if (close_out) *close_out = close_hr;
    if (FAILED(close_hr)) return false;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return false;
    hr = queue->Signal(fence.Get(), 1);
    if (FAILED(hr)) return false;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    if (fence->GetCompletedValue() < 1) {
        hr = fence->SetEventOnCompletion(1, event);
        if (FAILED(hr)) {
            CloseHandle(event);
            return false;
        }
        DWORD wait_result = WaitForSingleObject(event, 10000);
        if (wait_result != WAIT_OBJECT_0) {
            CloseHandle(event);
            return false;
        }
    }
    CloseHandle(event);
    return true;
}

static bool spawn_frame_copy_helper(int source_fd, UINT64 source_heap_size,
                                    int source_ordinal, int destination_fd,
                                    UINT64 destination_heap_size, int destination_ordinal,
                                    const UINT64* offsets, const UINT64* sizes,
                                    const char* helper) {
    if (!helper || !*helper) return false;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using Spawn = LONG (WINAPI *)(char* const[], int);
    auto spawn = ntdll ? reinterpret_cast<Spawn>(GetProcAddress(ntdll, "__wine_unix_spawnvp")) : nullptr;
    if (!spawn) return false;
    char text[16][32]{};
    std::snprintf(text[0], sizeof(text[0]), "%d", source_fd);
    std::snprintf(text[1], sizeof(text[1]), "%llu",
                  static_cast<unsigned long long>(source_heap_size));
    std::snprintf(text[2], sizeof(text[2]), "%d", source_ordinal);
    std::snprintf(text[3], sizeof(text[3]), "%d", destination_fd);
    std::snprintf(text[4], sizeof(text[4]), "%llu",
                  static_cast<unsigned long long>(destination_heap_size));
    std::snprintf(text[5], sizeof(text[5]), "%d", destination_ordinal);
    std::snprintf(text[6], sizeof(text[6]), "%d", 3);
    char* argv[19]{};
    argv[0] = const_cast<char*>(helper);
    argv[1] = const_cast<char*>("--batch");
    for (int index = 0; index < 7; ++index) argv[2 + index] = text[index];
    for (int index = 0; index < 3; ++index) {
        const int base = 7 + index * 3;
        std::snprintf(text[base], sizeof(text[base]), "%llu",
                      static_cast<unsigned long long>(offsets[index]));
        std::snprintf(text[base + 1], sizeof(text[base + 1]), "%llu",
                      static_cast<unsigned long long>(sizes[index]));
        std::snprintf(text[base + 2], sizeof(text[base + 2]), "%02x", 0);
        argv[2 + base] = text[base];
        argv[2 + base + 1] = text[base + 1];
        argv[2 + base + 2] = text[base + 2];
    }
    SetEnvironmentVariableA("MGPU_INHERIT_FD", text[0]);
    LONG result = spawn(argv, 1);
    SetEnvironmentVariableA("MGPU_INHERIT_FD", nullptr);
    std::fprintf(stderr, "cross_adapter_frame_helper=%s rc=%ld planes=3 source_fd=%d destination_fd=%d\n",
                 result == 0 ? "ok" : "FAIL", static_cast<long>(result), source_fd,
                 destination_fd);
    return result == 0;
}

struct ResourceCopyPair {
    int source_fd;
    UINT64 source_size;
    UINT64 source_offset;
    int destination_fd;
    UINT64 destination_size;
    UINT64 destination_offset;
    UINT64 bytes;
    unsigned int expected;
};

static bool spawn_resource_pairs_copy_helper(const ResourceCopyPair* pairs, int pair_count,
                                             int source_ordinal, int destination_ordinal,
                                             const char* helper, int repeat_count) {
    if (!helper || !*helper || !pairs || pair_count < 1 || pair_count > 3)
        return false;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using Spawn = LONG (WINAPI *)(char* const[], int);
    auto spawn = ntdll ? reinterpret_cast<Spawn>(GetProcAddress(ntdll, "__wine_unix_spawnvp")) : nullptr;
    if (!spawn) return false;

    if (repeat_count < 1) repeat_count = 1;
    char text[32][40]{};
    std::snprintf(text[0], sizeof(text[0]), "%d", source_ordinal);
    std::snprintf(text[1], sizeof(text[1]), "%d", destination_ordinal);
    std::snprintf(text[2], sizeof(text[2]), "%d", pair_count);
    char* argv[32]{};
    argv[0] = const_cast<char*>(helper);
    argv[1] = const_cast<char*>(repeat_count > 1 ? "--pairs-repeat" : "--pairs");
    argv[2] = text[0];
    argv[3] = text[1];
    argv[4] = text[2];
    int pair_base = 5;
    if (repeat_count > 1) {
        std::snprintf(text[5], sizeof(text[5]), "%d", repeat_count);
        argv[5] = text[5];
        pair_base = 6;
    }
    for (int index = 0; index < pair_count; ++index) {
        const int base = pair_base + index * 8;
        std::snprintf(text[base], sizeof(text[base]), "%d", pairs[index].source_fd);
        std::snprintf(text[base + 1], sizeof(text[base + 1]), "%llu",
                      static_cast<unsigned long long>(pairs[index].source_size));
        std::snprintf(text[base + 2], sizeof(text[base + 2]), "%llu",
                      static_cast<unsigned long long>(pairs[index].source_offset));
        std::snprintf(text[base + 3], sizeof(text[base + 3]), "%d", pairs[index].destination_fd);
        std::snprintf(text[base + 4], sizeof(text[base + 4]), "%llu",
                      static_cast<unsigned long long>(pairs[index].destination_size));
        std::snprintf(text[base + 5], sizeof(text[base + 5]), "%llu",
                      static_cast<unsigned long long>(pairs[index].destination_offset));
        std::snprintf(text[base + 6], sizeof(text[base + 6]), "%llu",
                      static_cast<unsigned long long>(pairs[index].bytes));
        std::snprintf(text[base + 7], sizeof(text[base + 7]), "%02x",
                      pairs[index].expected & 0xffU);
    }
    for (int index = 0; index < pair_count; ++index) {
        const int base = pair_base + index * 8;
        for (int field = 0; field < 8; ++field)
            argv[base + field] = text[base + field];
    }
    argv[pair_base + pair_count * 8] = nullptr;
    SetEnvironmentVariableA("MGPU_INHERIT_FD", text[pair_base]);
    LONG result = spawn(argv, 1);
    SetEnvironmentVariableA("MGPU_INHERIT_FD", nullptr);
    std::fprintf(stderr,
                 "cross_adapter_resource_pairs_helper=%s rc=%ld pairs=%d repeat_count=%d\n",
                 result == 0 ? "ok" : "FAIL", static_cast<long>(result), pair_count,
                 repeat_count);
    return result == 0;
}

static bool spawn_resource_pairs_daemon(const ResourceCopyPair* pairs, int pair_count,
                                        int source_ordinal, int destination_ordinal,
                                        const char* helper, int port, int repeat_count) {
    if (!helper || !*helper || !pairs || pair_count < 1 || pair_count > 3)
        return false;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using Spawn = LONG (WINAPI *)(char* const[], int);
    auto spawn = ntdll ? reinterpret_cast<Spawn>(GetProcAddress(ntdll, "__wine_unix_spawnvp")) : nullptr;
    if (!spawn || port < 1 || port > 65535) return false;
    if (repeat_count < 1) repeat_count = 1;

    char text[40][48]{};
    std::snprintf(text[0], sizeof(text[0]), "%d", source_ordinal);
    std::snprintf(text[1], sizeof(text[1]), "%d", destination_ordinal);
    std::snprintf(text[2], sizeof(text[2]), "%d", pair_count);
    std::snprintf(text[3], sizeof(text[3]), "%d", port);
    char* daemon_argv[32]{};
    daemon_argv[0] = const_cast<char*>(helper);
    daemon_argv[1] = const_cast<char*>("--resource-pair-daemon");
    daemon_argv[2] = text[0];
    daemon_argv[3] = text[1];
    daemon_argv[4] = text[2];
    daemon_argv[5] = text[3];
    int argument = 6;
    char inherit_text[256]{};
    size_t inherit_used = 0;
    for (int index = 0; index < pair_count; ++index) {
        const ResourceCopyPair& pair = pairs[index];
        const int values[] = {
            pair.source_fd, pair.destination_fd};
        const unsigned long long sizes[] = {
            pair.source_size, pair.source_offset, pair.destination_size,
            pair.destination_offset, pair.bytes};
        std::snprintf(text[argument], sizeof(text[argument]), "%d", values[0]);
        daemon_argv[argument] = text[argument];
        ++argument;
        std::snprintf(text[argument], sizeof(text[argument]), "%llu", sizes[0]);
        daemon_argv[argument] = text[argument];
        ++argument;
        std::snprintf(text[argument], sizeof(text[argument]), "%llu", sizes[1]);
        daemon_argv[argument] = text[argument];
        ++argument;
        std::snprintf(text[argument], sizeof(text[argument]), "%d", values[1]);
        daemon_argv[argument] = text[argument];
        ++argument;
        std::snprintf(text[argument], sizeof(text[argument]), "%llu", sizes[2]);
        daemon_argv[argument] = text[argument];
        ++argument;
        std::snprintf(text[argument], sizeof(text[argument]), "%llu", sizes[3]);
        daemon_argv[argument] = text[argument];
        ++argument;
        std::snprintf(text[argument], sizeof(text[argument]), "%llu", sizes[4]);
        daemon_argv[argument] = text[argument];
        ++argument;
        std::snprintf(text[argument], sizeof(text[argument]), "%llu",
                      static_cast<unsigned long long>(pair.expected & 0xffU));
        daemon_argv[argument] = text[argument];
        ++argument;
        for (int fd : values) {
            const int written = std::snprintf(
                inherit_text + inherit_used, sizeof(inherit_text) - inherit_used,
                "%s%d", inherit_used ? "," : "", fd);
            if (written < 0 || static_cast<size_t>(written) >= sizeof(inherit_text) - inherit_used)
                return false;
            inherit_used += static_cast<size_t>(written);
        }
    }
    daemon_argv[argument] = nullptr;
    SetEnvironmentVariableA("MGPU_INHERIT_FD", inherit_text);
    const LONG daemon_result = spawn(daemon_argv, 0);
    SetEnvironmentVariableA("MGPU_INHERIT_FD", nullptr);
    if (daemon_result != 0) {
        std::fprintf(stderr, "cross_adapter_resource_daemon_spawn=FAIL rc=%ld\n",
                     static_cast<long>(daemon_result));
        return false;
    }

    char port_text[16], repeat_text[16];
    std::snprintf(port_text, sizeof(port_text), "%d", port);
    std::snprintf(repeat_text, sizeof(repeat_text), "%d", repeat_count);
    char* client_argv[] = {const_cast<char*>(helper),
                           const_cast<char*>("--daemon-client"), port_text,
                           repeat_text, nullptr};
    const LONG client_result = spawn(client_argv, 1);
    std::fprintf(stderr,
                 "cross_adapter_resource_daemon_client=%s rc=%ld pairs=%d commands=%d port=%d\n",
                 client_result == 0 ? "ok" : "FAIL", static_cast<long>(client_result),
                 pair_count, repeat_count, port);
    return client_result == 0;
}

static bool spawn_single_resource_import_helper(int fd, UINT64 size, UINT64 offset,
                                                int source_ordinal, int destination_ordinal,
                                                const char* helper) {
    if (!helper || !*helper) return true;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using Spawn = LONG (WINAPI *)(char* const[], int);
    auto spawn = ntdll ? reinterpret_cast<Spawn>(GetProcAddress(ntdll, "__wine_unix_spawnvp")) : nullptr;
    if (!spawn) return false;
    char fd_text[32], size_text[32], source_text[16], destination_text[16], offset_text[32];
    std::snprintf(fd_text, sizeof(fd_text), "%d", fd);
    std::snprintf(size_text, sizeof(size_text), "%llu", static_cast<unsigned long long>(size));
    std::snprintf(source_text, sizeof(source_text), "%d", source_ordinal);
    std::snprintf(destination_text, sizeof(destination_text), "%d", destination_ordinal);
    std::snprintf(offset_text, sizeof(offset_text), "%llu", static_cast<unsigned long long>(offset));
    char* argv[] = {const_cast<char*>(helper), fd_text, size_text, source_text,
                    destination_text, offset_text, const_cast<char*>("readonly"), nullptr};
    SetEnvironmentVariableA("MGPU_INHERIT_FD", fd_text);
    SetEnvironmentVariableA("MGPU_CUDA_IMPORT_READONLY", "1");
    const LONG result = spawn(argv, 1);
    SetEnvironmentVariableA("MGPU_CUDA_IMPORT_READONLY", nullptr);
    SetEnvironmentVariableA("MGPU_INHERIT_FD", nullptr);
    std::fprintf(stderr, "cross_adapter_single_resource_helper=%s rc=%ld fd=%d size=%llu offset=%llu\n",
                 result == 0 ? "ok" : "FAIL", static_cast<long>(result), fd,
                 static_cast<unsigned long long>(size), static_cast<unsigned long long>(offset));
    return result == 0;
}

static bool set_transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    return true;
}

int main() {
    using Clock = std::chrono::steady_clock;
    const auto total_start = Clock::now();
    constexpr UINT width = 640;
    constexpr UINT height = 360;
    constexpr UINT64 alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    const int source_ordinal = std::getenv("MGPU_CUDA_SOURCE_ORDINAL")
        ? std::atoi(std::getenv("MGPU_CUDA_SOURCE_ORDINAL")) : 0;
    const int destination_ordinal = std::getenv("MGPU_CUDA_DESTINATION_ORDINAL")
        ? std::atoi(std::getenv("MGPU_CUDA_DESTINATION_ORDINAL")) : 1;
    const int persistent_repeat_count = std::getenv("MGPU_CROSS_ADAPTER_PERSISTENT_FRAMES")
        ? std::max(1, std::atoi(std::getenv("MGPU_CROSS_ADAPTER_PERSISTENT_FRAMES"))) : 1;
    const char* helper = std::getenv("MGPU_CUDA_P2P_COPY_HELPER");
    const bool resource_fd_mode = std::getenv("MGPU_CROSS_ADAPTER_RESOURCE_FD") &&
                                  std::strcmp(std::getenv("MGPU_CROSS_ADAPTER_RESOURCE_FD"), "1") == 0;
    const char* ngx_mode = std::getenv("MGPU_NGX_CROSS_ADAPTER");
    const bool ngx_requested = ngx_mode && std::strcmp(ngx_mode, "1") == 0;
    const int ngx_frame_count = ngx_requested && std::getenv("MGPU_NGX_FRAME_COUNT")
        ? std::clamp(std::atoi(std::getenv("MGPU_NGX_FRAME_COUNT")), 1, 32) : 1;
    int ngx_frames_completed = 0;
    const bool resource_daemon_mode = resource_fd_mode &&
        std::getenv("MGPU_CROSS_ADAPTER_RESOURCE_DAEMON") &&
        std::strcmp(std::getenv("MGPU_CROSS_ADAPTER_RESOURCE_DAEMON"), "1") == 0;
    const int resource_daemon_repeat = std::getenv("MGPU_CROSS_ADAPTER_DAEMON_REPEAT")
        ? std::max(1, std::atoi(std::getenv("MGPU_CROSS_ADAPTER_DAEMON_REPEAT"))) : 1;
    const int resource_daemon_port = std::getenv("MGPU_CROSS_ADAPTER_DAEMON_PORT")
        ? std::atoi(std::getenv("MGPU_CROSS_ADAPTER_DAEMON_PORT")) : 47941;
    const int resource_daemon_output_port = std::getenv("MGPU_CROSS_ADAPTER_DAEMON_OUTPUT_PORT")
        ? std::atoi(std::getenv("MGPU_CROSS_ADAPTER_DAEMON_OUTPUT_PORT"))
        : (resource_daemon_port >= 65535 ? resource_daemon_port - 1
                                         : resource_daemon_port + 1);

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return 2;
    const bool reverse_direction = std::getenv("MGPU_CROSS_ADAPTER_REVERSE") &&
                                   std::strcmp(std::getenv("MGPU_CROSS_ADAPTER_REVERSE"), "1") == 0;
    const int source_adapter_ordinal = reverse_direction ? 1 : 0;
    const int destination_adapter_ordinal = reverse_direction ? 0 : 1;
    ComPtr<IDXGIAdapter1> adapter_a(find_3090(factory.Get(), source_adapter_ordinal));
    ComPtr<IDXGIAdapter1> adapter_b(find_3090(factory.Get(), destination_adapter_ordinal));
    if (!adapter_a || !adapter_b) return 3;
    ComPtr<ID3D12Device> device_a;
    ComPtr<ID3D12Device> device_b;
    if (reverse_direction)
        SetEnvironmentVariableA("VKD3D_DUPLICATE_LUID_INDEX", "1");
    hr = D3D12CreateDevice(adapter_a.Get(), D3D_FEATURE_LEVEL_12_0,
                            IID_PPV_ARGS(&device_a));
    if (FAILED(hr)) return 4;
    if (reverse_direction)
        SetEnvironmentVariableA("VKD3D_DUPLICATE_LUID_INDEX", "0");
    hr = D3D12CreateDevice(adapter_b.Get(), D3D_FEATURE_LEVEL_12_0,
                            IID_PPV_ARGS(&device_b));
    if (reverse_direction)
        SetEnvironmentVariableA("VKD3D_DUPLICATE_LUID_INDEX", nullptr);
    if (FAILED(hr)) return 5;

    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 bytes = 0;
    device_a->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint,
                                    &rows, &row_size, &bytes);
    D3D12_RESOURCE_DESC motion_desc = texture_desc;
    motion_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    D3D12_RESOURCE_DESC depth_desc = texture_desc;
    depth_desc.Format = DXGI_FORMAT_R32_FLOAT;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT motion_footprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_footprint{};
    UINT motion_rows = 0;
    UINT depth_rows = 0;
    UINT64 motion_row_size = 0;
    UINT64 depth_row_size = 0;
    UINT64 motion_bytes = 0;
    UINT64 depth_bytes = 0;
    device_a->GetCopyableFootprints(&motion_desc, 0, 1, 0, &motion_footprint,
                                    &motion_rows, &motion_row_size, &motion_bytes);
    device_a->GetCopyableFootprints(&depth_desc, 0, 1, 0, &depth_footprint,
                                    &depth_rows, &depth_row_size, &depth_bytes);
    const UINT64 buffer_offset = align_up(bytes, alignment);
    const UINT64 motion_offset = align_up(buffer_offset + bytes, alignment);
    const UINT64 depth_offset = align_up(motion_offset + motion_bytes, alignment);
    const UINT64 heap_size = align_up(depth_offset + depth_bytes, alignment);
    std::fprintf(stderr, "cross_adapter_frame footprint=%ux%u row_pitch=%u rows=%u bytes=%llu motion_bytes=%llu depth_bytes=%llu heap=%llu color_offset=%llu motion_offset=%llu depth_offset=%llu\n",
                 footprint.Footprint.Width, footprint.Footprint.Height,
                 footprint.Footprint.RowPitch, rows,
                 static_cast<unsigned long long>(bytes),
                 static_cast<unsigned long long>(motion_bytes),
                 static_cast<unsigned long long>(depth_bytes),
                 static_cast<unsigned long long>(heap_size),
                 static_cast<unsigned long long>(buffer_offset),
                 static_cast<unsigned long long>(motion_offset),
                 static_cast<unsigned long long>(depth_offset));

    D3D12_HEAP_DESC heap_desc{};
    heap_desc.SizeInBytes = heap_size;
    heap_desc.Alignment = alignment;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Heap> heap_a;
    ComPtr<ID3D12Heap> heap_b;
    hr = device_a->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap_a));
    if (FAILED(hr)) return 6;
    hr = device_b->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap_b));
    if (FAILED(hr)) return 7;

    ComPtr<ID3D12Resource> texture_a;
    ComPtr<ID3D12Resource> buffer_a;
    ComPtr<ID3D12Resource> motion_buffer_a;
    ComPtr<ID3D12Resource> depth_buffer_a;
    ComPtr<ID3D12Resource> texture_b;
    ComPtr<ID3D12Resource> buffer_b;
    ComPtr<ID3D12Resource> motion_buffer_b;
    ComPtr<ID3D12Resource> depth_buffer_b;
    ComPtr<ID3D12Resource> motion_texture_a;
    ComPtr<ID3D12Resource> depth_texture_a;
    ComPtr<ID3D12Resource> motion_texture_b;
    ComPtr<ID3D12Resource> depth_texture_b;
    hr = device_a->CreatePlacedResource(heap_a.Get(), 0, &texture_desc,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                        IID_PPV_ARGS(&texture_a));
    if (FAILED(hr)) return 8;
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device_a->CreatePlacedResource(heap_a.Get(), buffer_offset, &buffer_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&buffer_a));
    if (FAILED(hr)) return 9;
    D3D12_RESOURCE_DESC motion_buffer_desc = buffer_desc;
    motion_buffer_desc.Width = motion_bytes;
    D3D12_RESOURCE_DESC depth_buffer_desc = buffer_desc;
    depth_buffer_desc.Width = depth_bytes;
    hr = device_a->CreatePlacedResource(heap_a.Get(), motion_offset,
                                        &motion_buffer_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&motion_buffer_a));
    if (FAILED(hr)) return 9;
    hr = device_a->CreatePlacedResource(heap_a.Get(), depth_offset,
                                        &depth_buffer_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&depth_buffer_a));
    if (FAILED(hr)) return 9;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = device_b->CreatePlacedResource(heap_b.Get(), 0, &texture_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&texture_b));
    if (FAILED(hr)) return 10;
    hr = device_b->CreatePlacedResource(heap_b.Get(), buffer_offset, &buffer_desc,
                                        D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                        IID_PPV_ARGS(&buffer_b));
    if (FAILED(hr)) return 11;
    hr = device_b->CreatePlacedResource(heap_b.Get(), motion_offset,
                                        &motion_buffer_desc,
                                        D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                        IID_PPV_ARGS(&motion_buffer_b));
    if (FAILED(hr)) return 11;
    hr = device_b->CreatePlacedResource(heap_b.Get(), depth_offset,
                                        &depth_buffer_desc,
                                        D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                        IID_PPV_ARGS(&depth_buffer_b));
    if (FAILED(hr)) return 11;
    D3D12_HEAP_PROPERTIES default_properties{};
    default_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC motion_texture_desc = motion_desc;
    motion_texture_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_DESC depth_texture_desc = depth_desc;
    depth_texture_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = device_a->CreateCommittedResource(
        &default_properties, D3D12_HEAP_FLAG_NONE,
        &motion_texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&motion_texture_a));
    if (FAILED(hr)) return 11;
    hr = device_a->CreateCommittedResource(
        &default_properties, D3D12_HEAP_FLAG_NONE,
        &depth_texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&depth_texture_a));
    if (FAILED(hr)) return 11;
    hr = device_b->CreateCommittedResource(
        &default_properties, D3D12_HEAP_FLAG_NONE,
        &motion_texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&motion_texture_b));
    if (FAILED(hr)) return 11;
    hr = device_b->CreateCommittedResource(
        &default_properties, D3D12_HEAP_FLAG_NONE,
        &depth_texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&depth_texture_b));
    if (FAILED(hr)) return 11;

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.NumDescriptors = 1;
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    if (FAILED(device_a->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)))) return 12;
    device_a->CreateRenderTargetView(texture_a.Get(), nullptr,
                                      rtv_heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue_a;
    ComPtr<ID3D12CommandQueue> queue_b;
    if (FAILED(device_a->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_a)))) return 13;
    if (FAILED(device_b->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_b)))) return 14;
    ComPtr<ID3D12CommandAllocator> allocator_a;
    ComPtr<ID3D12CommandAllocator> allocator_b;
    if (FAILED(device_a->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator_a)))) return 15;
    if (FAILED(device_b->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator_b)))) return 16;
    ComPtr<ID3D12GraphicsCommandList> list_a;
    ComPtr<ID3D12GraphicsCommandList> list_b;
    if (FAILED(device_a->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           allocator_a.Get(), nullptr,
                                           IID_PPV_ARGS(&list_a)))) return 17;
    if (FAILED(device_b->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           allocator_b.Get(), nullptr,
                                           IID_PPV_ARGS(&list_b)))) return 18;

    D3D12_HEAP_PROPERTIES upload_properties{};
    upload_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    auto make_upload = [&](UINT64 upload_bytes, unsigned char seed,
                           ComPtr<ID3D12Resource>* result) -> bool {
        D3D12_RESOURCE_DESC upload_desc = buffer_desc;
        upload_desc.Width = upload_bytes;
        if (FAILED(device_a->CreateCommittedResource(
                &upload_properties, D3D12_HEAP_FLAG_NONE, &upload_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(result->ReleaseAndGetAddressOf()))))
            return false;
        void* mapped = nullptr;
        if (FAILED((*result)->Map(0, nullptr, &mapped)) || !mapped)
            return false;
        unsigned char* bytes_ptr = static_cast<unsigned char*>(mapped);
        for (UINT64 index = 0; index < upload_bytes; ++index)
            bytes_ptr[index] = static_cast<unsigned char>((index + seed) & 0xffU);
        bytes_ptr[0] = 0;
        (*result)->Unmap(0, nullptr);
        return true;
    };
    ComPtr<ID3D12Resource> motion_upload;
    ComPtr<ID3D12Resource> depth_upload;
    if (!make_upload(motion_bytes, 0x31, &motion_upload) ||
        !make_upload(depth_bytes, 0x73, &depth_upload)) return 18;

    const float clear_value[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    list_a->ClearRenderTargetView(rtv_heap->GetCPUDescriptorHandleForHeapStart(),
                                  clear_value, 0, nullptr);
    set_transition(list_a.Get(), texture_a.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = texture_a.Get();
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination_location{};
    destination_location.pResource = buffer_a.Get();
    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination_location.PlacedFootprint = footprint;
    list_a->CopyTextureRegion(&destination_location, 0, 0, 0,
                              &source_location, nullptr);
    list_a->CopyBufferRegion(motion_buffer_a.Get(), 0, motion_upload.Get(), 0,
                             motion_bytes);
    list_a->CopyBufferRegion(depth_buffer_a.Get(), 0, depth_upload.Get(), 0,
                             depth_bytes);
    D3D12_TEXTURE_COPY_LOCATION motion_upload_location{};
    motion_upload_location.pResource = motion_upload.Get();
    motion_upload_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    motion_upload_location.PlacedFootprint = motion_footprint;
    D3D12_TEXTURE_COPY_LOCATION motion_texture_location{};
    motion_texture_location.pResource = motion_texture_a.Get();
    motion_texture_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list_a->CopyTextureRegion(&motion_texture_location, 0, 0, 0,
                              &motion_upload_location, nullptr);
    D3D12_TEXTURE_COPY_LOCATION depth_upload_location{};
    depth_upload_location.pResource = depth_upload.Get();
    depth_upload_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    depth_upload_location.PlacedFootprint = depth_footprint;
    D3D12_TEXTURE_COPY_LOCATION depth_texture_location{};
    depth_texture_location.pResource = depth_texture_a.Get();
    depth_texture_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list_a->CopyTextureRegion(&depth_texture_location, 0, 0, 0,
                              &depth_upload_location, nullptr);
    set_transition(list_a.Get(), motion_texture_a.Get(),
                   D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    set_transition(list_a.Get(), depth_texture_a.Get(),
                   D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    HRESULT close_a = S_OK;
    if (!wait_queue(device_a.Get(), queue_a.Get(), list_a.Get(), &close_a)) return 19;

    Vkd3dInteropDevice* interop_a = nullptr;
    Vkd3dInteropDevice* interop_b = nullptr;
    hr = device_a->QueryInterface(IID_ID3D12DXVKInteropDevice4,
                                  reinterpret_cast<void**>(&interop_a));
    if (FAILED(hr) || !interop_a) return 20;
    hr = device_b->QueryInterface(IID_ID3D12DXVKInteropDevice4,
                                  reinterpret_cast<void**>(&interop_b));
    if (FAILED(hr) || !interop_b) return 21;
    INT fd_a = -1;
    INT fd_b = -1;
    HRESULT export_a = E_FAIL;
    HRESULT export_b = E_FAIL;
    bool helper_ok = false;
    bool resource_planes_ok = !resource_fd_mode;
    const auto transport_start = Clock::now();
    if (resource_fd_mode) {
        Vkd3dInteropDevice* interop6_a = nullptr;
        Vkd3dInteropDevice* interop6_b = nullptr;
        HRESULT query_a = device_a->QueryInterface(IID_ID3D12DXVKInteropDevice6,
                                                   reinterpret_cast<void**>(&interop6_a));
        HRESULT query_b = device_b->QueryInterface(IID_ID3D12DXVKInteropDevice6,
                                                   reinterpret_cast<void**>(&interop6_b));
        if (SUCCEEDED(query_a) && SUCCEEDED(query_b) && interop6_a && interop6_b) {
            ID3D12Resource* source_resources[] = {
                texture_a.Get(), motion_texture_a.Get(), depth_texture_a.Get()};
            ID3D12Resource* destination_resources[] = {
                texture_b.Get(), motion_texture_b.Get(), depth_texture_b.Get()};
            const char* resource_names[] = {"color", "motion", "depth"};
            ResourceCopyPair resource_pairs[3]{};
            resource_planes_ok = true;
            for (size_t index = 0; index < 3; ++index) {
                INT source_fd = -1;
                INT destination_fd = -1;
                UINT64 source_offset = 0;
                UINT64 destination_offset = 0;
                UINT64 source_size = 0;
                UINT64 destination_size = 0;
                HRESULT source_export = interop6_a->lpVtbl->ExportVulkanResourceFd(
                    interop6_a, source_resources[index], 1U, &source_fd,
                    &source_offset, &source_size);
                HRESULT destination_export = interop6_b->lpVtbl->ExportVulkanResourceFd(
                    interop6_b, destination_resources[index], 1U, &destination_fd,
                    &destination_offset, &destination_size);
                std::fprintf(stderr,
                             "cross_adapter_resource_export name=%s A=0x%08lx fd=%d "
                             "offset=%llu size=%llu B=0x%08lx fd=%d offset=%llu size=%llu\n",
                             resource_names[index], static_cast<unsigned long>(source_export),
                             source_fd, static_cast<unsigned long long>(source_offset),
                             static_cast<unsigned long long>(source_size),
                             static_cast<unsigned long>(destination_export), destination_fd,
                             static_cast<unsigned long long>(destination_offset),
                             static_cast<unsigned long long>(destination_size));
                const UINT64 copy_bytes = std::min(source_size, destination_size);
                const bool pair_ok = SUCCEEDED(source_export) &&
                                     SUCCEEDED(destination_export) && source_fd >= 0 &&
                                     destination_fd >= 0 && source_offset == destination_offset &&
                                     copy_bytes > 0;
                if (pair_ok) {
                    resource_pairs[index] = {
                        source_fd, source_size, source_offset,
                        destination_fd, destination_size, destination_offset,
                        copy_bytes, 0};
                }
                resource_planes_ok = resource_planes_ok && pair_ok;
                if (!pair_ok)
                    break;
            }
            if (resource_planes_ok && !resource_daemon_mode &&
                std::getenv("MGPU_CUDA_IMPORT_HELPER")) {
                resource_planes_ok = spawn_single_resource_import_helper(
                    resource_pairs[0].source_fd, resource_pairs[0].source_size,
                    resource_pairs[0].source_offset, source_ordinal, destination_ordinal,
                    std::getenv("MGPU_CUDA_IMPORT_HELPER"));
            }
            if (resource_daemon_mode) {
                helper_ok = resource_planes_ok && spawn_resource_pairs_daemon(
                    resource_pairs, 3, source_ordinal, destination_ordinal, helper,
                    resource_daemon_port, resource_daemon_repeat);
            } else {
                helper_ok = resource_planes_ok &&
                            spawn_resource_pairs_copy_helper(resource_pairs, 3,
                                                             source_ordinal, destination_ordinal,
                                                             helper, persistent_repeat_count);
            }
        }
        if (!resource_planes_ok)
            std::fprintf(stderr, "cross_adapter_resource_planes=FAIL\n");
        if (interop6_a) interop6_a->lpVtbl->Release(interop6_a);
        if (interop6_b) interop6_b->lpVtbl->Release(interop6_b);
    } else {
        export_a = interop_a->lpVtbl->ExportVulkanHeapFd(interop_a, heap_a.Get(), 1U, &fd_a);
        export_b = interop_b->lpVtbl->ExportVulkanHeapFd(interop_b, heap_b.Get(), 1U, &fd_b);
        std::fprintf(stderr, "cross_adapter_heap_export A=0x%08lx fd=%d B=0x%08lx fd=%d\n",
                     static_cast<unsigned long>(export_a), fd_a,
                     static_cast<unsigned long>(export_b), fd_b);
        const UINT64 plane_offsets[3] = {buffer_offset, motion_offset, depth_offset};
        const UINT64 plane_sizes[3] = {bytes, motion_bytes, depth_bytes};
        helper_ok = SUCCEEDED(export_a) && SUCCEEDED(export_b) && fd_a >= 0 && fd_b >= 0 &&
                     spawn_frame_copy_helper(fd_a, heap_size, source_ordinal, fd_b,
                                             heap_size, destination_ordinal,
                                             plane_offsets, plane_sizes, helper);
    }
    const auto transport_us = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - transport_start).count();
    interop_a->lpVtbl->Release(interop_a);
    interop_b->lpVtbl->Release(interop_b);
    if (!helper_ok) return 22;

    D3D12_HEAP_PROPERTIES readback_heap_props{};
    readback_heap_props.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc = buffer_desc;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Resource> motion_readback;
    ComPtr<ID3D12Resource> depth_readback;
    if (FAILED(device_b->CreateCommittedResource(
            &readback_heap_props, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return 23;
    if (resource_fd_mode) {
        D3D12_RESOURCE_DESC motion_readback_desc = readback_desc;
        motion_readback_desc.Width = motion_bytes;
        D3D12_RESOURCE_DESC depth_readback_desc = readback_desc;
        depth_readback_desc.Width = depth_bytes;
        if (FAILED(device_b->CreateCommittedResource(
                &readback_heap_props, D3D12_HEAP_FLAG_NONE, &motion_readback_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&motion_readback)))) return 23;
        if (FAILED(device_b->CreateCommittedResource(
                &readback_heap_props, D3D12_HEAP_FLAG_NONE, &depth_readback_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&depth_readback)))) return 23;
    }
    if (!resource_fd_mode) {
        D3D12_TEXTURE_COPY_LOCATION b_source{};
        b_source.pResource = buffer_b.Get();
        b_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        b_source.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION b_texture_dest{};
        b_texture_dest.pResource = texture_b.Get();
        b_texture_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list_b->CopyTextureRegion(&b_texture_dest, 0, 0, 0, &b_source, nullptr);
        D3D12_TEXTURE_COPY_LOCATION motion_source{};
        motion_source.pResource = motion_buffer_b.Get();
        motion_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        motion_source.PlacedFootprint = motion_footprint;
        D3D12_TEXTURE_COPY_LOCATION motion_dest{};
        motion_dest.pResource = motion_texture_b.Get();
        motion_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list_b->CopyTextureRegion(&motion_dest, 0, 0, 0, &motion_source, nullptr);
        D3D12_TEXTURE_COPY_LOCATION depth_source{};
        depth_source.pResource = depth_buffer_b.Get();
        depth_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        depth_source.PlacedFootprint = depth_footprint;
        D3D12_TEXTURE_COPY_LOCATION depth_dest{};
        depth_dest.pResource = depth_texture_b.Get();
        depth_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list_b->CopyTextureRegion(&depth_dest, 0, 0, 0, &depth_source, nullptr);
    } else {
        set_transition(list_b.Get(), motion_texture_b.Get(),
                       D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        set_transition(list_b.Get(), depth_texture_b.Get(),
                       D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION motion_source{};
        motion_source.pResource = motion_texture_b.Get();
        motion_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION motion_destination{};
        motion_destination.pResource = motion_readback.Get();
        motion_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        motion_destination.PlacedFootprint = motion_footprint;
        list_b->CopyTextureRegion(&motion_destination, 0, 0, 0,
                                  &motion_source, nullptr);
        D3D12_TEXTURE_COPY_LOCATION depth_source{};
        depth_source.pResource = depth_texture_b.Get();
        depth_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION depth_destination{};
        depth_destination.pResource = depth_readback.Get();
        depth_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        depth_destination.PlacedFootprint = depth_footprint;
        list_b->CopyTextureRegion(&depth_destination, 0, 0, 0,
                                  &depth_source, nullptr);
    }
    set_transition(list_b.Get(), texture_b.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    set_transition(list_b.Get(), motion_texture_b.Get(),
                   resource_fd_mode ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                    : D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    set_transition(list_b.Get(), depth_texture_b.Get(),
                   resource_fd_mode ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                    : D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_TEXTURE_COPY_LOCATION b_readback_dest{};
    b_readback_dest.pResource = readback.Get();
    b_readback_dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    b_readback_dest.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION b_texture_source{};
    b_texture_source.pResource = texture_b.Get();
    b_texture_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list_b->CopyTextureRegion(&b_readback_dest, 0, 0, 0,
                              &b_texture_source, nullptr);
    ComPtr<ID3D12Resource> ngx_output;
    ComPtr<ID3D12Resource> ngx_motion;
    ComPtr<ID3D12Resource> ngx_depth;
    ComPtr<ID3D12Resource> ngx_readback;
    ComPtr<ID3D12Resource> returned_output_a;
    ComPtr<ID3D12Resource> returned_output_readback_a;
    bool remote_output_returned = false;
    UINT64 remote_output_nonzero = 0;
    UINT64 remote_output_fnv1a = 0;
    NVSDK_NGX_Result ngx_init_result = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Result ngx_create_result = NVSDK_NGX_Result_Fail;
    NVSDK_NGX_Result ngx_evaluate_result = NVSDK_NGX_Result_Fail;
    NgxEvaluateFeature ngx_evaluate = nullptr;
    NVSDK_NGX_Handle* ngx_handle = nullptr;
    NVSDK_NGX_Parameter* ngx_parameters = nullptr;
    NgxDestroyParameters ngx_destroy_parameters = nullptr;
    NgxReleaseFeature ngx_release_feature = nullptr;
    NgxShutdown ngx_shutdown = nullptr;
    HMODULE ngx_module = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ngx_footprint{};
    UINT64 ngx_bytes = 0;
    if (ngx_requested) {
        set_transition(list_b.Get(), texture_b.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto make_committed_texture = [&](UINT texture_width, UINT texture_height,
                                          DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                          D3D12_RESOURCE_STATES state) -> ComPtr<ID3D12Resource> {
            D3D12_HEAP_PROPERTIES properties{};
            properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            description.Width = texture_width;
            description.Height = texture_height;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.Format = format;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            description.Flags = flags;
            ComPtr<ID3D12Resource> resource;
            if (FAILED(device_b->CreateCommittedResource(
                    &properties, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                    IID_PPV_ARGS(&resource))))
                resource.Reset();
            return resource;
        };
        ngx_output = make_committed_texture(
            1280, 720, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ngx_motion = motion_texture_b;
        ngx_depth = depth_texture_b;
        UINT ngx_rows = 0;
        UINT64 ngx_row_size = 0;
        if (ngx_output) {
            D3D12_RESOURCE_DESC ngx_desc = ngx_output->GetDesc();
            device_b->GetCopyableFootprints(&ngx_desc, 0, 1, 0, &ngx_footprint,
                                             &ngx_rows, &ngx_row_size, &ngx_bytes);
        }
        if (ngx_output && ngx_bytes) {
            D3D12_HEAP_PROPERTIES properties{};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC description{};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = ngx_bytes;
            description.Height = 1;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            device_b->CreateCommittedResource(
                &properties, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&ngx_readback));
            D3D12_RESOURCE_DESC returned_description = ngx_output->GetDesc();
            returned_output_a = nullptr;
            device_a->CreateCommittedResource(
                &default_properties, D3D12_HEAP_FLAG_NONE, &returned_description,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&returned_output_a));
            if (returned_output_a) {
                returned_output_readback_a = nullptr;
                device_a->CreateCommittedResource(
                    &properties, D3D12_HEAP_FLAG_NONE, &description,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&returned_output_readback_a));
            }
        }
        ngx_module = LoadLibraryW(L"nvngx_dlss.dll");
        NgxInit ngx_init = resolve_ngx<NgxInit>(ngx_module, "NVSDK_NGX_D3D12_Init_Ext");
        NgxAllocateParameters ngx_allocate =
            resolve_ngx<NgxAllocateParameters>(ngx_module, "NVSDK_NGX_D3D12_AllocateParameters");
        ngx_destroy_parameters = resolve_ngx<NgxDestroyParameters>(
            ngx_module, "NVSDK_NGX_D3D12_DestroyParameters");
        NgxCreateFeature ngx_create = resolve_ngx<NgxCreateFeature>(
            ngx_module, "NVSDK_NGX_D3D12_CreateFeature");
        ngx_evaluate = resolve_ngx<NgxEvaluateFeature>(
            ngx_module, "NVSDK_NGX_D3D12_EvaluateFeature");
        ngx_release_feature = resolve_ngx<NgxReleaseFeature>(
            ngx_module, "NVSDK_NGX_D3D12_ReleaseFeature");
        ngx_shutdown = resolve_ngx<NgxShutdown>(
            ngx_module, "NVSDK_NGX_D3D12_Shutdown1");
        if (ngx_init && ngx_allocate && ngx_create && ngx_evaluate &&
            ngx_release_feature && ngx_shutdown && ngx_output && ngx_motion &&
            ngx_depth && ngx_readback) {
            ngx_init_result = ngx_init(231313132ULL, L".", device_b.Get(),
                                       NVSDK_NGX_Version_API, nullptr);
            if (NVSDK_NGX_SUCCEED(ngx_init_result) &&
                NVSDK_NGX_SUCCEED(ngx_allocate(&ngx_parameters))) {
                ngx_parameters->Set(NVSDK_NGX_Parameter_Width, 640U);
                ngx_parameters->Set(NVSDK_NGX_Parameter_Height, 360U);
                ngx_parameters->Set(NVSDK_NGX_Parameter_OutWidth, 1280U);
                ngx_parameters->Set(NVSDK_NGX_Parameter_OutHeight, 720U);
                ngx_parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                                    (int)NVSDK_NGX_PerfQuality_Value_MaxQuality);
                ngx_parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1U);
                ngx_parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1U);
                ngx_parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 2);
                set_ngx_resource(ngx_parameters, NVSDK_NGX_Parameter_Color, texture_b.Get());
                set_ngx_resource(ngx_parameters, NVSDK_NGX_Parameter_Output, ngx_output.Get());
                set_ngx_resource(ngx_parameters, NVSDK_NGX_Parameter_MotionVectors, ngx_motion.Get());
                set_ngx_resource(ngx_parameters, NVSDK_NGX_Parameter_Depth, ngx_depth.Get());
                set_ngx_resource(ngx_parameters, "DLSSNR.Color", texture_b.Get());
                set_ngx_resource(ngx_parameters, "DLSSNR.Output", ngx_output.Get());
                set_ngx_resource(ngx_parameters, "DLSSNR.MVec", ngx_motion.Get());
                set_ngx_resource(ngx_parameters, "DLSSNR.Depth", ngx_depth.Get());
                ngx_parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
                ngx_parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
                ngx_parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
                ngx_parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
                ngx_parameters->Set(NVSDK_NGX_Parameter_Reset, 1);
                ngx_parameters->Set("DLSSNR.ColorSubrectWidth", 640U);
                ngx_parameters->Set("DLSSNR.ColorSubrectHeight", 360U);
                ngx_parameters->Set("DLSSNR.OutputSubrectWidth", 1280U);
                ngx_parameters->Set("DLSSNR.OutputSubrectHeight", 720U);
                ngx_parameters->Set("DLSSNR.MVecSubrectWidth", 640U);
                ngx_parameters->Set("DLSSNR.MVecSubrectHeight", 360U);
                ngx_parameters->Set("DLSSNR.DepthSubrectWidth", 640U);
                ngx_parameters->Set("DLSSNR.DepthSubrectHeight", 360U);
                ngx_parameters->Set("DLSSNR.MVecScaleX", 1.0f);
                ngx_parameters->Set("DLSSNR.MVecScaleY", 1.0f);
                ngx_parameters->Set("DLSSNR.Enabled", 1);
                ngx_create_result = ngx_create(list_b.Get(), NVSDK_NGX_Feature_SuperSampling,
                                               ngx_parameters, &ngx_handle);
                if (NVSDK_NGX_SUCCEED(ngx_create_result) && ngx_handle) {
                    ngx_evaluate_result = ngx_evaluate(
                        list_b.Get(), ngx_handle, ngx_parameters, nullptr);
                    set_transition(list_b.Get(), ngx_output.Get(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
                    D3D12_TEXTURE_COPY_LOCATION output_source{};
                    output_source.pResource = ngx_output.Get();
                    output_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    D3D12_TEXTURE_COPY_LOCATION output_destination{};
                    output_destination.pResource = ngx_readback.Get();
                    output_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    output_destination.PlacedFootprint = ngx_footprint;
                    list_b->CopyTextureRegion(&output_destination, 0, 0, 0,
                                              &output_source, nullptr);
                }
            }
        }
    }
    HRESULT close_b = S_OK;
    const auto queue_b_start = Clock::now();
    const bool queue_ok = wait_queue(device_b.Get(), queue_b.Get(), list_b.Get(), &close_b);
    const auto queue_b_us = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - queue_b_start).count();
    if (!queue_ok) {
        std::fprintf(stderr,
                     "cross_adapter_queue_b_failed close=0x%08lx removed=0x%08lx\n",
                     static_cast<unsigned long>(close_b),
                     static_cast<unsigned long>(device_b->GetDeviceRemovedReason()));
        return 24;
    }
    if (ngx_requested && NVSDK_NGX_SUCCEED(ngx_evaluate_result)) {
        ngx_frames_completed = 1;
        for (int frame = 1; frame < ngx_frame_count; ++frame) {
            ComPtr<ID3D12CommandAllocator> frame_allocator;
            ComPtr<ID3D12GraphicsCommandList> frame_list;
            HRESULT frame_setup = device_b->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame_allocator));
            if (SUCCEEDED(frame_setup)) {
                frame_setup = device_b->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame_allocator.Get(), nullptr,
                    IID_PPV_ARGS(&frame_list));
            }
            if (SUCCEEDED(frame_setup)) {
                ngx_parameters->Set(NVSDK_NGX_Parameter_Reset, 0);
                ngx_evaluate_result = ngx_evaluate(
                    frame_list.Get(), ngx_handle, ngx_parameters, nullptr);
                HRESULT frame_close = S_OK;
                const bool frame_queue_ok = wait_queue(
                    device_b.Get(), queue_b.Get(), frame_list.Get(), &frame_close);
                std::fprintf(stderr,
                             "cross_adapter_ngx_frame index=%d evaluate=0x%08x "
                             "queue=%s close=0x%08lx removed=0x%08lx\n",
                             frame + 1, static_cast<unsigned int>(ngx_evaluate_result),
                             frame_queue_ok ? "ok" : "FAIL",
                             static_cast<unsigned long>(frame_close),
                             static_cast<unsigned long>(device_b->GetDeviceRemovedReason()));
                if (!frame_queue_ok || NVSDK_NGX_FAILED(ngx_evaluate_result)) break;
                ++ngx_frames_completed;
            } else {
                std::fprintf(stderr,
                             "cross_adapter_ngx_frame_setup index=%d hr=0x%08lx\n",
                             frame + 1, static_cast<unsigned long>(frame_setup));
                break;
            }
        }
    }

    if (resource_daemon_mode && ngx_requested &&
        NVSDK_NGX_SUCCEED(ngx_evaluate_result) && ngx_output && returned_output_a &&
        returned_output_readback_a && ngx_bytes) {
        Vkd3dInteropDevice* interop_output_b = nullptr;
        Vkd3dInteropDevice* interop_output_a = nullptr;
        const HRESULT query_output_b = device_b->QueryInterface(
            IID_ID3D12DXVKInteropDevice6,
            reinterpret_cast<void**>(&interop_output_b));
        const HRESULT query_output_a = device_a->QueryInterface(
            IID_ID3D12DXVKInteropDevice6,
            reinterpret_cast<void**>(&interop_output_a));
        if (SUCCEEDED(query_output_b) && SUCCEEDED(query_output_a) &&
            interop_output_b && interop_output_a) {
            INT output_fd_b = -1;
            INT output_fd_a = -1;
            UINT64 output_offset_b = 0;
            UINT64 output_offset_a = 0;
            UINT64 output_size_b = 0;
            UINT64 output_size_a = 0;
            const HRESULT export_output_b =
                interop_output_b->lpVtbl->ExportVulkanResourceFd(
                    interop_output_b, ngx_output.Get(), 1U, &output_fd_b,
                    &output_offset_b, &output_size_b);
            const HRESULT export_output_a =
                interop_output_a->lpVtbl->ExportVulkanResourceFd(
                    interop_output_a, returned_output_a.Get(), 1U, &output_fd_a,
                    &output_offset_a, &output_size_a);
            const UINT64 output_bytes = std::min(output_size_b, output_size_a);
            ResourceCopyPair output_pair{
                output_fd_b, output_size_b, output_offset_b,
                output_fd_a, output_size_a, output_offset_a,
                output_bytes, 0};
            remote_output_returned = SUCCEEDED(export_output_b) &&
                SUCCEEDED(export_output_a) && output_fd_b >= 0 && output_fd_a >= 0 &&
                output_offset_b == output_offset_a && output_bytes >= ngx_bytes &&
                spawn_resource_pairs_daemon(&output_pair, 1, destination_ordinal,
                                            source_ordinal, helper,
                                            resource_daemon_output_port, 1);
            std::fprintf(stderr,
                         "cross_adapter_output_export B=0x%08lx fd=%d size=%llu "
                         "A=0x%08lx fd=%d size=%llu returned=%s\n",
                         static_cast<unsigned long>(export_output_b), output_fd_b,
                         static_cast<unsigned long long>(output_size_b),
                         static_cast<unsigned long>(export_output_a), output_fd_a,
                         static_cast<unsigned long long>(output_size_a),
                         remote_output_returned ? "ok" : "FAIL");
        }
        if (interop_output_b)
            interop_output_b->lpVtbl->Release(interop_output_b);
        if (interop_output_a)
            interop_output_a->lpVtbl->Release(interop_output_a);
    }
    if (remote_output_returned) {
        D3D12_COMMAND_QUEUE_DESC output_queue_desc{};
        output_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> output_queue_a;
        ComPtr<ID3D12CommandAllocator> output_allocator_a;
        ComPtr<ID3D12GraphicsCommandList> output_list_a;
        if (SUCCEEDED(device_a->CreateCommandQueue(
                &output_queue_desc, IID_PPV_ARGS(&output_queue_a))) &&
            SUCCEEDED(device_a->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&output_allocator_a))) &&
            SUCCEEDED(device_a->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, output_allocator_a.Get(), nullptr,
                IID_PPV_ARGS(&output_list_a)))) {
            set_transition(output_list_a.Get(), returned_output_a.Get(),
                           D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION output_source_a{};
            output_source_a.pResource = returned_output_a.Get();
            output_source_a.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION output_destination_a{};
            output_destination_a.pResource = returned_output_readback_a.Get();
            output_destination_a.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            output_destination_a.PlacedFootprint = ngx_footprint;
            output_list_a->CopyTextureRegion(&output_destination_a, 0, 0, 0,
                                              &output_source_a, nullptr);
            HRESULT output_close = S_OK;
            if (wait_queue(device_a.Get(), output_queue_a.Get(), output_list_a.Get(),
                           &output_close)) {
                void* output_mapped = nullptr;
                D3D12_RANGE output_range{0, static_cast<SIZE_T>(ngx_bytes)};
                const HRESULT output_map = returned_output_readback_a->Map(
                    0, &output_range, &output_mapped);
                if (SUCCEEDED(output_map) && output_mapped) {
                    const auto* output_bytes_ptr =
                        static_cast<const unsigned char*>(output_mapped);
                    remote_output_fnv1a = fnv1a(output_bytes_ptr, ngx_bytes);
                    for (UINT64 index = 0; index < ngx_bytes; ++index)
                        if (output_bytes_ptr[index] != 0) ++remote_output_nonzero;
                    D3D12_RANGE output_written{0, 0};
                    returned_output_readback_a->Unmap(0, &output_written);
                }
                remote_output_returned = remote_output_returned &&
                    SUCCEEDED(output_map) && output_mapped && remote_output_nonzero > 0;
                std::fprintf(stderr,
                             "cross_adapter_output_readback map=0x%08lx nonzero=%llu "
                             "fnv1a=0x%016llx validation=%s\n",
                             static_cast<unsigned long>(output_map),
                             static_cast<unsigned long long>(remote_output_nonzero),
                             static_cast<unsigned long long>(remote_output_fnv1a),
                             remote_output_returned ? "ok" : "FAIL");
            } else {
                remote_output_returned = false;
            }
        } else {
            remote_output_returned = false;
        }
    }

    unsigned char first[8]{};
    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(bytes)};
    hr = readback->Map(0, &read_range, &mapped);
    if (SUCCEEDED(hr) && mapped) std::memcpy(first, mapped, sizeof(first));
    D3D12_RANGE no_write{0, 0};
    if (mapped) readback->Unmap(0, &no_write);
    const unsigned char expected[8] = {0x00, 0x34, 0x00, 0x38,
                                       0x00, 0x3a, 0x00, 0x3c};
    const bool valid = SUCCEEDED(hr) && std::memcmp(first, expected, sizeof(first)) == 0;
    std::fprintf(stderr, "cross_adapter_readback map=0x%08lx first=%02x%02x%02x%02x%02x%02x%02x%02x validation=%s\n",
                 static_cast<unsigned long>(hr), first[0], first[1], first[2], first[3],
                 first[4], first[5], first[6], first[7], valid ? "ok" : "FAIL");
    bool resource_planes_readback = true;
    if (resource_fd_mode) {
        struct PlaneReadback {
            const char* name;
            ID3D12Resource* resource;
            UINT64 bytes;
        } planes[] = {
            {"motion", motion_readback.Get(), motion_bytes},
            {"depth", depth_readback.Get(), depth_bytes},
        };
        for (const PlaneReadback& plane : planes) {
            void* plane_mapped = nullptr;
            D3D12_RANGE plane_range{0, static_cast<SIZE_T>(plane.bytes)};
            HRESULT plane_hr = plane.resource->Map(0, &plane_range, &plane_mapped);
            const unsigned char first_byte = plane_mapped
                ? *static_cast<const unsigned char*>(plane_mapped) : 0xff;
            if (plane_mapped) plane.resource->Unmap(0, &no_write);
            const bool plane_valid = SUCCEEDED(plane_hr) && plane_mapped && first_byte == 0;
            resource_planes_readback = resource_planes_readback && plane_valid;
            std::fprintf(stderr,
                         "cross_adapter_%s_readback map=0x%08lx first=%02x validation=%s\n",
                         plane.name, static_cast<unsigned long>(plane_hr), first_byte,
                         plane_valid ? "ok" : "FAIL");
        }
    }
    bool ngx_readback_valid = true;
    UINT64 ngx_fnv1a = 0;
    if (ngx_requested && ngx_readback) {
        void* ngx_mapped = nullptr;
        D3D12_RANGE ngx_read_range{0, static_cast<SIZE_T>(ngx_bytes)};
        HRESULT ngx_map = ngx_readback->Map(0, &ngx_read_range, &ngx_mapped);
        UINT64 ngx_nonzero = 0;
        if (SUCCEEDED(ngx_map) && ngx_mapped) {
            const unsigned char* ngx_bytes_ptr =
                static_cast<const unsigned char*>(ngx_mapped);
            ngx_fnv1a = fnv1a(ngx_bytes_ptr, ngx_bytes);
            for (UINT64 i = 0; i < ngx_bytes; ++i)
                if (ngx_bytes_ptr[i] != 0) ++ngx_nonzero;
            D3D12_RANGE ngx_written{0, 0};
            ngx_readback->Unmap(0, &ngx_written);
        }
        ngx_readback_valid = SUCCEEDED(ngx_map) && ngx_mapped != nullptr && ngx_nonzero > 0;
        std::fprintf(stderr, "cross_adapter_ngx init=0x%08x create=0x%08x evaluate=0x%08x readback_map=0x%08lx nonzero=%llu fnv1a=0x%016llx validation=%s\n",
                     static_cast<unsigned int>(ngx_init_result),
                     static_cast<unsigned int>(ngx_create_result),
                     static_cast<unsigned int>(ngx_evaluate_result),
                     static_cast<unsigned long>(ngx_map),
                     static_cast<unsigned long long>(ngx_nonzero),
                     static_cast<unsigned long long>(ngx_fnv1a),
                     ngx_readback_valid ? "ok" : "FAIL");
    }
    if (ngx_handle && ngx_release_feature)
        ngx_release_feature(ngx_handle);
    if (ngx_parameters && ngx_destroy_parameters)
        ngx_destroy_parameters(ngx_parameters);
    if (ngx_shutdown && NVSDK_NGX_SUCCEED(ngx_init_result))
        ngx_shutdown(device_b.Get());
    if (ngx_module) FreeLibrary(ngx_module);
    const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - total_start).count();
    std::printf("{\"gpu_a_to_b\":true,\"reverse_direction\":%s,\"source_cuda_ordinal\":%d,\"destination_cuda_ordinal\":%d,\"persistent_worker_iterations\":%d,\"resource_fd_mode\":%s,\"resource_daemon_mode\":%s,\"resource_daemon_commands\":%d,\"remote_output_returned\":%s,\"remote_output_nonzero\":%llu,\"remote_output_fnv1a\":\"0x%016llx\",\"resource_planes_readback\":%s,\"helper_p2p\":%s,\"queue_a_cpu_fence\":true,\"queue_b_cpu_fence\":true,\"readback_validation\":%s,\"ngx_requested\":%s,\"ngx_b_evaluate\":%s,\"ngx_b_frames_requested\":%d,\"ngx_b_frames_completed\":%d,\"ngx_b_readback\":%s,\"transport_us\":%lld,\"queue_b_us\":%lld,\"total_us\":%lld,\"bytes\":%llu}\n",
                reverse_direction ? "true" : "false", source_ordinal, destination_ordinal,
                persistent_repeat_count,
                resource_fd_mode ? "true" : "false",
                resource_daemon_mode ? "true" : "false",
                resource_daemon_mode ? resource_daemon_repeat : 0,
                remote_output_returned ? "true" : "false",
                static_cast<unsigned long long>(remote_output_nonzero),
                static_cast<unsigned long long>(remote_output_fnv1a),
                resource_planes_readback ? "true" : "false",
                helper_ok ? "true" : "false", valid ? "true" : "false",
                ngx_requested ? "true" : "false",
                NVSDK_NGX_SUCCEED(ngx_evaluate_result) ? "true" : "false",
                ngx_frame_count,
                ngx_frames_completed,
                (ngx_requested && ngx_readback_valid) ? "true" : "false",
                static_cast<long long>(transport_us),
                static_cast<long long>(queue_b_us),
                static_cast<long long>(total_us),
                static_cast<unsigned long long>(bytes));
    return valid && resource_planes_readback && (!ngx_requested ||
                     (NVSDK_NGX_SUCCEED(ngx_evaluate_result) &&
                      ngx_frames_completed == ngx_frame_count && ngx_readback_valid &&
                      (!resource_daemon_mode || remote_output_returned))) ? 0 : 25;
}
