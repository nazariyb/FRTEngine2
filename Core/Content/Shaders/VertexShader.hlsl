#include "CoreTypes.hlsli"


PSInput main(VSInput input)
{
	PSInput result;

	float4 pos = float4(input.position, 1.f);
	result.position = mul(mul(gViewProj, gWorld), pos);

	result.uv = input.uv;

	return result;
}
