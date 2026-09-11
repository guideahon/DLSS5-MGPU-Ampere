#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using create_external_fd_fn = HRESULT (STDMETHODCALLTYPE *)(void *, const void *, INT,
        ID3D12Resource **);
using release_fn = ULONG (STDMETHODCALLTYPE *)(void *);
struct device_ext6_vtbl {
    void *query_interface;
    void *add_ref;
    release_fn release;
    void *slots[18];
    create_external_fd_fn CreateResourceFromExternalFd;
};
struct device_ext6 {
    const device_ext6_vtbl *lpVtbl;
};

static const GUID IID_ID3D12DeviceExt6 =
    {0x0f6c3c31, 0x0d8b, 0x4e9a, {0x9a, 0x65, 0x41, 0xce, 0x6d, 0xd4, 0xd1, 0xc2}};

static IDXGIAdapter1 *find_3090(IDXGIFactory4 *factory, unsigned ordinal)
{
    unsigned found = 0;
    for (UINT index = 0; ; ++index)
    {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                wcsstr(desc.Description, L"RTX 3090") != nullptr &&
                found++ == ordinal)
            return adapter;
        adapter->Release();
    }
    return nullptr;
}

static bool parse_u64(const char *text, UINT64 *value)
{
    if (!text || !*text)
        return false;
    char *end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(text, &end, 0);
    if (errno || !end || *end)
        return false;
    *value = parsed;
    return true;
}

int main(int argc, char **argv)
{
    UINT64 fd_value = 0, allocation_size = 0, ordinal_value = 1;
    UINT64 width = 640, height = 360, format_value = DXGI_FORMAT_R16G16B16A16_FLOAT;
    UINT64 offset = 0;
    if (argc < 8 ||
            !parse_u64(argv[1], &fd_value) ||
            !parse_u64(argv[2], &allocation_size) ||
            !parse_u64(argv[3], &ordinal_value) ||
            !parse_u64(argv[4], &width) ||
            !parse_u64(argv[5], &height) ||
            !parse_u64(argv[6], &format_value) ||
            !parse_u64(argv[7], &offset))
    {
        std::fprintf(stderr, "usage: %s FD ALLOCATION_SIZE ORDINAL WIDTH HEIGHT DXGI_FORMAT OFFSET\n",
                argv[0]);
        return 2;
    }

    IDXGIFactory4 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
        return 3;
    IDXGIAdapter1 *adapter = find_3090(factory, static_cast<unsigned>(ordinal_value));
    if (!adapter)
    {
        factory->Release();
        return 4;
    }
    ID3D12Device *device = nullptr;
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&device));
    adapter->Release();
    factory->Release();
    if (FAILED(hr))
    {
        std::fprintf(stderr, "worker D3D12CreateDevice hr=0x%08lx\n",
                static_cast<unsigned long>(hr));
        return 5;
    }

    device_ext6 *ext = nullptr;
    hr = device->QueryInterface(IID_ID3D12DeviceExt6,
            reinterpret_cast<void **>(&ext));
    if (FAILED(hr) || !ext)
    {
        std::fprintf(stderr, "worker QueryInterface ID3D12DeviceExt6 hr=0x%08lx\n",
                static_cast<unsigned long>(hr));
        device->Release();
        return 6;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = static_cast<UINT>(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = static_cast<DXGI_FORMAT>(format_value);
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    struct resource_desc1_compat {
        D3D12_RESOURCE_DESC desc;
        UINT32 sampler_feedback_padding[4];
    } desc1{desc, {0, 0, 0, 0}};

    ID3D12Resource *resource = nullptr;
    hr = ext->lpVtbl->CreateResourceFromExternalFd(ext, &desc1,
            static_cast<INT>(fd_value), &resource);
    std::fprintf(stderr,
            "d3d12_external_fd_worker ordinal=%llu fd=%llu allocation=%llu offset=%llu hr=0x%08lx resource=%p\n",
            static_cast<unsigned long long>(ordinal_value),
            static_cast<unsigned long long>(fd_value),
            static_cast<unsigned long long>(allocation_size),
            static_cast<unsigned long long>(offset),
            static_cast<unsigned long>(hr), resource);
    const bool imported = SUCCEEDED(hr) && resource != nullptr;
    if (resource)
        resource->Release();
    ext->lpVtbl->release(ext);
    device->Release();
    std::printf("{\"worker\":true,\"resource_fd_imported\":%s,\"hr\":\"0x%08lx\"}\n",
            imported ? "true" : "false", static_cast<unsigned long>(hr));
    return imported ? 0 : 7;
}
