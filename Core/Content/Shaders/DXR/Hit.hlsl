#include "Common.hlsl"

// Raytracing acceleration structure, accessed as a SRV
RaytracingAccelerationStructure SceneBVH : register(t0);

Texture2D<float4> gMaterialTextures[16] : register(t1);
StructuredBuffer<VSInput> gVertices : register(t17);
StructuredBuffer<uint> gIndices : register(t18);
SamplerState gLinearSampler : register(s0);


struct ShadowHitInfo
{
	bool isHit;
};


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
	// shadow
	float3 lightPos = float3(0, 2, -3);

	// Find the world - space hit position
	float3 worldOrigin = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

	float3 lightDir = normalize(lightPos - worldOrigin);

	// Fire a shadow ray. The direction is hard-coded here, but can be fetched
	// from a constant-buffer
	RayDesc ray;
	ray.Origin = worldOrigin;
	ray.Direction = lightDir;
	ray.TMin = 0.01;
	ray.TMax = 100000;
	bool hit = true;

	// Initialize the ray payload
	ShadowHitInfo shadowPayload;
	shadowPayload.isHit = false;

	// Trace the ray
	TraceRay(
		// Acceleration structure
		SceneBVH,
		// Flags can be used to specify the behavior upon hitting a surface
		RAY_FLAG_NONE,
		// Instance inclusion mask, which can be used to mask out some geometry to
		// this ray by and-ing the mask with a geometry mask. The 0xFF flag then
		// indicates no geometry will be masked
		0xFF,
		// Depending on the type of ray, a given object can have several hit
		// groups attached (ie. what to do when hitting to compute regular
		// shading, and what to do when hitting to compute shadows). Those hit
		// groups are specified sequentially in the SBT, so the value below
		// indicates which offset (on 4 bits) to apply to the hit groups for this
		// ray. In this sample we only have one hit group per object, hence an
		// offset of 0.
		1,
		// The offsets in the SBT can be computed from the object ID, its instance
		// ID, but also simply by the order the objects have been pushed in the
		// acceleration structure. This allows the application to group shaders in
		// the SBT in the same order as they are added in the AS, in which case
		// the value below represents the stride (4 bits representing the number
		// of hit groups) between two consecutive objects.
		2,
		// Index of the miss shader to use in case several consecutive miss
		// shaders are present in the SBT. This allows to change the behavior of
		// the program when no geometry have been hit, for example one to return a
		// sky color for regular rendering, and another returning a full
		// visibility value for shadow rays. This sample has only one miss shader,
		// hence an index 0
		1,
		// Ray information to trace
		ray,
		// Payload associated to the ray, which will be used to communicate
		// between the hit/miss shaders and the raygen
		shadowPayload);

	float factor = shadowPayload.isHit ? 0.3 : 1.0;

	// texture & color
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

	payload.colorAndDistance = float4(hitColor * factor, RayTCurrent());
}
