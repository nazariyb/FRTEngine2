#pragma once

#include <filesystem>
#include <vector>

#include "Graphics/Render/RHI/Device.h"


namespace frt::graphics
{
/**
 * GPU -> disk captures without stalling: a copy into a readback buffer is recorded this frame
 * and the file is written once the frame's fence has completed (Poll). 8-bit targets become
 * PNG (WIC), float targets become PFM (linear RGB, bottom-up), for automated verification and
 * reference comparisons.
 */
class FRT_CORE_API CFrameCapture
{
public:
	void Initialize (rhi::CDevice& Device);
	void Shutdown ();

	void Record (ID3D12GraphicsCommandList* CommandList, ID3D12Resource* Source, D3D12_RESOURCE_STATES SourceState,
		const std::filesystem::path& Path);

	// Writes every capture whose copy has completed. Returns the number of files written.
	uint32 Poll (uint64 CompletedFenceValue);
	bool HasPending () const { return !Pending.empty(); }

private:
	struct SPending
	{
		rhi::SGpuBuffer Readback;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint = {};
		uint32 Width = 0u;
		uint32 Height = 0u;
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		std::filesystem::path Path;
		uint64 FenceValue = 0u;
	};

	bool WriteFile (const SPending& Capture) const;

	rhi::CDevice* Device = nullptr;
	std::vector<SPending> Pending;
};
}
