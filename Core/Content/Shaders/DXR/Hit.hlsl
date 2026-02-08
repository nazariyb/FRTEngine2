#include "Common.hlsl"

Texture2D<float4> gMaterialTextures[16] : register(t1);
StructuredBuffer<VSInput> gVertices : register(t17);
StructuredBuffer<uint> gIndices : register(t18);

static const uint MaterialTextureIndexInvalid = 0xFFFFFFFF;

uint GetMaterialTextureIndex (uint index)
{
	if (index < 4)
	{
		return gMaterialTextureIndices0[index];
	}
	if (index < 8)
	{
		return gMaterialTextureIndices1[index - 4];
	}
	if (index < 12)
	{
		return gMaterialTextureIndices2[index - 8];
	}
	return gMaterialTextureIndices3[index - 12];
}

[shader("closesthit")]
void ClosestHit (inout HitInfo payload, Attributes attrib)
{
	float3 barycentrics =
	  float3(1.f - attrib.bary.x - attrib.bary.y, attrib.bary.x, attrib.bary.y);

	const uint primitiveIndex = PrimitiveIndex();
	const uint indexBase = primitiveIndex * 3;
	const uint i0 = gIndices[indexBase + 0];
	const uint i1 = gIndices[indexBase + 1];
	const uint i2 = gIndices[indexBase + 2];

	const VSInput v0 = gVertices[i0];
	const VSInput v1 = gVertices[i1];
	const VSInput v2 = gVertices[i2];
	const float2 uv = v0.uv * barycentrics.x + v1.uv * barycentrics.y + v2.uv * barycentrics.z;

	float3 hitColor = gDiffuseAlbedo.xyz;

	const uint baseColorTextureIndex = GetMaterialTextureIndex(0);
	if (baseColorTextureIndex != MaterialTextureIndexInvalid)
	{
		uint2 textureSize = uint2(0, 0);
		gMaterialTextures[baseColorTextureIndex].GetDimensions(textureSize.x, textureSize.y);
		if (textureSize.x > 0 && textureSize.y > 0)
		{
			const float2 wrappedUv = frac(uv);
			const uint2 maxTexel = textureSize - 1;
			const uint2 texel = min(uint2(wrappedUv * float2(textureSize)), maxTexel);
			const float3 sampledColor = gMaterialTextures[baseColorTextureIndex].Load(int3(texel, 0)).rgb;
			hitColor *= sampledColor;
		}
	}

	payload.colorAndDistance = float4(hitColor, RayTCurrent());
}
