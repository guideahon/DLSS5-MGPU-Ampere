#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

struct InteropDevice;
struct InteropVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(InteropDevice *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(InteropDevice *);
    ULONG (STDMETHODCALLTYPE *Release)(InteropDevice *);
    HRESULT (STDMETHODCALLTYPE *GetDXGIAdapter)(InteropDevice *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *GetInstanceExtensions)(InteropDevice *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceExtensions)(InteropDevice *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceFeatures)(InteropDevice *, const void **);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHandles)(InteropDevice *, void **, void **, void **);
    HRESULT (STDMETHODCALLTYPE *GetVulkanQueueInfo)(InteropDevice *, ID3D12CommandQueue *, void **, UINT32 *);
    void (STDMETHODCALLTYPE *GetVulkanImageLayout)(InteropDevice *, ID3D12Resource *, D3D12_RESOURCE_STATES, int *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo)(InteropDevice *, ID3D12Resource *, UINT64 *, UINT64 *);
    HRESULT (STDMETHODCALLTYPE *LockCommandQueue)(InteropDevice *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *UnlockCommandQueue)(InteropDevice *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo1)(InteropDevice *, ID3D12Resource *, UINT64 *, UINT64 *, int *);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandQueue)(InteropDevice *, const D3D12_COMMAND_QUEUE_DESC *, UINT32, ID3D12CommandQueue **);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandAllocator)(InteropDevice *, D3D12_COMMAND_LIST_TYPE, UINT32, ID3D12CommandAllocator **);
    HRESULT (STDMETHODCALLTYPE *BeginVkCommandBufferInterop)(InteropDevice *, ID3D12CommandList *, void **);
    HRESULT (STDMETHODCALLTYPE *EndVkCommandBufferInterop)(InteropDevice *, ID3D12CommandList *);
    HRESULT (STDMETHODCALLTYPE *LockVulkanQueue)(InteropDevice *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *UnlockVulkanQueue)(InteropDevice *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHeapInfo)(InteropDevice *, ID3D12Heap *, UINT64 *, UINT64 *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *ExportVulkanHeapFd)(InteropDevice *, ID3D12Heap *, UINT32, INT *);
    HRESULT (STDMETHODCALLTYPE *ExportVulkanFenceFd)(InteropDevice *, ID3D12Fence *, UINT32, INT *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanPhysicalDeviceIdentity)(InteropDevice *, UINT8 *, UINT32 *, UINT32 *, UINT32 *, UINT32 *);
    HRESULT (STDMETHODCALLTYPE *ExportVulkanResourceFd)(InteropDevice *, ID3D12Resource *, UINT32, INT *, UINT64 *, UINT64 *);
};
struct InteropDevice { const InteropVtbl *lpVtbl; };

static const GUID IID_Interop5 =
    {0x5f7f64b7, 0x8e0d, 0x4aa8, {0x9e, 0x29, 0x4b, 0x2f, 0x1b, 0x3d, 0x7e, 0x61}};
static const GUID IID_Interop6 =
    {0x6a4b7d2e, 0x2c52, 0x4e11, {0x9c, 0x86, 0x2f, 0x0a, 0xf5, 0xf8, 0xb0, 0xc3}};

static bool wait_status(const char *path, const char *needle, unsigned timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream file(path);
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        if (content.find(needle) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static bool wait_queue(ID3D12Device *device, ID3D12CommandQueue *queue,
                       ID3D12GraphicsCommandList *list)
{
    if (FAILED(list->Close())) return false;
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&fence)))) return false;
    if (FAILED(queue->Signal(fence.Get(), 1))) return false;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    HRESULT hr = fence->SetEventOnCompletion(1, event);
    DWORD wait_result = hr == S_OK ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
    CloseHandle(event);
    return SUCCEEDED(hr) && wait_result == WAIT_OBJECT_0;
}

static bool spawn_fenced_helper(const char *helper, int source_ordinal,
                                int destination_ordinal, int wait_fd, int signal_fd,
                                UINT64 value, const char *status, const char *gate,
                                int source_resource_fd, UINT64 source_size,
                                UINT64 source_offset, int destination_resource_fd,
                                UINT64 destination_size, UINT64 destination_offset,
                                UINT64 bytes, unsigned expected)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using Spawn = LONG (WINAPI *)(char *const[], int);
    auto spawn = ntdll ? reinterpret_cast<Spawn>(
        GetProcAddress(ntdll, "__wine_unix_spawnvp")) : nullptr;
    if (!spawn) {
        std::fprintf(stderr, "fenced_helper_spawn=unavailable ntdll=%p\n", ntdll);
        return false;
    }
    char args[20][64]{};
    std::snprintf(args[0], sizeof(args[0]), "%d", source_ordinal);
    std::snprintf(args[1], sizeof(args[1]), "%d", destination_ordinal);
    std::snprintf(args[2], sizeof(args[2]), "%d", wait_fd);
    std::snprintf(args[3], sizeof(args[3]), "%d", signal_fd);
    std::snprintf(args[4], sizeof(args[4]), "%llu", static_cast<unsigned long long>(value));
    std::snprintf(args[5], sizeof(args[5]), "%d", 1);
    std::snprintf(args[6], sizeof(args[6]), "%d", source_resource_fd);
    std::snprintf(args[7], sizeof(args[7]), "%llu", static_cast<unsigned long long>(source_size));
    std::snprintf(args[8], sizeof(args[8]), "%llu", static_cast<unsigned long long>(source_offset));
    std::snprintf(args[9], sizeof(args[9]), "%d", destination_resource_fd);
    std::snprintf(args[10], sizeof(args[10]), "%llu", static_cast<unsigned long long>(destination_size));
    std::snprintf(args[11], sizeof(args[11]), "%llu", static_cast<unsigned long long>(destination_offset));
    std::snprintf(args[12], sizeof(args[12]), "%llu", static_cast<unsigned long long>(bytes));
    std::snprintf(args[13], sizeof(args[13]), "%02x", expected & 0xffU);
    char *argv[] = {
        const_cast<char *>(helper), const_cast<char *>("--fenced-pairs"),
        args[0], args[1], args[2], args[3], args[4], args[5],
        const_cast<char *>(status), const_cast<char *>(gate),
        args[6], args[7], args[8], args[9], args[10], args[11], args[12], args[13], nullptr};
    std::string inherit = std::to_string(wait_fd) + "," + std::to_string(signal_fd) + "," +
        std::to_string(source_resource_fd) + "," + std::to_string(destination_resource_fd);
    SetEnvironmentVariableA("MGPU_INHERIT_FD", inherit.c_str());
    LONG result = spawn(argv, 0);
    SetEnvironmentVariableA("MGPU_INHERIT_FD", nullptr);
    std::fprintf(stderr, "fenced_helper_spawn=%s rc=%ld wait_fd=%d signal_fd=%d source_fd=%d destination_fd=%d\n",
                 result == 0 ? "ok" : "fail", static_cast<long>(result), wait_fd,
                 signal_fd, source_resource_fd, destination_resource_fd);
    return result == 0;
}

static bool spawn_fenced_persistent_helper(const char *helper, int source_ordinal,
                                           int destination_ordinal, int fence_count,
                                           const int *wait_fds, const int *signal_fds,
                                           const char *status,
                                           const char *command, int source_resource_fd,
                                           UINT64 source_size, UINT64 source_offset,
                                           int destination_resource_fd,
                                           UINT64 destination_size, UINT64 destination_offset,
                                           UINT64 bytes)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using Spawn = LONG (WINAPI *)(char *const[], int);
    auto spawn = ntdll ? reinterpret_cast<Spawn>(
        GetProcAddress(ntdll, "__wine_unix_spawnvp")) : nullptr;
    if (!spawn) {
        std::fprintf(stderr, "fenced_persistent_spawn=unavailable\n");
        return false;
    }
    char args[64][64]{};
    std::snprintf(args[0], sizeof(args[0]), "%d", source_ordinal);
    std::snprintf(args[1], sizeof(args[1]), "%d", destination_ordinal);
    std::snprintf(args[2], sizeof(args[2]), "%d", fence_count);
    int argument = 3;
    std::string inherit;
    for (int index = 0; index < fence_count; ++index) {
        std::snprintf(args[argument], sizeof(args[argument]), "%d", wait_fds[index]);
        inherit += (inherit.empty() ? "" : ",") + std::string(args[argument]);
        ++argument;
        std::snprintf(args[argument], sizeof(args[argument]), "%d", signal_fds[index]);
        inherit += "," + std::string(args[argument]);
        ++argument;
    }
    const int resource_base = argument;
    std::snprintf(args[resource_base], sizeof(args[resource_base]), "%d", source_resource_fd);
    std::snprintf(args[resource_base + 1], sizeof(args[resource_base + 1]), "%llu", static_cast<unsigned long long>(source_size));
    std::snprintf(args[resource_base + 2], sizeof(args[resource_base + 2]), "%llu", static_cast<unsigned long long>(source_offset));
    std::snprintf(args[resource_base + 3], sizeof(args[resource_base + 3]), "%d", destination_resource_fd);
    std::snprintf(args[resource_base + 4], sizeof(args[resource_base + 4]), "%llu", static_cast<unsigned long long>(destination_size));
    std::snprintf(args[resource_base + 5], sizeof(args[resource_base + 5]), "%llu", static_cast<unsigned long long>(destination_offset));
    std::snprintf(args[resource_base + 6], sizeof(args[resource_base + 6]), "%llu", static_cast<unsigned long long>(bytes));
    std::snprintf(args[resource_base + 7], sizeof(args[resource_base + 7]), "%02x", 0);
    inherit += "," + std::to_string(source_resource_fd) + "," +
        std::to_string(destination_resource_fd);
    char *argv[80]{};
    int argv_count = 0;
    argv[argv_count++] = const_cast<char *>(helper);
    argv[argv_count++] = const_cast<char *>("--fenced-pair-persistent");
    argv[argv_count++] = args[0];
    argv[argv_count++] = args[1];
    argv[argv_count++] = args[2];
    argv[argv_count++] = const_cast<char *>(status);
    argv[argv_count++] = const_cast<char *>(command);
    for (int index = 3; index < resource_base + 8; ++index)
        argv[argv_count++] = args[index];
    argv[argv_count] = nullptr;
    SetEnvironmentVariableA("MGPU_INHERIT_FD", inherit.c_str());
    LONG result = spawn(argv, 0);
    SetEnvironmentVariableA("MGPU_INHERIT_FD", nullptr);
    std::fprintf(stderr, "fenced_persistent_spawn=%s rc=%ld\n",
                 result == 0 ? "ok" : "fail", static_cast<long>(result));
    return result == 0;
}

static bool write_command(const char *path, const std::string &command)
{
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    file << command << "\n";
    file.flush();
    return static_cast<bool>(file);
}

static bool run_persistent_frame_loop(
    const char *helper, const char *status_path, const char *command_path,
    int frame_count, ID3D12Device *device_a, ID3D12Device *device_b,
    ID3D12CommandQueue *queue_a, ID3D12CommandQueue *queue_b,
    ID3D12CommandAllocator *allocator_a, ID3D12CommandAllocator *allocator_b,
    ID3D12GraphicsCommandList *list_a, ID3D12GraphicsCommandList *list_b,
    ID3D12Resource *source, ID3D12Resource *upload, ID3D12Resource *destination,
    ID3D12Resource *readback, InteropDevice *interop_a, InteropDevice *interop_b,
    int source_fd, UINT64 source_size, UINT64 source_offset, int destination_fd,
    UINT64 destination_size, UINT64 destination_offset, UINT64 bytes)
{
    std::remove(status_path);
    std::remove(command_path);
    ComPtr<ID3D12Fence> fences_a[16], fences_b[16];
    int wait_fds[16]{}, signal_fds[16]{};
    for (int index = 0; index < frame_count; ++index) {
        if (FAILED(device_a->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                         IID_PPV_ARGS(&fences_a[index]))) ||
            FAILED(device_b->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                         IID_PPV_ARGS(&fences_b[index]))) ||
            FAILED(interop_a->lpVtbl->ExportVulkanFenceFd(
                interop_a, fences_a[index].Get(), 1U, &wait_fds[index])) ||
            FAILED(interop_b->lpVtbl->ExportVulkanFenceFd(
                interop_b, fences_b[index].Get(), 1U, &signal_fds[index]))) {
            std::fprintf(stderr, "persistent_fence_slot=%d export=fail\n", index);
            return false;
        }
    }
    if (!spawn_fenced_persistent_helper(helper, 0, 1, frame_count, wait_fds, signal_fds,
                                        status_path, command_path, source_fd, source_size,
                                        source_offset, destination_fd, destination_size,
                                        destination_offset, bytes) ||
        !wait_status(status_path, "ready rc=0", 5000)) {
        std::fprintf(stderr, "persistent_helper_ready=fail\n");
        return false;
    }

    int completed = 0;
    bool all_ok = true;
    for (int frame = 0; frame < frame_count && all_ok; ++frame) {
        void *mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped)) || !mapped) {
            std::fprintf(stderr, "persistent_frame=%d upload_map=fail\n", frame);
            all_ok = false; break;
        }
        auto *bytes_ptr = static_cast<unsigned char *>(mapped);
        const unsigned expected = static_cast<unsigned>((0x61 + frame) & 0xffU);
        for (UINT64 index = 0; index < bytes; ++index)
            bytes_ptr[index] = static_cast<unsigned char>((index + expected) & 0xffU);
        bytes_ptr[0] = static_cast<unsigned char>(expected);
        upload->Unmap(0, nullptr);
        if (FAILED(allocator_a->Reset()) || FAILED(list_a->Reset(allocator_a, nullptr))) {
            std::fprintf(stderr, "persistent_frame=%d source_reset=fail\n", frame);
            all_ok = false; break;
        }
        list_a->CopyBufferRegion(source, 0, upload, 0, bytes);
        if (FAILED(list_a->Close())) {
            std::fprintf(stderr, "persistent_frame=%d source_close=fail\n", frame);
            all_ok = false; break;
        }
        ID3D12CommandList *source_lists[] = {list_a};
        queue_a->ExecuteCommandLists(1, source_lists);
        if (FAILED(queue_a->Signal(fences_a[frame].Get(), 1)) ||
            !write_command(command_path, "go " + std::to_string(frame))) {
            std::fprintf(stderr, "persistent_frame=%d producer_signal=fail\n", frame);
            all_ok = false; break;
        }
        const std::string done = "done " + std::to_string(frame) + " rc=0";
        if (!wait_status(status_path, done.c_str(), 10000)) {
            std::fprintf(stderr, "persistent_frame=%d helper_done=fail\n", frame);
            all_ok = false; break;
        }
        if (FAILED(queue_b->Wait(fences_b[frame].Get(), 1)) ||
            FAILED(allocator_b->Reset()) || FAILED(list_b->Reset(allocator_b, nullptr))) {
            std::fprintf(stderr, "persistent_frame=%d consumer_wait=fail\n", frame);
            all_ok = false; break;
        }
        list_b->CopyBufferRegion(readback, 0, destination, 0, bytes);
        if (!wait_queue(device_b, queue_b, list_b)) {
            std::fprintf(stderr, "persistent_frame=%d consumer_queue=fail\n", frame);
            all_ok = false; break;
        }
        void *readback_ptr = nullptr;
        if (FAILED(readback->Map(0, nullptr, &readback_ptr)) || !readback_ptr ||
            static_cast<unsigned char *>(readback_ptr)[0] != expected) {
            std::fprintf(stderr, "persistent_frame=%d readback=fail\n", frame);
            all_ok = false;
        }
        readback->Unmap(0, nullptr);
        if (all_ok) ++completed;
    }
    if (!write_command(command_path, "quit") ||
        !wait_status(status_path, "stopped", 5000)) {
        std::fprintf(stderr, "persistent_helper_stop=fail\n");
        all_ok = false;
    }
    std::fprintf(stderr, "persistent_frame_loop=%s frames=%d/%d\n",
                 all_ok && completed == frame_count ? "pass" : "fail",
                 completed, frame_count);
    return all_ok && completed == frame_count;
}

int main()
{
    const int frame_count = std::clamp(std::getenv("MGPU_GPU_SYNC_FRAMES") ?
        std::atoi(std::getenv("MGPU_GPU_SYNC_FRAMES")) : 3, 1, 16);
    const char *helper = std::getenv("MGPU_FENCED_P2P_HELPER");
    const char *out_dir = std::getenv("MGPU_GPU_SYNC_OUT");
    if (!helper || !*helper || !out_dir || !*out_dir) {
        std::fprintf(stderr, "MGPU_FENCED_P2P_HELPER and MGPU_GPU_SYNC_OUT are required\n");
        return 2;
    }
    const std::string status_path = std::string(out_dir) + "/fenced-p2p-status.log";
    const std::string gate_path = status_path + ".gate";
    std::remove(status_path.c_str());
    std::remove(gate_path.c_str());

    SetEnvironmentVariableA("VKD3D_DUPLICATE_LUID_ADAPTERS", "1");
    SetEnvironmentVariableA("VKD3D_DUPLICATE_LUID_INDEX", "0");
    ComPtr<ID3D12Device> device_a;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                                   IID_PPV_ARGS(&device_a));
    SetEnvironmentVariableA("VKD3D_DUPLICATE_LUID_INDEX", "1");
    ComPtr<ID3D12Device> device_b;
    if (SUCCEEDED(hr)) hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                                               IID_PPV_ARGS(&device_b));
    SetEnvironmentVariableA("VKD3D_DUPLICATE_LUID_INDEX", nullptr);
    if (FAILED(hr)) return 5;

    constexpr UINT64 bytes = 4 * 1024 * 1024;
    D3D12_HEAP_DESC heap_desc{};
    heap_desc.SizeInBytes = bytes;
    heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Heap> heap_a, heap_b;
    if (FAILED(device_a->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap_a))) ||
        FAILED(device_b->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap_b)))) return 6;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> source, destination, upload, readback;
    if (FAILED(device_a->CreatePlacedResource(heap_a.Get(), 0, &desc,
             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&source))) ||
        FAILED(device_b->CreatePlacedResource(heap_b.Get(), 0, &desc,
             D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&destination)))) return 7;
    D3D12_HEAP_PROPERTIES upload_props{};
    upload_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES readback_props{};
    readback_props.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device_a->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE,
             &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) ||
        FAILED(device_b->CreateCommittedResource(&readback_props, D3D12_HEAP_FLAG_NONE,
             &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return 8;

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue_a, queue_b;
    ComPtr<ID3D12CommandAllocator> allocator_a, allocator_b;
    ComPtr<ID3D12GraphicsCommandList> list_a, list_b;
    if (FAILED(device_a->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_a))) ||
        FAILED(device_b->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_b))) ||
        FAILED(device_a->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_a))) ||
        FAILED(device_b->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_b))) ||
        FAILED(device_a->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_a.Get(), nullptr, IID_PPV_ARGS(&list_a))) ||
        FAILED(device_b->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_b.Get(), nullptr, IID_PPV_ARGS(&list_b)))) return 9;
    if (FAILED(list_a->Close()) || FAILED(list_b->Close())) {
        std::fprintf(stderr, "initial_command_close=fail\n");
        return 9;
    }

    InteropDevice *interop_a = nullptr, *interop_b = nullptr;
    if (FAILED(device_a->QueryInterface(IID_Interop5, reinterpret_cast<void **>(&interop_a))) ||
        FAILED(device_b->QueryInterface(IID_Interop5, reinterpret_cast<void **>(&interop_b)))) return 10;
    InteropDevice *interop6_a = nullptr, *interop6_b = nullptr;
    if (FAILED(device_a->QueryInterface(IID_Interop6, reinterpret_cast<void **>(&interop6_a))) ||
        FAILED(device_b->QueryInterface(IID_Interop6, reinterpret_cast<void **>(&interop6_b)))) return 11;
    INT source_fd = -1, destination_fd = -1;
    UINT64 source_offset = 0, destination_offset = 0, source_size = 0, destination_size = 0;
    hr = interop6_a->lpVtbl->ExportVulkanResourceFd(interop6_a, source.Get(), 1U, &source_fd,
                                                    &source_offset, &source_size);
    if (SUCCEEDED(hr)) hr = interop6_b->lpVtbl->ExportVulkanResourceFd(
        interop6_b, destination.Get(), 1U, &destination_fd, &destination_offset, &destination_size);
    std::printf("resource_export_hr=0x%08lx source_fd=%d destination_fd=%d\n",
                static_cast<unsigned long>(hr), source_fd, destination_fd);
    if (FAILED(hr) || source_fd < 0 || destination_fd < 0 ||
        source_offset != destination_offset) return 12;

    const bool persistent = !std::getenv("MGPU_GPU_SYNC_PERSISTENT") ||
        std::strcmp(std::getenv("MGPU_GPU_SYNC_PERSISTENT"), "0") != 0;
    if (persistent) {
        const std::string command_path = status_path + ".command";
        const bool passed = run_persistent_frame_loop(
            helper, status_path.c_str(), command_path.c_str(), frame_count,
            device_a.Get(), device_b.Get(), queue_a.Get(), queue_b.Get(),
            allocator_a.Get(), allocator_b.Get(), list_a.Get(), list_b.Get(),
            source.Get(), upload.Get(), destination.Get(), readback.Get(),
            interop_a, interop_b, source_fd, source_size, source_offset,
            destination_fd, destination_size, destination_offset,
            std::min(source_size, destination_size));
        interop6_a->lpVtbl->Release(interop6_a);
        interop6_b->lpVtbl->Release(interop6_b);
        interop_a->lpVtbl->Release(interop_a);
        interop_b->lpVtbl->Release(interop_b);
        std::printf("gpu_sync_resource_frame_loop=%s frames=%d/%d mode=persistent\n",
                    passed ? "pass" : "fail", passed ? frame_count : 0, frame_count);
        return passed ? 0 : 13;
    }

    int completed = 0;
    bool all_ok = true;
    for (int frame = 0; frame < frame_count && all_ok; ++frame) {
        void *mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, &mapped)) || !mapped) {
            std::fprintf(stderr, "frame=%d upload_map=fail\n", frame);
            all_ok = false; break;
        }
        auto *bytes_ptr = static_cast<unsigned char *>(mapped);
        const unsigned expected = static_cast<unsigned>((0x31 + frame) & 0xff);
        for (UINT64 index = 0; index < bytes; ++index)
            bytes_ptr[index] = static_cast<unsigned char>((index + expected) & 0xffU);
        bytes_ptr[0] = static_cast<unsigned char>(expected);
        upload->Unmap(0, nullptr);
        if (FAILED(allocator_a->Reset()) || FAILED(list_a->Reset(allocator_a.Get(), nullptr))) {
            std::fprintf(stderr, "frame=%d source_command_reset=fail\n", frame);
            all_ok = false; break;
        }
        list_a->CopyBufferRegion(source.Get(), 0, upload.Get(), 0, bytes);
        if (FAILED(list_a->Close())) {
            std::fprintf(stderr, "frame=%d source_command_close=fail\n", frame);
            all_ok = false; break;
        }
        ID3D12CommandList *source_lists[] = {list_a.Get()};
        queue_a->ExecuteCommandLists(1, source_lists);

        ComPtr<ID3D12Fence> fence_a, fence_b;
        if (FAILED(device_a->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_a))) ||
            FAILED(device_b->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_b)))) {
            std::fprintf(stderr, "frame=%d fence_create=fail\n", frame);
            all_ok = false; break;
        }
        INT wait_fd = -1, signal_fd = -1;
        if (FAILED(interop_a->lpVtbl->ExportVulkanFenceFd(
                interop_a, fence_a.Get(), 1U, &wait_fd)) ||
            FAILED(interop_b->lpVtbl->ExportVulkanFenceFd(
                interop_b, fence_b.Get(), 1U, &signal_fd))) {
            std::fprintf(stderr, "frame=%d fence_export=fail wait_fd=%d signal_fd=%d\n",
                         frame, wait_fd, signal_fd);
            all_ok = false; break;
        }
        const std::string frame_status = status_path + "." + std::to_string(frame);
        const std::string frame_gate = frame_status + ".gate";
        std::remove(frame_status.c_str());
        std::remove(frame_gate.c_str());
        if (!spawn_fenced_helper(helper, 0, 1, wait_fd, signal_fd, 1,
                frame_status.c_str(), frame_gate.c_str(), source_fd, source_size,
                source_offset, destination_fd, destination_size, destination_offset,
                std::min(source_size, destination_size), expected) ||
            !wait_status(frame_status.c_str(), "ready", 5000)) {
            std::ifstream status(frame_status);
            std::string contents((std::istreambuf_iterator<char>(status)),
                                 std::istreambuf_iterator<char>());
            std::fprintf(stderr, "frame=%d helper_ready=fail status=%s\n",
                         frame, contents.c_str());
            all_ok = false; break;
        }
        if (FAILED(queue_a->Signal(fence_a.Get(), 1))) {
            std::fprintf(stderr, "frame=%d queue_a_signal=fail\n", frame);
            all_ok = false; break;
        }
        std::ofstream gate(frame_gate, std::ios::app);
        gate << "go\n";
        gate.flush();
        if (!wait_status(frame_status.c_str(), "done rc=0", 10000)) {
            std::ifstream status(frame_status);
            std::string contents((std::istreambuf_iterator<char>(status)),
                                 std::istreambuf_iterator<char>());
            std::fprintf(stderr, "frame=%d helper_done=fail status=%s\n",
                         frame, contents.c_str());
            all_ok = false; break;
        }
        if (FAILED(queue_b->Wait(fence_b.Get(), 1))) {
            std::fprintf(stderr, "frame=%d queue_b_wait=fail\n", frame);
            all_ok = false; break;
        }
        if (FAILED(allocator_b->Reset()) || FAILED(list_b->Reset(allocator_b.Get(), nullptr))) {
            std::fprintf(stderr, "frame=%d destination_command_reset=fail\n", frame);
            all_ok = false; break;
        }
        list_b->CopyBufferRegion(readback.Get(), 0, destination.Get(), 0, bytes);
        if (!wait_queue(device_b.Get(), queue_b.Get(), list_b.Get())) {
            std::fprintf(stderr, "frame=%d destination_queue_wait=fail\n", frame);
            all_ok = false; break;
        }
        void *readback_ptr = nullptr;
        if (FAILED(readback->Map(0, nullptr, &readback_ptr)) || !readback_ptr ||
            static_cast<unsigned char *>(readback_ptr)[0] != expected) {
            std::fprintf(stderr, "frame=%d readback_validate=fail\n", frame);
            all_ok = false;
        }
        readback->Unmap(0, nullptr);
        if (all_ok) ++completed;
    }
    interop6_a->lpVtbl->Release(interop6_a);
    interop6_b->lpVtbl->Release(interop6_b);
    interop_a->lpVtbl->Release(interop_a);
    interop_b->lpVtbl->Release(interop_b);
    std::printf("gpu_sync_resource_frame_loop=%s frames=%d/%d\n",
                all_ok && completed == frame_count ? "pass" : "fail",
                completed, frame_count);
    return all_ok && completed == frame_count ? 0 : 13;
}
