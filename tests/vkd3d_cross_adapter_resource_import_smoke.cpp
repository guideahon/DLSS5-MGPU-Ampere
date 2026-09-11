#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

struct interop_device;
struct interop_device6_vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(interop_device *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(interop_device *);
    ULONG (STDMETHODCALLTYPE *Release)(interop_device *);
    HRESULT (STDMETHODCALLTYPE *GetDXGIAdapter)(interop_device *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *GetInstanceExtensions)(interop_device *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceExtensions)(interop_device *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceFeatures)(interop_device *, const void **);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHandles)(interop_device *, void *, void *, void *);
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
    HRESULT (STDMETHODCALLTYPE *ExportVulkanResourceFd)(interop_device *, ID3D12Resource *, UINT32, INT *, UINT64 *, UINT64 *);
};
struct interop_device { const interop_device6_vtbl *lpVtbl; };

using create_resource_from_fd_fn = HRESULT (STDMETHODCALLTYPE *)(void *, const void *, INT, ID3D12Resource **);
using release_device_ext_fn = ULONG (STDMETHODCALLTYPE *)(void *);
struct device_ext6_vtbl {
    void *QueryInterface;
    void *AddRef;
    release_device_ext_fn Release;
    void *slots[18];
    create_resource_from_fd_fn CreateResourceFromExternalFd;
};
struct device_ext6 { const device_ext6_vtbl *lpVtbl; };

static const GUID IID_ID3D12DXVKInteropDevice6 =
    {0x6a4b7d2e, 0x2c52, 0x4e11, {0x9c, 0x86, 0x2f, 0x0a, 0xf5, 0xf8, 0xb0, 0xc3}};
static const GUID IID_ID3D12DeviceExt6 =
    {0x0f6c3c31, 0x0d8b, 0x4e9a, {0x9a, 0x65, 0x41, 0xce, 0x6d, 0xd4, 0xd1, 0xc2}};

static void log_hr(const char *name, HRESULT hr)
{
    std::printf("%s=0x%08lx\n", name, (unsigned long)hr);
}

static bool wait_fence(ID3D12Device *device, ID3D12CommandQueue *queue, UINT64 value)
{
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return false;
    event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event) { fence->Release(); return false; }
    hr = queue->Signal(fence, value);
    if (SUCCEEDED(hr) && fence->GetCompletedValue() < value) {
        hr = fence->SetEventOnCompletion(value, event);
        if (SUCCEEDED(hr)) hr = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0
                ? S_OK : DXGI_ERROR_WAIT_TIMEOUT;
    }
    CloseHandle(event);
    fence->Release();
    return SUCCEEDED(hr) && value <= 1;
}

static bool record_clear(ID3D12Device *device, ID3D12Resource *resource,
        ID3D12CommandQueue *queue)
{
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = 1;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ID3D12DescriptorHeap *heap = nullptr;
    HRESULT hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) return false;
    device->CreateRenderTargetView(resource, nullptr,
            heap->GetCPUDescriptorHandleForHeapStart());

    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0,
            D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
            IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        const FLOAT clear[4] = {0.125f, 0.25f, 0.5f, 1.0f};
        list->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(), clear, 0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList *lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        ID3D12Fence *fence = nullptr;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        HANDLE event = SUCCEEDED(hr) ? CreateEventA(nullptr, FALSE, FALSE, nullptr) : nullptr;
        if (SUCCEEDED(hr) && event) {
            hr = queue->Signal(fence, 1);
            if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, event);
            if (SUCCEEDED(hr) && WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
                hr = DXGI_ERROR_WAIT_TIMEOUT;
        } else if (SUCCEEDED(hr)) hr = E_FAIL;
        if (event) CloseHandle(event);
        if (fence) fence->Release();
    }
    if (list) list->Release();
    if (allocator) allocator->Release();
    heap->Release();
    return SUCCEEDED(hr);
}

static bool submit_copy_and_readback(ID3D12Device *device, ID3D12Resource *source,
        UINT width, UINT height, UINT row_pitch, UINT64 bytes, UINT64 *checksum,
        UINT64 *nonzero)
{
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *readback = nullptr;
    HRESULT hr = device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE,
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback));
    if (FAILED(hr)) return false;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    if (SUCCEEDED(hr)) hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = source;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = width;
        dst.PlacedFootprint.Footprint.Height = height;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = row_pitch;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList *lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        ID3D12Fence *fence = nullptr;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        HANDLE event = SUCCEEDED(hr) ? CreateEventA(nullptr, FALSE, FALSE, nullptr) : nullptr;
        if (SUCCEEDED(hr) && event) {
            hr = queue->Signal(fence, 1);
            if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, event);
            if (SUCCEEDED(hr) && WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
                hr = DXGI_ERROR_WAIT_TIMEOUT;
        } else if (SUCCEEDED(hr)) hr = E_FAIL;
        if (event) CloseHandle(event);
        if (fence) fence->Release();
    }
    if (SUCCEEDED(hr)) {
        void *mapped = nullptr;
        D3D12_RANGE range{0, (SIZE_T)bytes};
        hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(hr)) {
            const auto *data = static_cast<const uint8_t *>(mapped);
            UINT64 sum = 1469598103934665603ull, count = 0;
            for (UINT64 i = 0; i < bytes; ++i) {
                sum ^= data[i]; sum *= 1099511628211ull;
                if (data[i]) ++count;
            }
            *checksum = sum; *nonzero = count;
            readback->Unmap(0, nullptr);
        }
    }
    if (list) list->Release();
    if (allocator) allocator->Release();
    if (queue) queue->Release();
    readback->Release();
    return SUCCEEDED(hr);
}

int main()
{
    constexpr UINT width = 64, height = 64;
    ID3D12Device *device_a = nullptr, *device_b = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&device_a));
    log_hr("create_device_a", hr);
    if (FAILED(hr)) return 2;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&device_b));
    log_hr("create_device_b", hr);
    if (FAILED(hr)) return 3;

    interop_device *interop_a = nullptr, *interop_b = nullptr;
    hr = device_a->QueryInterface(IID_ID3D12DXVKInteropDevice6,
            reinterpret_cast<void **>(&interop_a));
    log_hr("query_interop_a", hr);
    if (FAILED(hr)) return 4;
    hr = device_b->QueryInterface(IID_ID3D12DXVKInteropDevice6,
            reinterpret_cast<void **>(&interop_b));
    log_hr("query_interop_b", hr);
    if (FAILED(hr)) return 5;
    UINT8 uuid_a[16]{}, uuid_b[16]{};
    UINT32 da=0,ba=0,ia=0,fa=0, db=0,bb=0,ib=0,fb=0;
    interop_a->lpVtbl->GetVulkanPhysicalDeviceIdentity(interop_a, uuid_a, &da, &ba, &ia, &fa);
    interop_b->lpVtbl->GetVulkanPhysicalDeviceIdentity(interop_b, uuid_b, &db, &bb, &ib, &fb);
    const bool distinct = std::memcmp(uuid_a, uuid_b, 16) || da != db || ba != bb || ia != ib || fa != fb;
    std::printf("physical_identity_distinct=%s pci_a=%u:%u:%u.%u pci_b=%u:%u:%u.%u\n",
            distinct ? "yes" : "no", da, ba, ia, fa, db, bb, ib, fb);
    if (!distinct) return 6;

    D3D12_HEAP_PROPERTIES props{}; props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *resource_a = nullptr;
    hr = device_a->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&resource_a));
    log_hr("create_resource_a", hr);
    if (FAILED(hr)) return 7;
    D3D12_COMMAND_QUEUE_DESC queue_desc{}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue_a = nullptr;
    hr = device_a->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_a));
    if (FAILED(hr) || !record_clear(device_a, resource_a, queue_a)) return 8;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT source_footprint{};
    UINT source_rows = 0; UINT64 source_row_size = 0, source_bytes = 0;
    device_a->GetCopyableFootprints(&desc, 0, 1, 0, &source_footprint,
            &source_rows, &source_row_size, &source_bytes);
    UINT64 source_checksum = 0, source_nonzero = 0;
    const bool source_copied = submit_copy_and_readback(device_a, resource_a,
            width, height, source_footprint.Footprint.RowPitch, source_bytes,
            &source_checksum, &source_nonzero);
    std::printf("source_copy_readback=%s checksum=%llu nonzero=%llu\n",
            source_copied ? "pass" : "fail", (unsigned long long)source_checksum,
            (unsigned long long)source_nonzero);

    INT fd = -1; UINT64 offset = 0, size = 0;
    hr = interop_a->lpVtbl->ExportVulkanResourceFd(interop_a, resource_a, 1, &fd, &offset, &size);
    log_hr("export_resource_fd_a", hr);
    std::printf("resource_fd_a=%d offset=%llu size=%llu\n", fd,
            (unsigned long long)offset, (unsigned long long)size);
    if (FAILED(hr) || fd < 0 || offset != 0) return 9;

    struct resource_desc1_compat { D3D12_RESOURCE_DESC desc; UINT32 padding[4]; } desc1{desc, {0,0,0,0}};
    device_ext6 *ext_b = nullptr;
    hr = device_b->QueryInterface(IID_ID3D12DeviceExt6,
            reinterpret_cast<void **>(&ext_b));
    log_hr("query_device_ext6_b", hr);
    if (FAILED(hr)) return 11;
    ID3D12Resource *resource_b = nullptr;
    hr = ext_b->lpVtbl->CreateResourceFromExternalFd(ext_b, &desc1, fd, &resource_b);
    log_hr("import_resource_fd_b", hr);
    std::printf("resource_b=%p\n", resource_b);
    if (FAILED(hr) || !resource_b) { close(fd); return 12; }
    fd = -1;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; UINT64 row_size = 0, bytes = 0;
    device_b->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &bytes);
    UINT64 checksum = 0, nonzero = 0;
    const bool copied = submit_copy_and_readback(device_b, resource_b, width, height,
            footprint.Footprint.RowPitch, bytes, &checksum, &nonzero);
    std::printf("resource_b_copy_readback=%s checksum=%llu nonzero=%llu\n",
            copied ? "pass" : "fail", (unsigned long long)checksum,
            (unsigned long long)nonzero);
    const bool content_visible = copied && nonzero > 0;
    std::printf("cross_adapter_resource_import=%s\n", resource_b ? "pass" : "fail");
    std::printf("cross_adapter_resource_content=%s\n", content_visible ? "pass" : "fail");
    resource_b->Release();
    ext_b->lpVtbl->Release(ext_b);
    interop_a->lpVtbl->Release(interop_a);
    interop_b->lpVtbl->Release(interop_b);
    queue_a->Release(); resource_a->Release();
    device_b->Release(); device_a->Release();
    return resource_b && content_visible ? 0 : 13;
}
