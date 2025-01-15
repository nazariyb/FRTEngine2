cbuffer SceneConstantBuffer : register(b0)
{
	float4x4 model;
	// float4x4 projection;
	// float deltaSeconds;
}

struct VSInput
{
	float3 position : POSITION;
	float2 uv : TEXCOORD;
};

struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD;
};

struct PSOutput
{
	float4 color : SV_TARGET;
};
