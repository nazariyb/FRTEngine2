// Radiance = diffuse * diffuseAlbedo + specular * specularAlbedo + emission.
// Inputs are the (denoised or raw) demodulated signals of the path tracer; the albedos are the
// exact factors the path tracer divided by, so the round trip is lossless without denoising.

#include "Common/Bindings.hlsli"
#include "Common/Math.hlsli"

cbuffer RootConstants : register(b0)
{
	uint g_Diffuse;
	uint g_Specular;
	uint g_DiffuseAlbedo;
	uint g_SpecularAlbedo;
	uint g_Emission;
	uint g_Output;
	uint g_Width;
	uint g_Height;
};

[numthreads(8, 8, 1)]
void main (uint3 Id : SV_DispatchThreadID)
{
	if (Id.x >= g_Width || Id.y >= g_Height)
	{
		return;
	}
	const float3 diffuse = Tex2D(g_Diffuse)[Id.xy].rgb;
	const float3 specular = Tex2D(g_Specular)[Id.xy].rgb;
	const float3 dAlb = Tex2D(g_DiffuseAlbedo)[Id.xy].rgb;
	const float3 sAlb = Tex2D(g_SpecularAlbedo)[Id.xy].rgb;
	const float4 emission = Tex2D(g_Emission)[Id.xy];
	// Sky pixels (no primary hit, emission.a == 0) carry only the environment.
	float3 radiance = emission.a > 0.0f ? (diffuse * dAlb + specular * sAlb + emission.rgb) : emission.rgb;
	if (!IsFiniteFloat3(radiance))
	{
		radiance = 0.0f;
	}
	RwTex2D(g_Output)[Id.xy] = float4(min(max(radiance, 0.0f), 60000.0f), 1.0f);
}
