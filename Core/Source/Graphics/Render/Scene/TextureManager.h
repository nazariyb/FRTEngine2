#pragma once

#include <DirectXMath.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <vector>

#include "Containers/StringMap.h"
#include "Graphics/Render/RHI/Device.h"


namespace frt::jobs
{
class CWorkerPool;
}

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
 * Request() returns a stable handle immediately; decoding and mip generation run on the engine's
 * worker pool (Jobs/WorkerPool.h, one task per texture at Normal priority, in a cancel group of
 * this manager's). The decode fills a slot of its own that Update() polls, as it polled the
 * std::future this replaced: a decode is taken by the first Update() after it finished, whether
 * or not the pool's completions have been pumped since. Finished textures are uploaded under a
 * per-frame byte budget from Update(). Until then GetSrv() reports InvalidDescriptorIndex and
 * shaders fall back to the material factors. Textures are deduplicated by (path, colour space).
 *
 * Scripted runs need every frame to be the same from run to run, and the frame a texture
 * becomes resident is otherwise up to the decode threads: SetDeterministic(true) makes Update()
 * wait for the textures still decoding (a pump loop, CWorkerPool::PumpUntil), so residency depends
 * only on the frame a texture was requested in and the upload budget. Interactive runs never wait:
 * a decode cannot stall a frame.
 *
 * A decode that ended without an answer - cancelled while it ran, or dropped before it started, as
 * CancelAllAndDrain does - is submitted again: the texture still gets resident.
 */
class FRT_CORE_API CTextureManager
{
public:
	using HTexture = uint32;
	static constexpr HTexture InvalidHandle = 0xFFFFFFFFu;

	CTextureManager () = default;
	CTextureManager (const CTextureManager&) = delete;
	CTextureManager& operator= (const CTextureManager&) = delete;

	// WorkerPool must outlive the manager, and be owned by the thread that calls Update.
	void Initialize (rhi::CDevice& Device, jobs::CWorkerPool& WorkerPool);
	// Cancels the decodes still queued or running (their results are thrown away) and releases
	// every texture.
	void Shutdown ();

	HTexture Request (const std::filesystem::path& Path, ETextureColorSpace ColorSpace);
	uint32 GetSrv (HTexture Handle) const;
	// Mean linear colour of the texture (top mip), available once resident. White otherwise.
	DirectX::XMFLOAT4 GetAverageColor (HTexture Handle) const;

	// Uploads finished textures. Returns true if any texture became resident this frame
	// (materials referencing it need refreshing and progressive accumulation must restart).
	bool Update (ID3D12GraphicsCommandList* CommandList, uint64 UploadBudgetBytes = 96ull * 1024ull * 1024ull);

	void SetDeterministic (bool bInDeterministic) { bDeterministic = bInDeterministic; }

	// Requested and neither resident nor failed: still decoding, or waiting for upload budget.
	uint32 GetPendingCount () const;
	uint32 GetResidentCount () const;
	uint64 GetResidentBytes () const { return ResidentBytes; }

private:
	// One decode's slot, the future of the std::future it replaces: the decode writes the fields on
	// a worker and then sets bDone (release); the main thread reads them only once it has seen bDone
	// (acquire). bDone is set when the decode's work callable dies, whether it ran or the pool dropped
	// it unrun, the way a std::promise destroyed unfulfilled still makes its future ready: a slot
	// always fills, and Update never waits on one that cannot.
	struct SDecoded
	{
		uint32 Width = 0u;
		uint32 Height = 0u;
		std::vector<std::vector<uint8>> Mips;
		DirectX::XMFLOAT4 Average = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool bValid = false;
		// The decode reached an answer, the texture or a failure to load it. False when it was
		// cancelled while it ran or never started: decode it again.
		bool bFinished = false;
		std::atomic<bool> bDone = false;
	};

	struct SEntry
	{
		std::filesystem::path Path;
		ETextureColorSpace ColorSpace = ETextureColorSpace::Srgb;
		// The decode in flight; null once taken into Ready.
		std::shared_ptr<SDecoded> Pending;
		// The finished decode, from the slot, until it is uploaded.
		std::shared_ptr<SDecoded> Ready;
		rhi::SGpuTexture Texture;
		DirectX::XMFLOAT4 Average = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool bResident = false;
		bool bFailed = false;
	};

	// On a worker: std containers and the CRT heap only.
	static void Decode (const std::filesystem::path& Path, ETextureColorSpace ColorSpace, SDecoded& Out);
	// Submits Entry's decode with a fresh slot in Entry.Pending. False when the pool dropped it at
	// once (it is draining): the slot is done, without an answer.
	bool SubmitDecode (SEntry& Entry);
	// Once Entry's slot is done: takes a finished decode into Ready, or submits one that ended
	// without an answer again. Returns whether Ready is set.
	bool PollDecode (SEntry& Entry);
	// Deterministic mode: pumps the pool until Entry's decode is back.
	void WaitForDecode (SEntry& Entry);

	static_assert(static_cast<size_t>(ETextureColorSpace::Normal) == 2u, "Lookup holds one map per colour space");

	rhi::CDevice* Device = nullptr;
	jobs::CWorkerPool* WorkerPool = nullptr;
	uint64 DecodeGroup = 0u;
	std::vector<std::unique_ptr<SEntry>> Entries;
	std::array<TPathMap<HTexture>, 3> Lookup;   // per ETextureColorSpace: resolved path -> Entries index
	uint64 ResidentBytes = 0u;
	bool bDeterministic = false;
};
}
