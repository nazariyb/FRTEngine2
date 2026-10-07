#include "GraphicsCoreTypes.h"

#include <algorithm>
#include <cmath>


namespace frt::graphics
{
// ---- Time-of-day → sky parameters ----

static float Saturate (float V) { return V < 0.0f ? 0.0f : (V > 1.0f ? 1.0f : V); }
static float Lerp (float A, float B, float T) { return A + (B - A) * T; }
static SColor LerpColor (const SColor& A, const SColor& B, float T)
{
	return SColor(Lerp(A.R, B.R, T), Lerp(A.G, B.G, T), Lerp(A.B, B.B, T), Lerp(A.A, B.A, T));
}

void ComputeSkyFromTimeOfDay (float TimeOfDay, SSkyConstants& Out)
{
	const float t = Saturate(TimeOfDay);

	// Sun arc: at t=0.25 sun is at horizon east (rising), t=0.5 zenith, t=0.75 horizon west (setting),
	// t=0/1 below horizon. Elevation = sin(pi * (2t - 0.5)) clamped, azimuth simple linear sweep.
	const float kPi = 3.14159265359f;
	const float arcAngle = kPi * (2.0f * t - 0.5f);   // -pi/2 at t=0, +pi/2 at t=0.5, +3pi/2 at t=1
	const float elevation = std::sin(arcAngle);       // [-1, 1]
	const float azimuth = kPi * (2.0f * t);           // 0..2pi sweep across the day

	const float horizontalLen = std::sqrt(math::Max(0.0f, 1.0f - elevation * elevation));
	// Sun direction points FROM sun TO scene → invert vertical.
	Out.SunDirection = Vector3f(
		horizontalLen * std::cos(azimuth),
		-elevation,
		horizontalLen * std::sin(azimuth));

	// Day-night blend: 0 at night, 1 around midday.
	const float dayness = Saturate(elevation);

	// Sun color: warm orange at horizon, neutral at zenith.
	const SColor warmSun = SColor(1.00f, 0.55f, 0.30f);
	const SColor noonSun = SColor(1.00f, 0.95f, 0.85f);
	const float horizonness = 1.0f - Saturate(elevation * 2.0f);
	Out.SunColor = LerpColor(noonSun, warmSun, horizonness);
	Out.SunIntensity = Lerp(0.5f, 8.0f, dayness);

	// Sky tint: deep night at low elevation, blue-ish at noon, warm at sunrise/sunset.
	const SColor nightZenith = SColor(0.01f, 0.01f, 0.04f);
	const SColor noonZenith  = SColor(0.10f, 0.20f, 0.45f);
	Out.SkyZenithColor = LerpColor(nightZenith, noonZenith, dayness);

	const SColor nightHorizon = SColor(0.02f, 0.02f, 0.05f);
	const SColor duskHorizon  = SColor(0.55f, 0.30f, 0.15f);
	const SColor noonHorizon  = SColor(0.40f, 0.45f, 0.55f);
	const float duskness = horizonness * math::Max(0.0f, math::Min(1.0f, dayness * 4.0f));
	Out.SkyHorizonColor = LerpColor(nightHorizon, LerpColor(noonHorizon, duskHorizon, duskness), dayness);

	Out.GroundColor = SColor(0.04f, 0.04f, 0.04f);
	Out.SkyIntensity = Lerp(0.05f, 1.0f, dayness);
	Out.HorizonSoftness = 0.20f;
}
}
