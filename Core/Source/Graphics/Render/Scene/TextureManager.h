#pragma once

#include <DirectXMath.h>

#include <array>
#include <filesystem>
#include <future>
#include <memory>
#include <vector>

#include "Containers/StringMap.h"
#include "Graphics/Render/RHI/Device.h"


namespace frt::graphics
{
enum class ETextureColorSpace : uint8
{
	Srgb,       // colour data (base colour, emissive): sRGB view, mips filtered in linear space
	Linear,     // data (metallic-roughness, occlusion)
	Normal,     // tangent-space normal map: mips renormalised
};

/**
 * Material texture streaming.
 *
 * Request() returns a stable handle immediately; decoding and mip generation run on worker
 * threads, and finished textures are uploaded under a per-frame byte budget from Update().
 * Until then GetSrv() reports InvalidDescriptorIndex and shaders fall back to the material
 * factors. Textures are deduplicated by (path, colour space).
 */
class FRT_CORE_API CTextureManager
{
public:
	using HTexture = uint32;
	static constexpr HTexture InvalidHandle = 0xFFFFFFFFu;

	CTextureManager () = default;
	CTextureManager (const CTextureManager&) = delete;
	CTextureManager& operator= (const CTextureManager&) = delete;

	void Initialize (rhi::CDevice& Device);
	void Shutdown ();

	HTexture Request (const std::filesystem::path& Path, ETextureColorSpace ColorSpace);
	uint32 GetSrv (HTexture Handle) const;
	// Mean linear colour of the texture (top mip), available once resident. White otherwise.
	DirectX::XMFLOAT4 GetAverageColor (HTexture Handle) const;

	// Uploads finished textures. Returns true if any texture became resident this frame
	// (materials referencing it need refreshing and progressive accumulation must restart).
	bool Update (ID3D12GraphicsCommandList* CommandList, uint64 UploadBudgetBytes = 96ull * 1024ull * 1024ull);

	uint32 GetPendingCount () const;
	uint32 GetResidentCount () const;
	uint64 GetResidentBytes () const { return ResidentBytes; }

private:
	struct SDecoded
	{
		uint32 Width = 0u;
		uint32 Height = 0u;
		std::vector<std::vector<uint8>> Mips;
		DirectX::XMFLOAT4 Average = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool bValid = false;
	};

	struct SEntry
	{
		std::filesystem::path Path;
		ETextureColorSpace ColorSpace = ETextureColorSpace::Srgb;
		std::future<std::shared_ptr<SDecoded>> Pending;
		std::shared_ptr<SDecoded> Ready;
		rhi::SGpuTexture Texture;
		DirectX::XMFLOAT4 Average = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool bResident = false;
		bool bFailed = false;
	};

	static std::shared_ptr<SDecoded> Decode (std::filesystem::path Path, ETextureColorSpace ColorSpace);

	static_assert(static_cast<size_t>(ETextureColorSpace::Normal) == 2u, "Lookup holds one map per colour space");

	rhi::CDevice* Device = nullptr;
	std::vector<std::unique_ptr<SEntry>> Entries;
	std::array<TPathMap<HTexture>, 3> Lookup;   // per ETextureColorSpace: resolved path -> Entries index
	uint64 ResidentBytes = 0u;
};
}
