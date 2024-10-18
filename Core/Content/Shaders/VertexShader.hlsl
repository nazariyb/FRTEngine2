#include "CoreTypes.hlsli"

PSInput main(VSInput input)
{
	PSInput result;

	result.position = float4(input.position, 1.f);
	result.uv = input.uv;

	return result;
}
