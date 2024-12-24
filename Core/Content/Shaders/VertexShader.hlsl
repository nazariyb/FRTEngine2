#include "CoreTypes.hlsli"

PSInput main(VSInput input)
{
	PSInput result;

	input.position.yz = input.position.zy;
	result.position = mul(float4(input.position, 1.f), model);
	// result.position = float4(input.position, 1.f);
	result.uv = input.uv;

	return result;
}
