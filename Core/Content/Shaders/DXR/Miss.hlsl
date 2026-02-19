#include "Common.hlsl"

[shader("miss")]
void Miss (inout HitInfo payload : SV_RayPayload)
{
	uint2 launchIndex = DispatchRaysIndex().xy;
	float2 dims = float2(DispatchRaysDimensions().xy);

	float ramp = launchIndex.y / dims.y;
	payload.color = float3(0.25 + 0.6*ramp, 0.25 + 0.6*ramp, 0.8 + 0.2*ramp);
	payload.distance = -1.0f;
}
