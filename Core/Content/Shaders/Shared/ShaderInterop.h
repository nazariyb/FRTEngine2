// Shared C++ / HLSL type vocabulary.
//
// Structs that cross the CPU/GPU boundary are declared once, in Shared/*.h, and compiled by
// both MSVC and DXC. Layout rules:
//  - constant buffers follow HLSL 16-byte packing: group members so that no vector straddles
//    a 16-byte boundary, and pad explicitly (every CB struct has a static_assert on its size);
//  - structured buffers are tightly packed (4-byte alignment), same as C++.
// Matrices: C++ stores DirectXMath row-vector matrices as-is, shaders are compiled with -Zpc,
// so HLSL sees the transpose and uses mul(M, v).

#ifndef FRT_SHADER_INTEROP_H
#define FRT_SHADER_INTEROP_H

#ifdef __cplusplus

#include <DirectXMath.h>
#include <cstdint>

namespace frt::gpu
{
using uint = uint32_t;
using float2 = DirectX::XMFLOAT2;
using float3 = DirectX::XMFLOAT3;
using float4 = DirectX::XMFLOAT4;
using uint2 = DirectX::XMUINT2;
using uint3 = DirectX::XMUINT3;
using uint4 = DirectX::XMUINT4;
using int2 = DirectX::XMINT2;
using float4x4 = DirectX::XMFLOAT4X4;
}

#define FRT_GPU_NAMESPACE_BEGIN namespace frt::gpu {
#define FRT_GPU_NAMESPACE_END }
#define FRT_GPU_STATIC_ASSERT_SIZE(Type, Size) static_assert(sizeof(Type) == (Size), #Type " layout must match HLSL")
#define FRT_GPU_CONST static constexpr

#else // HLSL

#define FRT_GPU_NAMESPACE_BEGIN
#define FRT_GPU_NAMESPACE_END
#define FRT_GPU_STATIC_ASSERT_SIZE(Type, Size)
#define FRT_GPU_CONST static const

#endif

#define FRT_INVALID_INDEX 0xFFFFFFFFu

#endif // FRT_SHADER_INTEROP_H
