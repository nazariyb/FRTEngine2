#pragma once

#include <span>
#include <string>
#include <vector>

#include "CoreTypes.h"


namespace frt::graphics
{
struct SDisplayOptions
{
public:
	uint8 OutputsNum = 0;
	std::vector<uint8> ResolutionOptionNums;
	std::vector<uint8> RefreshRateOptionNums;
	std::vector<std::wstring> OutputsNames;
	std::vector<struct SRect> OutputsRects;

	std::vector<uint32> Resolutions;
	std::vector<uint64> RefreshRates;

	static constexpr int32 DefaultIndex = 0;

public:
	[[nodiscard]] std::vector<std::string> GetNames () const;
	[[nodiscard]] uint64 GetFullscreenResolutionEncoded (uint8 OutputIndex) const;

	[[nodiscard]] std::span<const uint32> GetResolutions(uint8 OutputIndex) const;
	[[nodiscard]] std::span<const uint64> GetRefreshRates(uint8 OutputIndex, int32 ResolutionIndex) const;
	[[nodiscard]] int32 GetResolutionIndex	(uint8 OutputIndex, const struct SResolution& Resolution) const;
	[[nodiscard]] int32 GetResolutionIndex	(uint8 OutputIndex, uint16 Width, uint16 Height) const;
	[[nodiscard]] int32 GetResolutionIndex	(uint8 OutputIndex, uint32 ResolutionEncoded) const;
	[[nodiscard]] int32 GetRefreshRateIndex	(uint8 OutputIndex, const std::string_view& RefreshRateTxt) const;
	[[nodiscard]] int32 GetRefreshRateIndex	(uint8 OutputIndex, uint32 RefreshRate) const;
	[[nodiscard]] int32 GetRefreshRateIndex	(uint8 OutputIndex, uint32 Numerator, uint32 Denominator) const;

	[[nodiscard]] uint8 ClampMonitorIndex (uint8 InMonitorIndex) const;
};


struct SRect
{
	int64 Left;
	int64 Top;
	int64 Right;
	int64 Bottom;
};

struct SResolution
{
	static constexpr uint16 InvalidComponent = (uint16)-1;

	uint16 Width	= InvalidComponent;
	uint16 Height	= InvalidComponent;
	bool bValid		= false;

	// Accepts "<width><delimiter><height>" where delimiter is one of x X : | -, and rejects
	// anything else - including partially numeric text. Width/Height are left at
	// InvalidComponent and bValid false on failure, so a rejected string never leaves a
	// half-written resolution behind.
	bool Parse (std::string_view Str);

	// Round-trips through Parse. Written into config, so the delimiter has to be one Parse
	// accepts, and the result has to fit SDisplaySettings::ResolutionTextBufferSize.
	std::string ToString () const;

	uint32 GetEncoded () const;

	static SResolution FromEncoded (uint32 Encoded);
};
}
