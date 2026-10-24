#include "Graphics/Render/Scene/TextureManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <stb_image.h>

#include "Containers/String.h"
#include "Jobs/WorkerPool.h"


namespace frt::graphics
{
namespace
{
struct SSrgbTables
{
	float ToLinear[256];
	SSrgbTables ()
	{
		for (uint32 i = 0; i < 256; ++i)
		{
			const float c = static_cast<float>(i) / 255.0f;
			ToLinear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
		}
	}
};

const SSrgbTables& SrgbTables ()
{
	static SSrgbTables tables;
	return tables;
}

uint8 LinearToSrgb8 (float Linear)
{
	Linear = std::clamp(Linear, 0.0f, 1.0f);
	const float s = Linear <= 0.0031308f ? Linear * 12.92f : 1.055f * std::pow(Linear, 1.0f / 2.4f) - 0.055f;
	return static_cast<uint8>(std::clamp(s * 255.0f + 0.5f, 0.0f, 255.0f));
}

uint8 ToUnorm8 (float V)
{
	return static_cast<uint8>(std::clamp(V * 255.0f + 0.5f, 0.0f, 255.0f));
}

// 2x2 box downsample (odd sizes clamp the last row/column). Colour data is filtered in linear
// space; normal maps are averaged as vectors and renormalised.
void Downsample (const uint8* Src, uint32 SrcW, uint32 SrcH, uint8* Dst, uint32 DstW, uint32 DstH, ETextureColorSpace Space)
{
	const SSrgbTables& srgb = SrgbTables();
	for (uint32 y = 0; y < DstH; ++y)
	{
		for (uint32 x = 0; x < DstW; ++x)
		{
			float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (uint32 dy = 0; dy < 2; ++dy)
			{
				const uint32 sy = (std::min)(y * 2u + dy, SrcH - 1u);
				for (uint32 dx = 0; dx < 2; ++dx)
				{
					const uint32 sx = (std::min)(x * 2u + dx, SrcW - 1u);
					const uint8* p = Src + (static_cast<size_t>(sy) * SrcW + sx) * 4u;
					if (Space == ETextureColorSpace::Srgb)
					{
						acc[0] += srgb.ToLinear[p[0]];
						acc[1] += srgb.ToLinear[p[1]];
						acc[2] += srgb.ToLinear[p[2]];
					}
					else if (Space == ETextureColorSpace::Normal)
					{
						acc[0] += p[0] / 127.5f - 1.0f;
						acc[1] += p[1] / 127.5f - 1.0f;
						acc[2] += p[2] / 127.5f - 1.0f;
					}
					else
					{
						acc[0] += p[0] / 255.0f;
						acc[1] += p[1] / 255.0f;
						acc[2] += p[2] / 255.0f;
					}
					acc[3] += p[3] / 255.0f;
				}
			}
			uint8* o = Dst + (static_cast<size_t>(y) * DstW + x) * 4u;
			if (Space == ETextureColorSpace::Srgb)
			{
				o[0] = LinearToSrgb8(acc[0] * 0.25f);
				o[1] = LinearToSrgb8(acc[1] * 0.25f);
				o[2] = LinearToSrgb8(acc[2] * 0.25f);
			}
			else if (Space == ETextureColorSpace::Normal)
			{
				float nx = acc[0] * 0.25f, ny = acc[1] * 0.25f, nz = acc[2] * 0.25f;
				const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (len > 1e-6f)
				{
					nx /= len; ny /= len; nz /= len;
				}
				else
				{
					nx = 0.0f; ny = 0.0f; nz = 1.0f;
				}
				o[0] = ToUnorm8(nx * 0.5f + 0.5f);
				o[1] = ToUnorm8(ny * 0.5f + 0.5f);
				o[2] = ToUnorm8(nz * 0.5f + 0.5f);
			}
			else
			{
				o[0] = ToUnorm8(acc[0] * 0.25f);
				o[1] = ToUnorm8(acc[1] * 0.25f);
				o[2] = ToUnorm8(acc[2] * 0.25f);
			}
			o[3] = ToUnorm8(acc[3] * 0.25f);
		}
	}
}
}


void CTextureManager::Initialize (rhi::CDevice& InDevice, jobs::CWorkerPool& InWorkerPool)
{
	Device = &InDevice;
	WorkerPool = &InWorkerPool;
	DecodeGroup = WorkerPool->CreateCancelGroup();
}

void CTextureManager::Shutdown ()
{
	// Queued decodes are dropped, running ones finish on their own data, and no completion of this
	// manager runs after this line: the entries can go.
	if (WorkerPool != nullptr)
	{
		WorkerPool->Cancel(DecodeGroup);
	}
	for (std::unique_ptr<SEntry>& entry : Entries)
	{
		if (entry->Texture.IsValid())
		{
			Device->Destroy(entry->Texture);
		}
	}
	Entries.clear();
	for (TPathMap<HTexture>& byPath : Lookup)
	{
		byPath.clear();
	}
	ResidentBytes = 0u;
}

void CTextureManager::Decode (const std::filesystem::path& Path, ETextureColorSpace ColorSpace, SDecoded& Out)
{
	// Opened by its UTF-16 name: stbi_load would take narrow text, which cannot name every file.
	FILE* file = nullptr;
	if (_wfopen_s(&file, Path.c_str(), L"rb") != 0 || file == nullptr)
	{
		std::fprintf(stderr, "[texture] failed to open %s\n", FString::FromPath(Path).CStr());
		return;
	}
	int width = 0;
	int height = 0;
	int channels = 0;
	stbi_uc* pixels = stbi_load_from_file(file, &width, &height, &channels, 4);
	std::fclose(file);
	if (!pixels || width <= 0 || height <= 0)
	{
		std::fprintf(stderr, "[texture] failed to load %s: %s\n", FString::FromPath(Path).CStr(), stbi_failure_reason());
		if (pixels)
		{
			stbi_image_free(pixels);
		}
		return;
	}
	if (jobs::CWorkerPool::IsCancelled())
	{
		// The manager shut down meanwhile: nobody will take the mips.
		stbi_image_free(pixels);
		return;
	}

	Out.Width = static_cast<uint32>(width);
	Out.Height = static_cast<uint32>(height);
	const uint32 mipCount = (std::min)(rhi::ComputeMipCount(Out.Width, Out.Height), rhi::MaxTextureMips);
	Out.Mips.resize(mipCount);
	Out.Mips[0].assign(pixels, pixels + static_cast<size_t>(width) * height * 4u);
	stbi_image_free(pixels);

	uint32 w = Out.Width;
	uint32 h = Out.Height;
	for (uint32 mip = 1; mip < mipCount; ++mip)
	{
		const uint32 nw = (std::max)(1u, w / 2u);
		const uint32 nh = (std::max)(1u, h / 2u);
		Out.Mips[mip].resize(static_cast<size_t>(nw) * nh * 4u);
		Downsample(Out.Mips[mip - 1].data(), w, h, Out.Mips[mip].data(), nw, nh, ColorSpace);
		w = nw;
		h = nh;
	}

	// The 1x1 mip is the texture mean (in the filtering space).
	const uint8* top = Out.Mips.back().data();
	if (ColorSpace == ETextureColorSpace::Srgb)
	{
		const SSrgbTables& srgb = SrgbTables();
		Out.Average = { srgb.ToLinear[top[0]], srgb.ToLinear[top[1]], srgb.ToLinear[top[2]], top[3] / 255.0f };
	}
	else
	{
		Out.Average = { top[0] / 255.0f, top[1] / 255.0f, top[2] / 255.0f, top[3] / 255.0f };
	}
	Out.bValid = true;
}

void CTextureManager::SubmitDecode (SEntry& Entry)
{
	// The decode fills its own SDecoded on a worker; the completion hands it to the entry on the main
	// thread. The entry's address is stable (Entries holds unique_ptrs), and Shutdown cancels the
	// group before any entry dies, so the completion never sees a dead one.
	auto decoded = std::make_shared<SDecoded>();
	SEntry* entry = &Entry;
	Entry.DecodeEpoch = WorkerPool->GetEpoch();
	WorkerPool->Submit(jobs::EWorkPriority::Normal, DecodeGroup,
		[decoded, path = Entry.Path, colorSpace = Entry.ColorSpace] () { Decode(path, colorSpace, *decoded); },
		[entry, decoded] () { entry->Ready = decoded; });
}

void CTextureManager::WaitForDecode (SEntry& Entry)
{
	while (!Entry.Ready)
	{
		if (Entry.DecodeEpoch != WorkerPool->GetEpoch())
		{
			SubmitDecode(Entry);
		}
		const uint64 epoch = Entry.DecodeEpoch;
		const bool bBack = WorkerPool->PumpUntil([this, &Entry, epoch] ()
		{
			return Entry.Ready != nullptr || WorkerPool->GetEpoch() != epoch;
		});
		if (!bBack)
		{
			// The pool went idle without this decode (nothing could bring it back): the texture fails
			// instead of the frame waiting forever.
			Entry.Ready = std::make_shared<SDecoded>();
		}
	}
}

CTextureManager::HTexture CTextureManager::Request (const std::filesystem::path& Path, ETextureColorSpace ColorSpace)
{
	if (Path.empty())
	{
		return InvalidHandle;
	}
	std::error_code ec;
	const std::filesystem::path normal = std::filesystem::weakly_canonical(Path, ec);
	const std::filesystem::path& resolved = ec ? Path : normal;
	// A path key: one texture per file however the request spells it (case, separators).
	FString key = FString::FromPath(resolved, EPathStyle::Generic);
	TPathMap<HTexture>& byPath = Lookup[static_cast<size_t>(ColorSpace)];
	if (const auto it = byPath.find(key); it != byPath.end())
	{
		return it->second;
	}

	auto entry = std::make_unique<SEntry>();
	entry->Path = resolved;
	entry->ColorSpace = ColorSpace;
	SubmitDecode(*entry);
	const HTexture handle = static_cast<HTexture>(Entries.size());
	Entries.push_back(std::move(entry));
	byPath.emplace(std::move(key), handle);
	return handle;
}

uint32 CTextureManager::GetSrv (HTexture Handle) const
{
	if (Handle >= Entries.size() || !Entries[Handle]->bResident)
	{
		return rhi::InvalidDescriptorIndex;
	}
	return Entries[Handle]->Texture.Srv;
}

DirectX::XMFLOAT4 CTextureManager::GetAverageColor (HTexture Handle) const
{
	if (Handle >= Entries.size() || !Entries[Handle]->bResident)
	{
		return { 1.0f, 1.0f, 1.0f, 1.0f };
	}
	return Entries[Handle]->Average;
}

uint32 CTextureManager::GetPendingCount () const
{
	uint32 count = 0u;
	for (const std::unique_ptr<SEntry>& entry : Entries)
	{
		count += (!entry->bResident && !entry->bFailed) ? 1u : 0u;
	}
	return count;
}

uint32 CTextureManager::GetResidentCount () const
{
	uint32 count = 0u;
	for (const std::unique_ptr<SEntry>& entry : Entries)
	{
		count += entry->bResident ? 1u : 0u;
	}
	return count;
}

bool CTextureManager::Update (ID3D12GraphicsCommandList* CommandList, uint64 UploadBudgetBytes)
{
	bool bAnyResident = false;
	uint64 uploaded = 0u;

	for (std::unique_ptr<SEntry>& entry : Entries)
	{
		if (entry->bResident || entry->bFailed)
		{
			continue;
		}
		if (!entry->Ready)
		{
			if (entry->DecodeEpoch != WorkerPool->GetEpoch())
			{
				// The pool threw the decode's completion away (a drain, a new epoch): decode again.
				SubmitDecode(*entry);
			}
			// Interactive: the decode is not back yet (its completion runs in a pump). Deterministic:
			// wait for it, pumping.
			if (!bDeterministic)
			{
				continue;
			}
			WaitForDecode(*entry);
		}
		if (!entry->Ready || !entry->Ready->bValid)
		{
			entry->bFailed = true;
			entry->Ready.reset();
			continue;
		}

		uint64 bytes = 0u;
		for (const std::vector<uint8>& mip : entry->Ready->Mips)
		{
			bytes += mip.size();
		}
		// Always let at least one texture through so a huge one cannot stall forever.
		if (uploaded > 0u && uploaded + bytes > UploadBudgetBytes)
		{
			break;
		}

		const SDecoded& decoded = *entry->Ready;
		rhi::STextureDesc desc;
		desc.Width = decoded.Width;
		desc.Height = decoded.Height;
		desc.MipLevels = static_cast<uint16>(decoded.Mips.size());
		desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		desc.SrvFormat = entry->ColorSpace == ETextureColorSpace::Srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.InitialState = D3D12_RESOURCE_STATE_COPY_DEST;
		const FString nameUtf8 = FString::FromPath(entry->Path.filename());
		desc.Name = nameUtf8;
		entry->Texture = Device->CreateTexture(desc);

		const void* mipData[rhi::MaxTextureMips] = {};
		for (size_t mip = 0; mip < decoded.Mips.size(); ++mip)
		{
			mipData[mip] = decoded.Mips[mip].data();
		}
		Device->UploadTexture(
			CommandList, entry->Texture, mipData, static_cast<uint32>(decoded.Mips.size()),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		entry->Average = decoded.Average;
		entry->Ready.reset();
		entry->bResident = true;
		uploaded += bytes;
		ResidentBytes += bytes;
		bAnyResident = true;
	}
	return bAnyResident;
}
}
