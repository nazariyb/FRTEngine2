#include "Common.hlsl"

// Sky-as-environment miss handler. Returns the sky radiance for the escaping ray
// direction. Sun is NOT shaded into miss — direct sun comes via NEE on a Directional
// light entry, and sky-NEE adds diffuse sky illumination separately (shadow ray that
// escapes — i.e. hits this miss — counts as unblocked sky).

[shader("miss")]
void Miss (inout HitInfo payload : SV_RayPayload)
{
	const float3 dir = normalize(WorldRayDirection());
	payload.color = EvalSkyRadiance(dir);
}
