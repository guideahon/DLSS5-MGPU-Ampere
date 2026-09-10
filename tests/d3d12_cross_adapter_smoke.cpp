#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <stdio.h>

static void log_hr(const char* label, HRESULT hr) {
    fprintf(stderr, "%s: 0x%08lx\n", label, (unsigned long)hr);
}

int main() {
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    log_hr("CreateDXGIFactory1", hr);
    if (FAILED(hr)) return 2;

    IDXGIAdapter1* adapter_a = nullptr;
    IDXGIAdapter1* adapter_b = nullptr;
    UINT index = 0;
    while (factory->EnumAdapters1(index, &adapter_a) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter_a->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            wcsstr(desc.Description, L"RTX 3090") != nullptr)
            break;
        adapter_a->Release();
        adapter_a = nullptr;
        ++index;
    }
    if (!adapter_a) return 3;
    UINT second = index + 1;
    while (factory->EnumAdapters1(second, &adapter_b) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter_b->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            wcsstr(desc.Description, L"RTX 3090") != nullptr)
            break;
        adapter_b->Release();
        adapter_b = nullptr;
        ++second;
    }
    if (!adapter_b) return 4;

    ID3D12Device* device_a = nullptr;
    ID3D12Device* device_b = nullptr;
    hr = D3D12CreateDevice(adapter_a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_a));
    log_hr("D3D12CreateDevice A", hr);
    if (FAILED(hr)) return 5;
    hr = D3D12CreateDevice(adapter_b, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_b));
    log_hr("D3D12CreateDevice B", hr);
    if (FAILED(hr)) return 6;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 4096;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

    D3D12_HEAP_DESC shared_heap_desc{};
    shared_heap_desc.SizeInBytes = 65536;
    shared_heap_desc.Properties = heap_props;
    shared_heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    shared_heap_desc.Flags = D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    ID3D12Heap* heap_a = nullptr;
    hr = device_a->CreateHeap(&shared_heap_desc, IID_PPV_ARGS(&heap_a));
    log_hr("CreateHeap SHARED_CROSS_ADAPTER", hr);
    if (FAILED(hr)) return 7;

    ID3D12Resource* resource_a = nullptr;
    hr = device_a->CreatePlacedResource(
        heap_a, 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&resource_a));
    log_hr("CreatePlacedResource on GPU A", hr);
    if (FAILED(hr)) return 7;

    ID3D12Device1* device_a1 = nullptr;
    hr = device_a->QueryInterface(IID_PPV_ARGS(&device_a1));
    log_hr("QueryInterface ID3D12Device1", hr);
    if (FAILED(hr)) return 8;

    HANDLE shared_handle = nullptr;
    hr = device_a1->CreateSharedHandle(heap_a, nullptr,
                                        GENERIC_ALL, nullptr, &shared_handle);
    log_hr("CreateSharedHandle(heap)", hr);
    bool shared_resource = false;
    if (FAILED(hr)) {
        hr = device_a1->CreateSharedHandle(resource_a, nullptr,
                                            GENERIC_ALL, nullptr, &shared_handle);
        log_hr("CreateSharedHandle(resource fallback)", hr);
        shared_resource = SUCCEEDED(hr);
    }
    if (FAILED(hr)) return 9;

    ID3D12Heap* heap_b = nullptr;
    ID3D12Resource* resource_b = nullptr;
    if (shared_resource) {
        hr = device_b->OpenSharedHandle(shared_handle, IID_PPV_ARGS(&resource_b));
        log_hr("OpenSharedHandle(resource) on GPU B", hr);
    } else {
        hr = device_b->OpenSharedHandle(shared_handle, IID_PPV_ARGS(&heap_b));
        log_hr("OpenSharedHandle(heap) on GPU B", hr);
        if (SUCCEEDED(hr))
            hr = device_b->CreatePlacedResource(
                heap_b, 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&resource_b));
        log_hr("CreatePlacedResource on GPU B", hr);
    }
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC imported_desc = resource_b->GetDesc();
        fprintf(stderr, "Imported resource: dimension=%u width=%llu layout=%u flags=0x%x\n",
                (unsigned int)imported_desc.Dimension,
                (unsigned long long)imported_desc.Width,
                (unsigned int)imported_desc.Layout,
                (unsigned int)imported_desc.Flags);
    }

    if (shared_handle) CloseHandle(shared_handle);
    if (resource_b) resource_b->Release();
    if (heap_b) heap_b->Release();
    device_a1->Release();
    resource_a->Release();
    heap_a->Release();
    device_b->Release();
    device_a->Release();
    adapter_b->Release();
    adapter_a->Release();
    factory->Release();
    return SUCCEEDED(hr) ? 0 : 10;
}
