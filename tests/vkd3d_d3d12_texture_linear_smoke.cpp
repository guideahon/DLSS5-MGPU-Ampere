#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>

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
    HRESULT (STDMETHODCALLTYPE *GetVulkanHandles)(Vkd3dInteropDevice*, void*, void*, void*);
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
};
struct Vkd3dInteropDevice { const Vkd3dInteropVtbl* lpVtbl; };

static const GUID IID_ID3D12DXVKInteropDevice4 =
    {0xb4eb6e34, 0x0a3a, 0x4a91, {0x9f, 0x21, 0x0f, 0x5a, 0x5c, 0x6f, 0x54, 0xd4}};

static IDXGIAdapter1* find_3090(IDXGIFactory4* factory) {
    const int wanted = std::getenv("MGPU_D3D12_ADAPTER_INDEX")
        ? std::atoi(std::getenv("MGPU_D3D12_ADAPTER_INDEX")) : 0;
    int found = 0;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            wcsstr(desc.Description, L"RTX 3090")) {
            if (found++ == wanted) return adapter;
        }
        adapter->Release();
    }
    return nullptr;
}

static bool spawn_readback_helper(int fd, UINT64 size, const char* helper,
                                  int source, int destination) {
    if (!helper || !*helper) return false;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using Spawn = LONG (WINAPI *)(char* const[], int);
    auto spawn = ntdll ? reinterpret_cast<Spawn>(GetProcAddress(ntdll, "__wine_unix_spawnvp")) : nullptr;
    if (!spawn) return false;
    char fd_text[32], size_text[32], source_text[16], destination_text[16];
    std::snprintf(fd_text, sizeof(fd_text), "%d", fd);
    std::snprintf(size_text, sizeof(size_text), "%llu", static_cast<unsigned long long>(size));
    std::snprintf(source_text, sizeof(source_text), "%d", source);
    std::snprintf(destination_text, sizeof(destination_text), "%d", destination);
    char* argv[] = {const_cast<char*>(helper), fd_text, size_text,
                    source_text, destination_text, nullptr};
    SetEnvironmentVariableA("MGPU_INHERIT_FD", fd_text);
    const LONG result = spawn(argv, 1);
    SetEnvironmentVariableA("MGPU_INHERIT_FD", nullptr);
    std::fprintf(stderr, "d3d12_linear_readback_helper=%s spawn_rc=%ld source=%d destination=%d\n",
                 result == 0 ? "started" : "failed", static_cast<long>(result), source, destination);
    return result == 0;
}

int main() {
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return 2;
    ComPtr<IDXGIAdapter1> adapter = find_3090(factory.Get());
    if (!adapter) {
        std::fprintf(stderr, "no RTX 3090 DXGI adapter\n");
        return 3;
    }
    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                            IID_PPV_ARGS(&device));
    if (FAILED(hr)) return 4;

    constexpr UINT width = 1280;
    constexpr UINT height = 720;
    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture_desc.SampleDesc = {1, 0};
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 total_size = 0;
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint,
                                  &rows, &row_size, &total_size);
    if (!total_size || footprint.Offset != 0) return 5;
    std::fprintf(stderr, "d3d12_linear_texture footprint=%ux%u row_pitch=%u rows=%u total=%llu\n",
                 footprint.Footprint.Width, footprint.Footprint.Height,
                 footprint.Footprint.RowPitch, rows,
                 static_cast<unsigned long long>(total_size));

    D3D12_HEAP_DESC heap_desc{};
    heap_desc.SizeInBytes = total_size;
    heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.Properties.CreationNodeMask = 1;
    heap_desc.Properties.VisibleNodeMask = 1;
    ComPtr<ID3D12Heap> heap;
    hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) return 6;

    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = total_size;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc = {1, 0};
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> linear_buffer;
    hr = device->CreatePlacedResource(heap.Get(), 0, &buffer_desc,
                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&linear_buffer));
    if (FAILED(hr)) return 7;

    D3D12_HEAP_PROPERTIES texture_heap{};
    texture_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    texture_heap.CreationNodeMask = 1;
    texture_heap.VisibleNodeMask = 1;
    D3D12_CLEAR_VALUE optimized_clear{};
    optimized_clear.Format = texture_desc.Format;
    optimized_clear.Color[0] = 0.25f;
    optimized_clear.Color[1] = 0.5f;
    optimized_clear.Color[2] = 0.75f;
    optimized_clear.Color[3] = 1.0f;
    ComPtr<ID3D12Resource> texture;
    hr = device->CreateCommittedResource(&texture_heap, D3D12_HEAP_FLAG_NONE,
                                         &texture_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                         &optimized_clear, IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        std::fprintf(stderr, "CreateCommittedResource(texture) hr=0x%08lx\n",
                     static_cast<unsigned long>(hr));
        return 8;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.NumDescriptors = 1;
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    if (FAILED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)))) return 9;
    device->CreateRenderTargetView(texture.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) return 10;
    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&allocator)))) return 11;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&list)))) return 12;
    const float clear_value[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    list->ClearRenderTargetView(rtv_heap->GetCPUDescriptorHandleForHeapStart(), clear_value, 0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = linear_buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    if (FAILED(list->Close())) return 13;
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 14;
    if (FAILED(queue->Signal(fence.Get(), 1))) return 15;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 16;
    if (fence->GetCompletedValue() < 1 &&
        FAILED(fence->SetEventOnCompletion(1, event))) {
        CloseHandle(event);
        return 17;
    }
    if (fence->GetCompletedValue() < 1) WaitForSingleObject(event, 10000);
    CloseHandle(event);

    Vkd3dInteropDevice* interop = nullptr;
    hr = device->QueryInterface(IID_ID3D12DXVKInteropDevice4,
                                reinterpret_cast<void**>(&interop));
    if (FAILED(hr) || !interop) return 18;
    int fd = -1;
    hr = interop->lpVtbl->ExportVulkanHeapFd(interop, heap.Get(), 1U, &fd);
    std::fprintf(stderr, "d3d12_linear_export hr=0x%08lx fd=%d size=%llu\n",
                 static_cast<unsigned long>(hr), fd,
                 static_cast<unsigned long long>(total_size));
    bool helper_ok = false;
    if (SUCCEEDED(hr) && fd >= 0) {
        const int source_ordinal = std::getenv("MGPU_CUDA_SOURCE_ORDINAL")
            ? std::atoi(std::getenv("MGPU_CUDA_SOURCE_ORDINAL")) : 0;
        const int destination_ordinal = std::getenv("MGPU_CUDA_DESTINATION_ORDINAL")
            ? std::atoi(std::getenv("MGPU_CUDA_DESTINATION_ORDINAL")) : 1;
        helper_ok = spawn_readback_helper(fd, total_size,
                                          std::getenv("MGPU_CUDA_READBACK_HELPER"),
                                          source_ordinal, destination_ordinal);
    }
    interop->lpVtbl->Release(interop);
    std::printf("{\"texture_to_linear_buffer\":true,\"copy_submitted\":true,\""
                "helper_spawned\":%s,\"footprint_row_pitch\":%u,\""
                "linear_bytes\":%llu}\n",
                helper_ok ? "true" : "false", footprint.Footprint.RowPitch,
                static_cast<unsigned long long>(total_size));
    return helper_ok ? 0 : 19;
}
