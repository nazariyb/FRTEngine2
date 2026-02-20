#include "Common.hlsl"

[shader("miss")]
void Miss (inout HitInfo payload : SV_RayPayload)
{
	uint2 launchIndex = DispatchRaysIndex().xy;
	float2 dims = float2(DispatchRaysDimensions().xy);

	float ramp = launchIndex.y / dims.y;
	payload.color = float3(0.2 + 0.6*ramp, 0.2 + 0.6*ramp, 0.75 + 0.2*ramp) * 0.25f;
	payload.color = float3(0.f, 0.f, 0.f);
	payload.distance = -1.0f;
}
