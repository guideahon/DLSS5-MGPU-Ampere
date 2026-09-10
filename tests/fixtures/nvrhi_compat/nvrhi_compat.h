#pragma once

#ifndef _Inout_
#define _Inout_
#endif

// This forward declaration is present in newer NVRHI snapshots but missing
// from the archived header bundled with the sample.
namespace nvrhi {
namespace d3d12 {
class RootSignature;
}
}

#include <d3d12.h>

#ifndef D3D12_ENCODE_BASIC_FILTER
#define D3D12_ENCODE_BASIC_FILTER(min, mag, mip, reduction) \
    ((D3D12_FILTER)((((min) & D3D12_FILTER_TYPE_MASK) << D3D12_MIN_FILTER_SHIFT) | \
                    (((mag) & D3D12_FILTER_TYPE_MASK) << D3D12_MAG_FILTER_SHIFT) | \
                    (((mip) & D3D12_FILTER_TYPE_MASK) << D3D12_MIP_FILTER_SHIFT) | \
                    (((reduction) & D3D12_FILTER_REDUCTION_TYPE_MASK) << D3D12_FILTER_REDUCTION_TYPE_SHIFT)))
#endif
#ifndef D3D12_ENCODE_ANISOTROPIC_FILTER
#define D3D12_ENCODE_ANISOTROPIC_FILTER(reduction) \
    ((D3D12_FILTER)(D3D12_ANISOTROPIC_FILTERING_BIT | \
                    D3D12_ENCODE_BASIC_FILTER(D3D12_FILTER_TYPE_LINEAR, \
                                              D3D12_FILTER_TYPE_LINEAR, \
                                              D3D12_FILTER_TYPE_LINEAR, reduction)))
#endif
