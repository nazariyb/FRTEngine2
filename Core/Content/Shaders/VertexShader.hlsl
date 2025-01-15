#include "CoreTypes.hlsli"

PSInput main(VSInput input)
{
	PSInput result;

	// input.position.yz = input.position.zy;

	result.position = mul(model, float4(input.position, 1.f));

	// result.position = float4(input.position, 1.f);
	result.uv = input.uv;

	return result;
}
