#include "Common.hlsl"

// ─────────────────────────────────────────────────────────────────────────────
// Resource bindings
// ─────────────────────────────────────────────────────────────────────────────

RaytracingAccelerationStructure SceneBVH        : register(t0);
Texture2D<float4>               gMaterialTextures[16] : register(t1);
StructuredBuffer<VSInput>       gVertices        : register(t17);
StructuredBuffer<uint>          gIndices         : register(t18);
SamplerState                    gLinearSampler   : register(s0);

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

static const float kPi                  = 3.14159265359f;
static const float kEpsilon             = 1e-6f;
static const uint  kInvalidTextureSlot  = 0xFFFFFFFF;
static const uint  kMaxBounces          = 4u;
static const uint  kRussianRouletteDepth = 2u;    // start RR termination at this depth

// ─────────────────────────────────────────────────────────────────────────────
// Shadow payload  (kept minimal — just a single bool)
// ─────────────────────────────────────────────────────────────────────────────

struct ShadowHitInfo
{
	bool isHit;
};

// ─────────────────────────────────────────────────────────────────────────────
// Utility
// ─────────────────────────────────────────────────────────────────────────────

// Perceptual luminance weights per ITU-R BT.709
float Luminance(float3 c)
{
	return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// Material texture slots are packed into four uint4 constant-buffer entries.
// This decodes a flat slot index [0..15] back to the actual texture descriptor index.
uint GetMaterialTextureIndex(uint slot)
{
	if (slot < 4)  return gMaterialTextureIndices0[slot];
	if (slot < 8)  return gMaterialTextureIndices1[slot - 4];
	if (slot < 12) return gMaterialTextureIndices2[slot - 8];
	return              gMaterialTextureIndices3[slot - 12];
}

// ─────────────────────────────────────────────────────────────────────────────
// Mip-LOD estimation for ray-traced texture lookups
//
// In rasterisation the GPU derives UV gradients automatically via finite
// differences across quads.  In DXR we must estimate them ourselves:
//   1. Approximate the world-space footprint of one pixel at the hit distance
//      using tan(halfFovY), which is encoded as 1/gProj[1][1].
//   2. Convert to texel space via the world-area / UV-area ratio of the
//      hit triangle.
//   3. Feed the resulting texel footprint into log2 to get the mip level.
// ─────────────────────────────────────────────────────────────────────────────

float ComputeTextureLod(
	float2 uv0, float2 uv1, float2 uv2,
	float3 p0,  float3 p1,  float3 p2,
	uint2  texSize)
{
	if (texSize.x == 0 || texSize.y == 0)
		return 0.0f;

	// Area of the UV-space triangle (× 2, cancels in the ratio below)
	const float2 duv1   = uv1 - uv0;
	const float2 duv2   = uv2 - uv0;
	const float uvArea2 = abs(duv1.x * duv2.y - duv1.y * duv2.x);
	if (uvArea2 <= 1e-8f) return 0.0f;

	// Area of the world-space triangle (× 2)
	const float3 e1        = p1 - p0;
	const float3 e2        = p2 - p0;
	const float worldArea2 = length(cross(e1, e2));
	if (worldArea2 <= 1e-8f) return 0.0f;

	// gProj[1][1] = 1 / tan(halfFovY)  →  tan(halfFovY) = 1 / gProj[1][1]
	const float invProjY = abs(gProj[1][1]);
	if (invProjY <= 1e-8f || gRenderTargetSize.y <= 1.0f) return 0.0f;

	const float tanHalfFovY    = 1.0f / invProjY;
	const float pixelWorldSize = (2.0f * max(RayTCurrent(), 0.0f) * tanHalfFovY) / gRenderTargetSize.y;

	// World units that correspond to one unit of UV
	const float worldUnitsPerUv = sqrt(worldArea2 / uvArea2);
	if (worldUnitsPerUv <= 1e-8f) return 0.0f;

	const float texelFootprint = (pixelWorldSize / worldUnitsPerUv) * max((float)texSize.x, (float)texSize.y);
	return log2(max(texelFootprint, 1e-8f));
}

// ─────────────────────────────────────────────────────────────────────────────
// BRDF functions  (Cook-Torrance specular + Lambertian diffuse)
//
// Naming convention for dot-products:
//   NoV = dot(N, V),  NoL = dot(N, L),  NoH = dot(N, H),  VoH = dot(V, H)
//   N = shading normal,  V = view (outgoing),  L = light (incoming),
//   H = half-vector = normalize(V + L)
// ─────────────────────────────────────────────────────────────────────────────

// Fresnel reflectance — Schlick approximation.
// f0  : specular reflectance at normal incidence (0°), i.e. looking straight at the surface.
//       For dielectrics typically 0.04; for metals = base colour.
// VoH : cosine of the angle between view direction and the half-vector.
float3 F_Schlick(float VoH, float3 f0)
{
	// Avoid pow() — unroll (1-x)^5 explicitly for performance
	float t  = 1.0f - saturate(VoH);
	float t2 = t * t;
	float t5 = t2 * t2 * t;
	return f0 + (1.0f - f0) * t5;
}

// GGX / Trowbridge-Reitz Normal Distribution Function (NDF).
// Measures what fraction of microfacets are oriented toward H.
// alpha = roughness^2  (Disney/Unreal perceptual roughness remapping)
float D_GGX(float NoH, float alpha)
{
	const float a2    = alpha * alpha;
	const float denom = NoH * NoH * (a2 - 1.0f) + 1.0f;
	return a2 / max(kPi * denom * denom, kEpsilon);
}

// Smith G1 masking/shadowing term for a single direction (GGX variant).
// Accounts for microfacets that are self-shadowed or masked from one direction.
float G1_Smith(float NoX, float alpha)
{
	const float a2    = alpha * alpha;
	const float denom = NoX + sqrt(a2 + (1.0f - a2) * NoX * NoX);
	return (2.0f * NoX) / max(denom, kEpsilon);
}

// Combined Smith height-correlated G term for both view and light directions.
float G_Smith(float NoV, float NoL, float alpha)
{
	return G1_Smith(NoV, alpha) * G1_Smith(NoL, alpha);
}

// ─────────────────────────────────────────────────────────────────────────────
// Importance sampling helpers
// ─────────────────────────────────────────────────────────────────────────────

// Sample a direction from a cosine-weighted hemisphere (Malley's method).
// Maps (u1, u2) ∈ [0,1)² to the +Z hemisphere.
// PDF = NoL / π
float3 SampleCosineHemisphere(float u1, float u2)
{
	const float r   = sqrt(u1);            // projected-disk radius = sin(θ)
	const float phi = 2.0f * kPi * u2;
	return float3(r * cos(phi), r * sin(phi), sqrt(max(1.0f - u1, 0.0f)));
}

// Sample a GGX-distributed microfacet normal H in the +Z hemisphere.
// Used for specular bounce importance sampling.
// PDF(H) = D(H) * NoH
float3 SampleGGXHalfVector(float u1, float u2, float alpha)
{
	const float a2       = alpha * alpha;
	const float phi      = 2.0f * kPi * u2;
	// Inverse CDF for the GGX elevation angle
	const float cosTheta = sqrt((1.0f - u1) / max(1.0f + (a2 - 1.0f) * u1, kEpsilon));
	const float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));
	return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// ─────────────────────────────────────────────────────────────────────────────
// Shading frame (TBN = Tangent-Bitangent-Normal matrix)
//
// Transforms vectors from tangent space to world space.
// Row layout: TBN[0] = T,  TBN[1] = B,  TBN[2] = N
// Usage: worldVec = mul(tangentVec, TBN)
// ─────────────────────────────────────────────────────────────────────────────

// Builds the TBN matrix from a world-space shading normal and a (potentially
// non-orthogonal) world-space tangent.  Gram-Schmidt re-orthogonalises the
// tangent with respect to N, because interpolated tangents can drift.
float3x3 BuildTBN(float3 N, float3 T)
{
	T = normalize(T - dot(T, N) * N);          // Gram-Schmidt
	const float3 B = normalize(cross(N, T));
	return float3x3(T, B, N);
}

// ─────────────────────────────────────────────────────────────────────────────
// Shadow ray
// ─────────────────────────────────────────────────────────────────────────────

// Traces a shadow ray from `origin` toward `L` and returns:
//   1.0  — path is unoccluded (lit)
//   0.0  — path is blocked (shadowed)
//
// origin : biased hit point (caller is responsible for the offset)
// L      : unit direction toward the light
// tMax   : distance to the light (stops the ray before hitting the light itself)
float TraceShadowRay(float3 origin, float3 L, float tMax)
{
	ShadowHitInfo shadowPayload;
	shadowPayload.isHit = false;

	RayDesc ray;
	ray.Origin    = origin;
	ray.Direction = L;
	ray.TMin      = 0.001f;
	ray.TMax      = tMax;

	TraceRay(
		SceneBVH,
		RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,   // stop at first occluder, no shading needed
		0xFF,
		1, 2, 1,   // hit-group offset 1 → ShadowClosestHit;  miss index 1 → ShadowMiss
		ray,
		shadowPayload);

	return shadowPayload.isHit ? 0.0f : 1.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// BSDF importance sampling
// ─────────────────────────────────────────────────────────────────────────────

// Output of SampleBSDF — the sampled outgoing direction and the corresponding
// Monte-Carlo estimator weight: (BRDF × NoL) / pdf
struct BSDFSample
{
	float3 wi;       // sampled world-space outgoing direction
	float3 weight;   // (f_diff + f_spec) * NoL / pdf
};

// Importance-samples one BSDF lobe (diffuse or specular) using balance-heuristic MIS.
// V      : view direction (pointing away from the surface, toward the camera)
// N      : shading normal
// TBN    : tangent-to-world matrix (row 2 = N)
// albedo : base colour (already texture-multiplied)
BSDFSample SampleBSDF(
	float3   V,
	float3   N,
	float3x3 TBN,
	float3   albedo,
	float    roughness,
	float    metallic,
	inout uint rngState)
{
	BSDFSample result = (BSDFSample)0;

	const float NoV = saturate(dot(N, V));
	if (NoV <= 1e-5f) return result;

	// alpha = perceptual roughness squared (Disney remapping)
	const float  alpha = max(roughness * roughness, 0.02f);
	// f0 : specular colour at normal incidence
	//      dielectrics ≈ 0.04 (water/plastic); metals tint by albedo
	const float3 f0   = lerp(0.04f.xxx, albedo, metallic);
	// Diffuse reflectance — metals have no diffuse component
	const float3 kd   = (1.0f - metallic) * albedo;

	// ── MIS lobe probability ──────────────────────────────────────────────
	// Weight between specular and diffuse lobes proportionally to their
	// luminance at the current view angle.  Clamp to [0.05, 0.95] so neither
	// lobe is ever completely starved (except for pure metals where pSpec=1).
	const float3 F_view   = F_Schlick(NoV, f0);
	const float  specLum  = Luminance(F_view);
	const float  diffLum  = Luminance(kd);
	float pSpec = specLum / max(specLum + diffLum, kEpsilon);
	pSpec = (metallic > 0.999f) ? 1.0f : clamp(pSpec, 0.05f, 0.95f);

	// ── Sample a direction ────────────────────────────────────────────────
	const float choose = NextFloat01(rngState);
	const float u1     = NextFloat01(rngState);
	const float u2     = NextFloat01(rngState);

	float3 H;
	if (choose < pSpec)
	{
		// Specular: sample GGX half-vector H, then reflect V around it
		H = normalize(mul(SampleGGXHalfVector(u1, u2, alpha), TBN));
		result.wi = normalize(reflect(-V, H));
	}
	else
	{
		// Diffuse: cosine-weighted hemisphere around N
		result.wi = normalize(mul(SampleCosineHemisphere(u1, u2), TBN));
		H = normalize(V + result.wi);
	}

	const float NoL = saturate(dot(N, result.wi));
	if (NoL <= 1e-5f) return result;

	const float NoH = saturate(dot(N, H));
	const float VoH = saturate(dot(V, H));

	// ── Evaluate full BRDF ────────────────────────────────────────────────
	// Cook-Torrance specular:  f_spec = (D * G * F) / (4 * NoV * NoL)
	const float  D     = D_GGX(NoH, alpha);
	const float  G     = G_Smith(NoV, NoL, alpha);
	const float3 F     = F_Schlick(VoH, f0);
	const float3 fSpec = (D * G * F) / max(4.0f * NoV * NoL, kEpsilon);

	// Lambertian diffuse:  f_diff = albedo / π
	const float3 fDiff = kd / kPi;

	const float3 brdf  = fDiff + fSpec;

	// ── Mixed PDF (balance heuristic MIS) ─────────────────────────────────
	// PDF for cosine-hemisphere (diffuse): p_diff = NoL / π
	// PDF for GGX half-vector reflected  : p_spec = D(H) * NoH / (4 * VoH)
	const float pdfDiff = NoL / kPi;
	const float pdfSpec = (D * NoH) / max(4.0f * VoH, kEpsilon);
	const float pdf     = pSpec * pdfSpec + (1.0f - pSpec) * pdfDiff;
	if (pdf <= kEpsilon) return result;

	result.weight = brdf * (NoL / pdf);
	return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Closest-hit shader
// ─────────────────────────────────────────────────────────────────────────────

[shader("closesthit")]
void ClosestHit(inout HitInfo payload, Attributes attrib)
{
	// ── 1. Ray geometry ───────────────────────────────────────────────────

	const float3 rayDir = WorldRayDirection();
	const float3 hitPos = WorldRayOrigin() + RayTCurrent() * rayDir;   // world-space hit point
	const uint   depth  = payload.depth;

	// ── 2. Triangle attribute interpolation ──────────────────────────────

	// Barycentric coordinates (w0, w1, w2) for the hit point inside the triangle
	const float3 bary = float3(1.0f - attrib.bary.x - attrib.bary.y, attrib.bary.x, attrib.bary.y);

	const uint indexBase = PrimitiveIndex() * 3;
	const VSInput v0 = gVertices[gIndices[indexBase + 0]];
	const VSInput v1 = gVertices[gIndices[indexBase + 1]];
	const VSInput v2 = gVertices[gIndices[indexBase + 2]];

	const float2 uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;

	// World-space vertex positions (needed for both LOD estimation and Ng)
	const float3x4 o2w   = ObjectToWorld3x4();
	const float3   p0    = mul(o2w, float4(v0.position, 1.0f));
	const float3   p1    = mul(o2w, float4(v1.position, 1.0f));
	const float3   p2    = mul(o2w, float4(v2.position, 1.0f));

	// ── 3. Normals ────────────────────────────────────────────────────────

	// Ng: flat geometric normal, derived from world-space edge cross-product.
	// Used for ray-origin offsetting to avoid self-intersection.
	float3 Ng = normalize(cross(p1 - p0, p2 - p0));

	// N: interpolated (smooth) shading normal, transformed to world space.
	// Used for all BRDF evaluations.
	float3 N = normalize(v0.normal * bary.x + v1.normal * bary.y + v2.normal * bary.z);
	N = normalize(mul((float3x3)o2w, N));

	// Flip both normals to face the incoming ray, handling double-sided surfaces
	// and backface hits that can occur due to normal mapping.
	// step(0, x) = 1 when x >= 0 → "1 - 2*step" flips when dot >= 0
	Ng *= 1.0f - 2.0f * step(0.0f, dot(Ng, rayDir));
	N  *= 1.0f - 2.0f * step(0.0f, dot(N,  rayDir));

	// If the shading normal dips below the geometric hemisphere (can happen with
	// aggressive normal maps), fall back to Ng to avoid invalid BRDF inputs.
	N = lerp(Ng, N, step(0.0f, dot(N, Ng)));

	// ── 4. Tangent / shading frame (TBN) ─────────────────────────────────

	float3 T = normalize(v0.tangent * bary.x + v1.tangent * bary.y + v2.tangent * bary.z);
	T = normalize(mul((float3x3)o2w, T));
	const float3x3 TBN = BuildTBN(N, T);

	// ── 5. Material: base colour ──────────────────────────────────────────
	// TODO: also sample normal map (slot 1), roughness/metallic (slot 2)

	float3 albedo = gDiffuseAlbedo.xyz;

	const uint baseColorSlot = GetMaterialTextureIndex(0);
	if (baseColorSlot != kInvalidTextureSlot)
	{
		uint2 texSize = uint2(0, 0);
		gMaterialTextures[baseColorSlot].GetDimensions(texSize.x, texSize.y);
		const float lod = ComputeTextureLod(v0.uv, v1.uv, v2.uv, p0, p1, p2, texSize);
		albedo *= gMaterialTextures[baseColorSlot].SampleLevel(gLinearSampler, frac(uv), lod).rgb;
	}

	const float roughness      = saturate(gRoughness);
	const float metallic       = saturate(gMetallic);
	const bool  isPerfectMirror = (roughness <= 1e-4f);

	// ── 6. Emissive early-out ─────────────────────────────────────────────
	// Emissive surfaces are treated as light sources: they terminate the path
	// and return their own emission.  Emissive energy is independent of albedo.
	if (gEmissiveIntensity > 0.0f)
	{
		payload.color    = gEmissive.rgb * gEmissiveIntensity;
		payload.distance = RayTCurrent();
		payload.depth    = depth;
		return;
	}

	// ── 7. Path depth limit ───────────────────────────────────────────────
	// Terminate paths that have bounced too many times.
	// TODO: review item — currently returns black, losing any direct lighting
	//       computed at this vertex (bug #4).  Becomes relevant once NEE runs
	//       at all depths (bug #1 fix).
	if (depth >= kMaxBounces)
	{
		payload.color    = float3(0.0f, 0.0f, 0.0f);   // no env/sky yet (review item)
		payload.distance = RayTCurrent();
		payload.depth    = depth;
		return;
	}

	// ── 8. Russian Roulette path termination ─────────────────────────────
	// Probabilistically kill low-throughput paths starting at kRussianRouletteDepth.
	// Surviving paths are reweighted by 1/p to keep the estimator unbiased.
	// Continuation probability is clamped to ≥ 0.1 to prevent infinite variance.
	float rrWeight = 1.0f;
	if (!isPerfectMirror && depth >= kRussianRouletteDepth)
	{
		float pContinue = saturate(max(albedo.r, max(albedo.g, albedo.b)));
		pContinue = max(pContinue, 0.1f);
		if (NextFloat01(payload.rngState) > pContinue)
		{
			payload.color    = float3(0.0f, 0.0f, 0.0f);
			payload.distance = RayTCurrent();
			payload.depth    = depth;
			return;
		}
		rrWeight = 1.0f / pContinue;
	}

	// ── 9. Direct lighting  (Next-Event Estimation) ───────────────────────
	// Explicitly connect the current path vertex to the light source and trace
	// one shadow ray.  NEE greatly reduces variance for direct illumination
	// compared to relying on the indirect bounce to accidentally hit the light.
	//
	// TODO: review item — currently only runs at depth 0 (bug #1)
	// TODO: review item — currently only diffuse; no specular highlight (bug #2)
	// TODO: review item — hardcoded light position/intensity (item #6)

	const float3 V = normalize(-rayDir);       // view direction: outgoing, toward camera

	float3 directLighting = float3(0.0f, 0.0f, 0.0f);
	{
		const float3 lightPos       = float3(0.0f, 2.0f, -3.0f);
		const float3 lightRadiance  = float3(20.0f, 20.0f, 20.0f);

		const float3 toLight     = lightPos - hitPos;
		const float  lightDist   = length(toLight);
		const float3 L           = lightDist > 1e-5f ? toLight / lightDist : float3(0.0f, 1.0f, 0.0f);
		const float3 kd          = (1.0f - metallic) * albedo;

		// TODO: remove the depth == 0 guard to enable NEE at all path vertices (bug #1)
		const bool shouldSampleDirect =
			(!isPerfectMirror) && (depth == 0u) && (lightDist > 1e-5f) && (Luminance(kd) > 1e-4f);

		if (shouldSampleDirect)
		{
			const float NoL = saturate(dot(N, L));
			if (NoL > 1e-4f)
			{
				// Offset the shadow ray origin along Ng to avoid self-intersection.
				// Sign is chosen so the offset pushes toward the light side of the surface.
				const float  shadowSign   = dot(L, Ng) >= 0.0f ? 1.0f : -1.0f;
				const float3 shadowOrigin = hitPos + Ng * (shadowSign * 0.001f);
				const float  visibility   = TraceShadowRay(shadowOrigin, L, lightDist - 0.001f);

				// Inverse-square attenuation: E = Φ / (4π * d²),  simplified to 1/d²
				const float  attenuation = 1.0f / max(lightDist * lightDist, 1e-4f);

				// TODO: add specular BRDF term here (bug #2) — currently Lambertian only
				directLighting = kd * NoL * lightRadiance * attenuation * visibility;
			}
		}
	}

	// ── 10. Indirect lighting  (BSDF-sampled bounce ray) ─────────────────
	// Sample one direction from the BSDF, trace a new ray into the scene,
	// and weight the returned radiance by the Monte-Carlo estimator weight.

	float3 bounceRadiance = float3(0.0f, 0.0f, 0.0f);

	if (isPerfectMirror)
	{
		// Perfect specular mirror — reflect the incident ray exactly.
		// reflect() of unit vectors produces a unit vector, no normalize needed.
		const float3 wi          = reflect(rayDir, N);
		const float  originSign  = dot(wi, Ng) >= 0.0f ? 1.0f : -1.0f;

		RayDesc bounceRay;
		bounceRay.Origin    = hitPos + Ng * (originSign * 0.001f);
		bounceRay.Direction = wi;
		bounceRay.TMin      = 0.001f;
		bounceRay.TMax      = 100000.0f;

		HitInfo bouncePayload  = (HitInfo)0;
		bouncePayload.depth    = depth + 1u;
		bouncePayload.rngState = payload.rngState;

		// RAY_FLAG_NONE: do not cull back-faces — bounce rays must hit all geometry
		TraceRay(SceneBVH, RAY_FLAG_NONE, 0xFF, 0, 2, 0, bounceRay, bouncePayload);
		payload.rngState = bouncePayload.rngState;

		bounceRadiance = bouncePayload.color * albedo;   // mirror tints by surface colour
	}
	else
	{
		// Rough / dielectric surface: importance-sample the BSDF
		BSDFSample s = SampleBSDF(V, N, TBN, albedo, roughness, metallic, payload.rngState);

		if (Luminance(s.weight) > kEpsilon)
		{
			const float  originSign = dot(s.wi, Ng) >= 0.0f ? 1.0f : -1.0f;

			RayDesc bounceRay;
			bounceRay.Origin    = hitPos + Ng * (originSign * 0.001f);
			bounceRay.Direction = s.wi;
			bounceRay.TMin      = 0.001f;
			bounceRay.TMax      = 100000.0f;

			HitInfo bouncePayload  = (HitInfo)0;
			bouncePayload.depth    = depth + 1u;
			bouncePayload.rngState = payload.rngState;

			// RAY_FLAG_NONE: do not cull back-faces — bounce rays must hit all geometry
			TraceRay(SceneBVH, RAY_FLAG_NONE, 0xFF, 0, 2, 0, bounceRay, bouncePayload);
			payload.rngState = bouncePayload.rngState;

			bounceRadiance = bouncePayload.color * s.weight * rrWeight;
		}
	}

	// ── 11. Combine ───────────────────────────────────────────────────────

	payload.color    = directLighting + bounceRadiance;
	payload.distance = RayTCurrent();
	payload.depth    = depth;
}
