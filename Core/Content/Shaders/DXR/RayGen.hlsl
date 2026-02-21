#include "Common.hlsl"

// Raytracing output texture, accessed as a UAV
RWTexture2D<float4> gOutput : register(u0);

// Raytracing acceleration structure, accessed as a SRV
RaytracingAccelerationStructure SceneBVH : register(t0);

[shader("raygeneration")]
void RayGen ()
{
	// Get the location within the dispatched 2D grid of work items
	// (often maps to pixels, so this could represent a pixel coordinate).
	uint2 launchIndex = DispatchRaysIndex().xy;
	float2 dims = float2(DispatchRaysDimensions().xy);

	const uint sampleCount = gRaytracingSampleCount;   // set via PassConstantBuffer; no shader recompile needed to tune
	float3 accumulatedColor = 0.f;
	for (uint i = 0; i < sampleCount; ++i)
	{
		// Initialize the ray payload
		HitInfo payload;
		payload.color = float3(0.0f, 0.0f, 0.0f);
		payload.depth = 0u;
		payload.rngState =
			launchIndex.x * 1973u ^
			launchIndex.y * 9277u ^
			(i + 1u) * 26699u ^
			(gFrameIndex + 1u) * 3181u;

		// Define a ray, consisting of origin, direction, and the min-max distance values
		RayDesc ray;
		ray.Origin = gCameraPos;
		const float2 jitter = float2(NextFloat01(payload.rngState), NextFloat01(payload.rngState)); // in-pixel sample [0,1)
		const float2 samplePos = launchIndex.xy + jitter;
		const float2 d = ((samplePos / dims.xy) * 2.f - 1.f);
		// gProjInv maps clip-space to view-space; w is not necessarily 1, so
		// divide by w before rotating to world space (perspective divide).
		const float4 target = mul(gProjInv, float4(d.x, -d.y, 1.0f, 1.0f));
		ray.Direction = normalize(mul(gViewInv, float4(target.xyz / target.w, 0.0f)).xyz);
		ray.TMin = 0;
		ray.TMax = 100000;

		// Trace the ray
		TraceRay(
			// Parameter name: AccelerationStructure
			// Acceleration structure
			SceneBVH,

			// Parameter name: RayFlags
			// Flags can be used to specify the behavior upon hitting a surface
			RAY_FLAG_NONE,

			// Parameter name: InstanceInclusionMask
			// Instance inclusion mask, which can be used to mask out some geometry to this ray by
			// and-ing the mask with a geometry mask. The 0xFF flag then indicates no geometry will be
			// masked
			0xFF,

			// Parameter name: RayContributionToHitGroupIndex
			// Depending on the type of ray, a given object can have several hit groups attached
			// (ie. what to do when hitting to compute regular shading, and what to do when hitting
			// to compute shadows). Those hit groups are specified sequentially in the SBT, so the value
			// below indicates which offset (on 4 bits) to apply to the hit groups for this ray. In this
			// sample we only have one hit group per object, hence an offset of 0.
			0,

			// Parameter name: MultiplierForGeometryContributionToHitGroupIndex
			// The offsets in the SBT can be computed from the object ID, its instance ID, but also simply
			// by the order the objects have been pushed in the acceleration structure. This allows the
			// application to group shaders in the SBT in the same order as they are added in the AS, in
			// which case the value below represents the stride (4 bits representing the number of hit
			// groups) between two consecutive objects.
			2,

			// Parameter name: MissShaderIndex
			// Index of the miss shader to use in case several consecutive miss shaders are present in the
			// SBT. This allows to change the behavior of the program when no geometry have been hit, for
			// example one to return a sky color for regular rendering, and another returning a full
			// visibility value for shadow rays. This sample has only one miss shader, hence an index 0
			0,

			// Parameter name: Ray
			// Ray information to trace
			ray,

			// Parameter name: Payload
			// Payload associated to the ray, which will be used to communicate between the hit/miss
			// shaders and the raygen
			payload);

		// ── Firefly clamping ─────────────────────────────────────────────────
		// Clamp each sample's luminance to kFireflyClamp before accumulating.
		// Fireflies are high-variance outlier samples (e.g. near-specular bounces
		// that happen to hit a bright emitter).  Clamping is biased but visually
		// essential: unclamped fireflies appear as bright flickering specks that
		// are far more distracting than the small bias this introduces.
		const float kFireflyClamp = 10.0f;
		const float sampleLum = dot(payload.color, float3(0.2126f, 0.7152f, 0.0722f));
		if (sampleLum > kFireflyClamp)
			payload.color *= kFireflyClamp / sampleLum;

		accumulatedColor += payload.color;
	}

	float3 lin = accumulatedColor / sampleCount;
	lin = lin / (1.0f + lin);                             // Reinhard tone map
	float3 srgb = pow(saturate(lin), 1.0f / 2.2f);       // approximate sRGB gamma

	// ── Temporal accumulation ─────────────────────────────────────────────
	// Pure running average: alpha = 1/(N+1), no floor.
	// Without a floor the noise level keeps decreasing as long as the scene
	// is static — there is no cap on convergence.
	// The counter is reset to 0 by the CPU on any scene change (camera move,
	// object move, topology change), so history is always valid and there is
	// no ghosting — just transient noise that re-converges in a few seconds.
	//
	//   * Frame 0 (after any change) -> write fresh, no blend
	//   * Frame N (static)           -> alpha = 1/(N+1), improving every frame
	if (gAccumulationFrameIndex > 0u)
	{
		const float alpha = 1.0f / (float)(gAccumulationFrameIndex + 1u);
		srgb = lerp(gOutput[launchIndex].rgb, srgb, alpha);
	}

	gOutput[launchIndex] = float4(srgb, 1.f);
}
