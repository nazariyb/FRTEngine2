#ifndef FRT_COLOR_HLSLI
#define FRT_COLOR_HLSLI

// Linear Rec.709 luminance.
float Luminance (float3 C) { return dot(C, float3(0.2126f, 0.7152f, 0.0722f)); }

float3 SrgbToLinear (float3 C)
{
	return select(C <= 0.04045f, C / 12.92f, pow(max((C + 0.055f) / 1.055f, 0.0f), 2.4f));
}

float3 LinearToSrgb (float3 C)
{
	C = max(C, 0.0f);
	return select(C <= 0.0031308f, C * 12.92f, 1.055f * pow(C, 1.0f / 2.4f) - 0.055f);
}

float3 RgbToYCoCg (float3 C)
{
	return float3(
		0.25f * C.r + 0.5f * C.g + 0.25f * C.b,
		0.5f * C.r - 0.5f * C.b,
		-0.25f * C.r + 0.5f * C.g - 0.25f * C.b);
}

float3 YCoCgToRgb (float3 C)
{
	return float3(C.x + C.y - C.z, C.x + C.z, C.x - C.y - C.z);
}

#endif
