#pragma once

#include <span>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Containers/String.h"
// #include "Math/Vector2.h"


namespace frt::graphics
{
struct FRT_CORE_API SDisplayOptions
{
public:
	std::vector<uint8> ResolutionOptionNums;
	std::vector<uint8> RefreshRateOptionNums;
	std::vector<FString> OutputsNames;
	std::vector<struct SRect> OutputsRects;

	std::vector<struct SResolution> Resolutions;
	std::vector<uint64> RefreshRates;

	uint8 OutputsNum = 0;
	uint8 Padding[7];

	static constexpr int32 DefaultIndex = 0;

public:
	[[nodiscard]] const std::vector<FString>& GetNames () const { return OutputsNames; }
	[[nodiscard]] SResolution GetFullscreenResolution (uint8 OutputIndex) const;

	[[nodiscard]] std::span<const SResolution> GetResolutions(uint8 OutputIndex) const;
	[[nodiscard]] std::span<const uint64> GetRefreshRates(uint8 OutputIndex, int32 ResolutionIndex) const;
	[[nodiscard]] int32 GetResolutionIndex	(uint8 OutputIndex, const struct SResolution& Resolution) const;
	[[nodiscard]] int32 GetResolutionIndex	(uint8 OutputIndex, uint16 Width, uint16 Height) const;
	[[nodiscard]] int32 GetRefreshRateIndex	(uint8 OutputIndex, FStringView RefreshRateTxt) const;
	[[nodiscard]] int32 GetRefreshRateIndex	(uint8 OutputIndex, uint32 RefreshRate) const;
	[[nodiscard]] int32 GetRefreshRateIndex	(uint8 OutputIndex, uint32 Numerator, uint32 Denominator) const;

	[[nodiscard]] uint8 ClampMonitorIndex (int32 InMonitorIndex) const;
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
	// InvalidComponent truncated to the 15-bit Height field - what an invalid Height reads back as.
	static constexpr uint16 InvalidHeight = (InvalidComponent & ~(1 << 15));

private:
	uint16 Width	= InvalidComponent;
	uint16 Height	: 15 = InvalidHeight;
	uint16 bValid	: 1 = false;

public:
	SResolution () = default;

	template <concepts::Numerical N = uint16>
	SResolution (N InWidth, N InHeight)
		: Width(static_cast<uint16>(InWidth)), Height(static_cast<uint16>(InHeight)), bValid(true)
	{
		// A size that does not survive the narrowing (or the 15-bit Height field) would
		// silently wrap.
		frt_assert(static_cast<N>(Width) == InWidth && static_cast<N>(Height) == InHeight);
	}

public:
	// TODO: use vec, but figure out what's wrong with its include
	// template <concepts::Numerical N = uint16>
	// [[nodiscard]] math::TVector2<N> Get() const { return { (N)Width, (N)Height }; }
	[[nodiscard]] struct { uint16 W; uint16 H; } Get() const { return { Width, Height }; }
	[[nodiscard]] uint16 GetWidth () const { return Width; }
	[[nodiscard]] uint16 GetHeight () const { return Height; }
	[[nodiscard]] uint16 IsValid () const { return bValid; }

	void Set (uint16 InWidth, uint16 InHeight)
	{
		Width = InWidth;
		Height = (InHeight & ~(1 << 15));
		bValid = true;
	}

	void Invalidate () { bValid = false; }

public:
	// Accepts "<width><delimiter><height>" where delimiter is one of x X : | -, and rejects
	// anything else - including partially numeric text. Width/Height are left at
	// InvalidComponent/InvalidHeight and bValid false on failure, so a rejected string never
	// leaves a half-written resolution behind.
	FRT_CORE_API bool Parse (FStringView Text);

	// Round-trips through Parse. Written into config, so the delimiter has to be one Parse
	// accepts.
	FRT_CORE_API FString ToString (char Delimiter = 'x') const;

	operator bool () const
	{
		return IsValid();
	}
};

inline bool operator == (const SResolution& Lhs, const SResolution& Rhs)
{
	return Lhs.IsValid() == Rhs.IsValid()
		&& Lhs.GetWidth() == Rhs.GetWidth()
		&& Lhs.GetHeight() == Rhs.GetHeight();
}
}
