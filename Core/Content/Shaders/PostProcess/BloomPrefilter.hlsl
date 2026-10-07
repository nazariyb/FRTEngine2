// Bloom, level 0: HDR colour -> half resolution, exposed, firefly-suppressed.
//
// Jimenez 2014 ("Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH
// Advances): 13-tap downsample = five overlapping 4x4 boxes (centre box weight 0.5, four corner
// boxes 0.125 each), which avoids the pulsing / aliasing of a plain 2x2 box on moving
// highlights. On this first level each of the 13 taps is weighted by the Karis average
// 1 / (1 + luma) and the result renormalised, so a single firefly cannot dominate the pyramid.
//
// Exact formulation: output pixel o covers source texels 2o, 2o+1; every bilinear tap of the
// reference filter sits on a texel corner and is therefore an exact 2x2 box average. The group
// loads its 20x20 source footprint once into LDS with point loads, which lets every texel be
// sanitised (NaN / Inf) and exposed *before* any filtering - a bilinear fetch would smear a
// NaN into the neighbourhood before it could be caught.

#include "Common/Bindings.hlsli"
#include "Common/Color.hlsli"
#include "PostProcess/PostCommon.hlsli"
#include "Shared/PostProcessData.h"

ConstantBuffer<SBloomPassConstants> g_Pass : register(b2);

static const uint kGroupSize = 8u;
static const uint kTileSize = kGroupSize * 2u + 4u;   // 2 texels of border on each side
static const uint kTileTexels = kTileSize * kTileSize;

groupshared float3 gs_Tile[kTileTexels];

float3 SanitizeExposed (float3 C, float Exposure)
{
	C = select(isnan(C), 0.0f, C);
	return clamp(C * Exposure, 0.0f, g_Pass.MaxExposedValue);
}

float3 TileTexel (int2 P)
{
	return gs_Tile[P.y * kTileSize + P.x];
}

float KarisWeight (float3 C)
{
	return 1.0f / (1.0f + Luminance(C));
}

[numthreads(8, 8, 1)]
void main (uint3 GroupId : SV_GroupID, uint3 GroupThreadId : SV_GroupThreadID, uint GroupIndex : SV_GroupIndex)
{
	Texture2D<float4> source = Tex2D(g_Pass.SourceTexture);

	const float exposure = LoadExposure(g_Pass.ExposureBuffer, g_Pass.FallbackExposure) * GetInvPreExposure();

	// Cooperative load of the group footprint: source texels [2 * origin - 2, 2 * origin + 17].
	const int2 tileOrigin = int2(GroupId.xy * (kGroupSize * 2u)) - 2;
	const int2 maxTexel = int2(g_Pass.SourceSize) - 1;
	for (uint index = GroupIndex; index < kTileTexels; index += kGroupSize * kGroupSize)
	{
		const int2 local = int2(index % kTileSize, index / kTileSize);
		const int2 texel = clamp(tileOrigin + local, 0, maxTexel);
		gs_Tile[index] = SanitizeExposed(source.Load(int3(texel, 0)).rgb, exposure);
	}
	GroupMemoryBarrierWithGroupSync();

	const uint2 pixel = GroupId.xy * kGroupSize + GroupThreadId.xy;
	if (any(pixel >= g_Pass.OutputSize))
	{
		return;
	}

	// Tile position of source texel 2o (top-left texel of this output's own 2x2 box). The
	// footprint is the 6x6 texel block [2o - 2, 2o + 3]; fetch it once into registers.
	const int2 c = int2(GroupThreadId.xy) * 2 + 2;
	float3 t[6][6];
	[unroll]
	for (int y = 0; y < 6; ++y)
	{
		[unroll]
		for (int x = 0; x < 6; ++x)
		{
			t[y][x] = TileTexel(c + int2(x - 2, y - 2));
		}
	}

	// Reference taps, as texel-corner offsets q from the centre corner 2o + 1 (Jimenez 2014):
	//   A(-2,-2)  B(0,-2)  C(2,-2)
	//       D(-1,-1)  E(1,-1)
	//   F(-2, 0)  G(0, 0)  H(2, 0)
	//       I(-1, 1)  J(1, 1)
	//   K(-2, 2)  L(0, 2)  M(2, 2)
	// The bilinear tap at corner offset q is the 2x2 box with top-left texel 2o + q, i.e. the
	// register block starting at index q + 2.
#define FRT_BOX(QX, QY) (0.25f * (t[(QY) + 2][(QX) + 2] + t[(QY) + 2][(QX) + 3] + t[(QY) + 3][(QX) + 2] + t[(QY) + 3][(QX) + 3]))
	const float3 tapA = FRT_BOX(-2, -2);
	const float3 tapB = FRT_BOX(0, -2);
	const float3 tapC = FRT_BOX(2, -2);
	const float3 tapD = FRT_BOX(-1, -1);
	const float3 tapE = FRT_BOX(1, -1);
	const float3 tapF = FRT_BOX(-2, 0);
	const float3 tapG = FRT_BOX(0, 0);
	const float3 tapH = FRT_BOX(2, 0);
	const float3 tapI = FRT_BOX(-1, 1);
	const float3 tapJ = FRT_BOX(1, 1);
	const float3 tapK = FRT_BOX(-2, 2);
	const float3 tapL = FRT_BOX(0, 2);
	const float3 tapM = FRT_BOX(2, 2);
#undef FRT_BOX

	// Karis average over the 13 taps: each 2x2 tap is weighted by its share of the reference
	// filter (five 4x4 boxes, centre 0.5, corners 0.125: D/E/I/J and G 0.125, B/F/H/L 0.0625,
	// A/C/K/M 0.03125) times 1 / (1 + luma), then renormalised. Uniform regions keep their
	// energy; a firefly is attenuated by its own brightness against its dark neighbours.
	// Per tap, not per 4x4 box: a texel in the central 2x2 of the footprint lies in all five
	// boxes, so per-box weights were all equal, cancelled in the renormalisation, and every
	// single-pixel firefly of a raw 1 spp frame bloomed into a bright 4-8 px square.
	const float3 taps[13] = { tapA, tapB, tapC, tapD, tapE, tapF, tapG, tapH, tapI, tapJ, tapK, tapL, tapM };
	const float shares[13] = {
		0.03125f, 0.0625f, 0.03125f,
		0.125f, 0.125f,
		0.0625f, 0.125f, 0.0625f,
		0.125f, 0.125f,
		0.03125f, 0.0625f, 0.03125f };
	float3 sum = 0.0f;
	float weightSum = 0.0f;
	[unroll]
	for (uint i = 0u; i < 13u; ++i)
	{
		const float w = shares[i] * KarisWeight(taps[i]);
		sum += taps[i] * w;
		weightSum += w;
	}
	const float3 result = sum / weightSum;

	RwTex2D(g_Pass.OutputTexture)[pixel] = float4(result, 1.0f);
}
