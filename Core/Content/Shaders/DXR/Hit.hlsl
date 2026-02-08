#include "Common.hlsl"

Texture2D<float4> gMaterialTextures[16] : register(t1);
StructuredBuffer<VSInput> gVertices : register(t17);
StructuredBuffer<uint> gIndices : register(t18);
SamplerState gLinearSampler : register(s0);

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

float ComputeTextureLod (
	float2 uv0,
	float2 uv1,
	float2 uv2,
	float3 position0,
	float3 position1,
	float3 position2,
	uint2 textureSize)
{
	if (textureSize.x == 0 || textureSize.y == 0)
	{
		return 0.0f;
	}

	const float2 duv1 = uv1 - uv0;
	const float2 duv2 = uv2 - uv0;
	const float uvArea2 = abs(duv1.x * duv2.y - duv1.y * duv2.x);
	if (uvArea2 <= 1e-8f)
	{
		return 0.0f;
	}

	const float3 edge1 = position1 - position0;
	const float3 edge2 = position2 - position0;
	const float worldArea2 = length(cross(edge1, edge2));
	if (worldArea2 <= 1e-8f)
	{
		return 0.0f;
	}

	const float invProjY = abs(gProj[1][1]);
	if (invProjY <= 1e-8f || gRenderTargetSize.y <= 1.0f)
	{
		return 0.0f;
	}

	const float tanHalfFovY = 1.0f / invProjY;
	const float pixelWorldSize = (2.0f * max(RayTCurrent(), 0.0f) * tanHalfFovY) / gRenderTargetSize.y;

	const float worldUnitsPerUv = sqrt(worldArea2 / uvArea2);
	if (worldUnitsPerUv <= 1e-8f)
	{
		return 0.0f;
	}

	const float uvFootprint = pixelWorldSize / worldUnitsPerUv;
	const float texelFootprint = uvFootprint * max((float)textureSize.x, (float)textureSize.y);
	return log2(max(texelFootprint, 1e-8f));
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
	const float3 worldPosition0 = mul(ObjectToWorld3x4(), float4(v0.position, 1.0f));
	const float3 worldPosition1 = mul(ObjectToWorld3x4(), float4(v1.position, 1.0f));
	const float3 worldPosition2 = mul(ObjectToWorld3x4(), float4(v2.position, 1.0f));

	float3 hitColor = gDiffuseAlbedo.xyz;

	const uint baseColorTextureIndex = GetMaterialTextureIndex(0);
	if (baseColorTextureIndex != MaterialTextureIndexInvalid)
	{
		const float2 wrappedUv = frac(uv);
		uint2 textureSize = uint2(0, 0);
		gMaterialTextures[baseColorTextureIndex].GetDimensions(textureSize.x, textureSize.y);
		const float lod = ComputeTextureLod(
			v0.uv, v1.uv, v2.uv,
			worldPosition0, worldPosition1, worldPosition2,
			textureSize);
		const float3 sampledColor = gMaterialTextures[baseColorTextureIndex]
			.SampleLevel(gLinearSampler, wrappedUv, lod)
			.rgb;
		hitColor *= sampledColor;
	}

	payload.colorAndDistance = float4(hitColor, RayTCurrent());
}
