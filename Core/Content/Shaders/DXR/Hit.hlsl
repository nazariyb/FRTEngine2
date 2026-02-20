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
static const float Pi = 3.14159265359f;

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

float3 FresnelSchlick (float cosTheta, float3 f0)
{
	const float t = pow(1.0f - saturate(cosTheta), 5.0f);
	return f0 + (1.0f.xxx - f0) * t;
}

float D_Ggx (float nDotH, float alpha)
{
	const float a2 = alpha * alpha;
	const float denom = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
	return a2 / max(Pi * denom * denom, 1e-6f);
}

float G1_SmithGgx (float nDotX, float alpha)
{
	const float a2 = alpha * alpha;
	const float denom = nDotX + sqrt(a2 + (1.0f - a2) * nDotX * nDotX);
	return (2.0f * nDotX) / max(denom, 1e-6f);
}

float G_SmithGgx (float nDotV, float nDotL, float alpha)
{
	return G1_SmithGgx(nDotV, alpha) * G1_SmithGgx(nDotL, alpha);
}

float Luminance (float3 c)
{
	return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 SampleCosineHemisphere (float u1, float u2)
{
	const float r = sqrt(u1);
	const float phi = 2.0f * Pi * u2;
	return float3(r * cos(phi), r * sin(phi), sqrt(max(1.0f - u1, 0.0f)));
}

float3 SampleGgxHalfVector (float u1, float u2, float alpha)
{
	const float a2 = alpha * alpha;
	const float phi = 2.0f * Pi * u2;
	const float cosTheta = sqrt((1.0f - u1) / max(1.0f + (a2 - 1.0f) * u1, 1e-6f));
	const float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));
	return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

[shader("closesthit")]
void ClosestHit (inout HitInfo payload, Attributes attrib)
{
	float3 lightPos = float3(0, 2, -3); // TODO: pass actual lights from code
	float3 lightIntensity = float3(20.0f, 20.0f, 20.0f);

	// shadow

	float3 hitDir = WorldRayDirection();
	// Find the world-space hit position
	float3 worldOrigin = WorldRayOrigin() + RayTCurrent() * hitDir;

	const float3 toLight = lightPos - worldOrigin;
	const float lightDistance = length(toLight);
	const float3 lightDir = lightDistance > 1e-5f ? toLight / lightDistance : float3(0.0f, 1.0f, 0.0f);
	const uint currentDepth = payload.depth;
	const uint maxReflectionDepth = 4u;

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
	float3 geometricNormal = normalize(cross(worldPosition1 - worldPosition0, worldPosition2 - worldPosition0));
	float3 normal = normalize(v0.normal * barycentrics.x + v1.normal * barycentrics.y + v2.normal * barycentrics.z);
	normal = normalize(mul((float3x3)ObjectToWorld3x4(), normal));

	const float geomFlip = 1.0f - 2.0f * step(0.0f, dot(geometricNormal, hitDir));
	const float shadeFlip = 1.0f - 2.0f * step(0.0f, dot(normal, hitDir));
	geometricNormal *= geomFlip;
	normal *= shadeFlip;
	normal = lerp(geometricNormal, normal, step(0.0f, dot(normal, geometricNormal)));

	float3 tangent = normalize(v0.tangent * barycentrics.x + v1.tangent * barycentrics.y + v2.tangent * barycentrics.z);
	tangent = normalize(mul((float3x3)ObjectToWorld3x4(), tangent));

	// Gram-Schmidt to keep tangent orthogonal to normal
	tangent = normalize(tangent - dot(tangent, normal) * normal);
	const float3 bitangent = normalize(cross(normal, tangent));
	float3x3 tangentToWorld = float3x3(tangent, bitangent, normal);

	// texture & color
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

	const float roughness = saturate(gRoughness);
	const bool isPerfectMirror = roughness <= 1e-4f;
	const float metallicForDirect = saturate(gMetallic);
	const float3 kdForDirect = (1.0f - metallicForDirect) * hitColor;
	float3 directLighting = float3(0.0f, 0.0f, 0.0f);
	const bool shouldSampleDirect =
		(!isPerfectMirror) && (currentDepth == 0u) && (lightDistance > 1e-5f) && (Luminance(kdForDirect) > 1e-4f);
	if (shouldSampleDirect)
	{
		const float nDotL = saturate(dot(normal, lightDir));
		if (nDotL > 1e-4f)
		{
			ShadowHitInfo shadowPayload;
			shadowPayload.isHit = false;

			RayDesc shadowRay;
			const float shadowBias = 0.001f;
			const float shadowSign = lerp(-1.0f, 1.0f, step(0.0f, dot(lightDir, geometricNormal)));
			shadowRay.Origin = worldOrigin + geometricNormal * (shadowSign * shadowBias);
			shadowRay.Direction = lightDir;
			shadowRay.TMin = shadowBias;
			shadowRay.TMax = max(lightDistance - shadowBias, shadowBias);

			TraceRay(
				SceneBVH,
				RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
				0xFF,
				1,
				2,
				1,
				shadowRay,
				shadowPayload);

			const float visibility = shadowPayload.isHit ? 0.0f : 1.0f;
			const float distanceAttenuation = 1.0f / max(lightDistance * lightDistance, 1e-4f);
			directLighting = kdForDirect * (visibility * nDotL) * lightIntensity * distanceAttenuation;
		}
	}

	if (gEmissiveIntensity > 0.f)
	{
		payload.color = gEmissive.rgb * gEmissiveIntensity * hitColor;
		payload.distance = RayTCurrent();
		payload.depth = currentDepth;
		return;
	}

	if (currentDepth >= maxReflectionDepth)
	{
		payload.color = float3(0.f, 0.f, 0.f);
		payload.distance = RayTCurrent();
		payload.depth = currentDepth;
		return;
	}

	// Russian roulette: probabilistically terminate long paths while keeping
	// expected energy by reweighting survivors.
	const uint rrStartDepth = 2u;
	float rrWeight = 1.0f;
	if (!isPerfectMirror && currentDepth >= rrStartDepth)
	{
		float continueProbability = saturate(max(hitColor.r, max(hitColor.g, hitColor.b)));
		continueProbability = max(continueProbability, 0.1f);
		if (NextFloat01(payload.rngState) > continueProbability)
		{
			payload.color = float3(0.f, 0.f, 0.f);
			payload.distance = RayTCurrent();
			payload.depth = currentDepth;
			return;
		}
		rrWeight = 1.0f / continueProbability;
	}

	float3 bounceDir = 0.0f.xxx;
	float3 sampleWeight = 0.0f.xxx;
	if (isPerfectMirror)
	{
		bounceDir = normalize(reflect(hitDir, normal));
		sampleWeight = hitColor;
	}
	else
	{
		const float3 v = normalize(-hitDir);
		const float nDotV = saturate(dot(normal, v));
		if (nDotV <= 1e-5f)
		{
			payload.color = directLighting;
			payload.distance = RayTCurrent();
			payload.depth = currentDepth;
			return;
		}

		const float alpha = max(roughness * roughness, 0.02f);
		const float metallic = saturate(gMetallic);
		const float3 f0 = lerp(0.04f.xxx, hitColor, metallic);
		const float3 kd = (1.0f - metallic) * hitColor;

		const float3 fView = FresnelSchlick(nDotV, f0);
		const float specStrength = Luminance(fView);
		const float diffStrength = Luminance(kd);
		float pSpec = specStrength / max(specStrength + diffStrength, 1e-6f);
		if (metallic > 0.999f)
		{
			pSpec = 1.0f;
		}
		else
		{
			pSpec = clamp(pSpec, 0.05f, 0.95f);
		}

		const float choose = NextFloat01(payload.rngState);
		const float u1 = NextFloat01(payload.rngState);
		const float u2 = NextFloat01(payload.rngState);
		if (choose < pSpec)
		{
			float3 h = SampleGgxHalfVector(u1, u2, alpha);
			h = normalize(mul(h, tangentToWorld));
			bounceDir = normalize(reflect(-v, h));
		}
		else
		{
			float3 localDir = SampleCosineHemisphere(u1, u2);
			bounceDir = normalize(mul(localDir, tangentToWorld));
		}

		const float nDotL = saturate(dot(normal, bounceDir));
		if (nDotL <= 1e-5f)
		{
			payload.color = directLighting;
			payload.distance = RayTCurrent();
			payload.depth = currentDepth;
			return;
		}

		const float3 h = normalize(v + bounceDir);
		const float nDotH = saturate(dot(normal, h));
		const float vDotH = saturate(dot(v, h));

		const float d = D_Ggx(nDotH, alpha);
		const float g = G_SmithGgx(nDotV, nDotL, alpha);
		const float3 f = FresnelSchlick(vDotH, f0);

		const float3 fSpec = (d * g * f) / max(4.0f * nDotV * nDotL, 1e-6f);
		const float3 fDiff = kd / Pi;
		const float3 bsdf = fDiff + fSpec;

		const float pdfDiff = nDotL / Pi;
		const float pdfSpec = (d * nDotH) / max(4.0f * vDotH, 1e-6f);
		const float pdf = pSpec * pdfSpec + (1.0f - pSpec) * pdfDiff;
		if (pdf <= 1e-6f)
		{
			payload.color = directLighting;
			payload.distance = RayTCurrent();
			payload.depth = currentDepth;
			return;
		}
		sampleWeight = bsdf * (nDotL / pdf);
	}

	RayDesc reflectedRay;
	const float rayOriginBias = 0.001f;
	const float originSign = lerp(-1.0f, 1.0f, step(0.0f, dot(bounceDir, geometricNormal)));
	reflectedRay.Origin = worldOrigin + geometricNormal * (originSign * rayOriginBias);
	reflectedRay.Direction = bounceDir;
	reflectedRay.TMin = 0.001f;
	reflectedRay.TMax = 100000;

	HitInfo reflectedPayload;
	reflectedPayload.color = float3(0.0f, 0.0f, 0.0f);
	reflectedPayload.distance = 0.0f;
	reflectedPayload.depth = currentDepth + 1u;
	reflectedPayload.rngState = payload.rngState;
	TraceRay(
		SceneBVH,
		RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_NONE,
		0xFF,
		0,
		2,
		0,
		reflectedRay,
		reflectedPayload);

	payload.color = directLighting + reflectedPayload.color * sampleWeight * rrWeight;
	payload.distance = RayTCurrent();
	payload.depth = currentDepth;
	payload.rngState = reflectedPayload.rngState;
}
