#include "Common.hlsl"

// ── Sky model ────────────────────────────────────────────────────────────────
// Procedural three-stop hemisphere blend driven by the SkyConstantBuffer:
//   - GroundColor      below horizon (dir.y < 0)
//   - SkyHorizonColor  at horizon
//   - SkyZenithColor   at zenith
//
// The sun is NOT shaded into the miss radiance — direct sun illumination is
// added via NEE on a Directional light entry. This keeps MIS clean (no
// double-count) and lets shadow rays stop at occluders without the sun's
// brightness leaking around them.

[shader("miss")]
void Miss (inout HitInfo payload : SV_RayPayload)
{
	const float3 dir = normalize(WorldRayDirection());

	// Smoothstep across the horizon for a soft transition between sky and ground.
	const float softness = max(gSkyHorizonSoftness, 1e-4f);
	const float skyT = smoothstep(-softness, softness, dir.y);    // 0 deep ground, 1 above horizon

	// Blend horizon -> zenith for the sky half.
	const float zenithT = saturate(dir.y);                          // 0 at horizon, 1 at zenith
	const float3 skyColor = lerp(gSkyHorizonColor.rgb, gSkyZenithColor.rgb, zenithT);

	// Smoothstep band between ground and sky.
	const float3 above = skyColor * gSkyIntensity;
	const float3 below = gGroundColor.rgb;
	payload.color = lerp(below, above, skyT);
}
