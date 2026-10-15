// Depth-only shadow-map views of the raster preview: the sun (orthographic, fitted to the scene
// bounds) and the cube faces of the emitter sphere lights, all reversed Z. Alpha-masked
// materials run the same alpha test as the main pass so cut-out leaves cast cut-out shadows.

#include "Common/Bindings.hlsli"
#include "Common/Math.hlsli"
#include "Shared/RasterData.h"

ConstantBuffer<SRasterShadowDrawConstants> g_Draw : register(b0);
ConstantBuffer<SRasterConstants> g_Raster : register(b2);

struct SShadowVsInput
{
	float3 Position : POSITION;
	float2 Uv : TEXCOORD;
};

struct SShadowVsOutput
{
	float4 Position : SV_POSITION;
	float2 Uv : TEXCOORD;
};

SShadowVsOutput VsShadow (SShadowVsInput In)
{
	const SInstanceData instance = LoadInstance(g_Draw.InstanceIndex);
	SShadowVsOutput o;
	o.Position = mul(g_Raster.Views[g_Draw.ViewIndex].ViewProj, float4(TransformPoint(instance.ObjectToWorld, In.Position), 1.0f));
	o.Uv = In.Uv;
	return o;
}

// Opaque casters: depth only (an empty pixel shader keeps early Z).
void PsShadowOpaque ()
{
}

void PsShadowMasked (SShadowVsOutput In)
{
	const SGeometryData geometry = LoadGeometry(g_Draw.GeometryIndex);
	const SMaterialData m = LoadMaterial(geometry.MaterialIndex);
	float alpha = m.BaseColorFactor.a;
	if (m.BaseColorTexture != FRT_INVALID_INDEX)
	{
		alpha *= Tex2D(m.BaseColorTexture).Sample(s_AnisoWrap, In.Uv).a;
	}
	if (alpha < m.AlphaCutoff)
	{
		discard;
	}
}
