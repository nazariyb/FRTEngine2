#include "Common.hlsl"

// ── Sky parameters ────────────────────────────────────────────────────────────
// Tune SkyIntensity to control how much the environment illuminates the scene:
//   ~1.0  bright midday sky   (local lights barely visible)
//   ~0.1  overcast dusk       (local lights start to show)
//   ~0.02 late evening        (local lights are the dominant light source)
//   ~0.0  pure night / studio (scene lit entirely by explicit lights)
static const float  SkyIntensity  = 0.08f;

// Zenith (top) and horizon colours for a late-evening sky.
// ramp = 0 at screen top (ray going up → zenith), 1 at bottom (horizon/ground).
static const float3 SkyZenith   = float3(0.04f,  0.05f,  0.18f);   // deep blue-indigo
static const float3 SkyHorizon  = float3(0.18f,  0.09f,  0.05f);   // faint amber-red glow

[shader("miss")]
void Miss (inout HitInfo payload : SV_RayPayload)
{
	float2 dims = float2(DispatchRaysDimensions().xy);
	float  ramp = DispatchRaysIndex().y / dims.y;   // 0 = zenith, 1 = horizon

	payload.color = lerp(SkyZenith, SkyHorizon, ramp) * SkyIntensity;
}
