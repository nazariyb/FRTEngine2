#include "Graphics/Render/Scene/TextureManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <semaphore>
#include <thread>

#include <stb_image.h>


namespace frt::graphics
{
namespace
{
// Bounded decode concurrency: stb_image + mip generation is CPU-heavy and a large scene
// requests every texture in the same frame.
std::counting_semaphore<64>& DecodeSlots ()
{
	static std::counting_semaphore<64> slots(
		static_cast<std::ptrdiff_t>(std::clamp<uint32>(std::thread::hardware_concurrency() > 2u ? std::thread::hardware_concurrency() - 2u : 1u, 1u, 16u)));
	return slots;
}

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


void CTextureManager::Initialize (rhi::CDevice& InDevice)
{
	Device = &InDevice;
}

void CTextureManager::Shutdown ()
{
	for (std::unique_ptr<SEntry>& entry : Entries)
	{
		if (entry->Pending.valid())
		{
			entry->Pending.wait();
		}
		if (entry->Texture.IsValid())
		{
			Device->Destroy(entry->Texture);
		}
	}
	Entries.clear();
	Lookup.clear();
	ResidentBytes = 0u;
}

std::shared_ptr<CTextureManager::SDecoded> CTextureManager::Decode (std::filesystem::path Path, ETextureColorSpace ColorSpace)
{
	DecodeSlots().acquire();
	auto result = std::make_shared<SDecoded>();

	int width = 0;
	int height = 0;
	int channels = 0;
	stbi_uc* pixels = stbi_load(Path.string().c_str(), &width, &height, &channels, 4);
	if (!pixels || width <= 0 || height <= 0)
	{
		std::fprintf(stderr, "[texture] failed to load %s: %s\n", Path.string().c_str(), stbi_failure_reason());
		if (pixels)
		{
			stbi_image_free(pixels);
		}
		DecodeSlots().release();
		return result;
	}

	result->Width = static_cast<uint32>(width);
	result->Height = static_cast<uint32>(height);
	const uint32 mipCount = (std::min)(rhi::ComputeMipCount(result->Width, result->Height), rhi::MaxTextureMips);
	result->Mips.resize(mipCount);
	result->Mips[0].assign(pixels, pixels + static_cast<size_t>(width) * height * 4u);
	stbi_image_free(pixels);

	uint32 w = result->Width;
	uint32 h = result->Height;
	for (uint32 mip = 1; mip < mipCount; ++mip)
	{
		const uint32 nw = (std::max)(1u, w / 2u);
		const uint32 nh = (std::max)(1u, h / 2u);
		result->Mips[mip].resize(static_cast<size_t>(nw) * nh * 4u);
		Downsample(result->Mips[mip - 1].data(), w, h, result->Mips[mip].data(), nw, nh, ColorSpace);
		w = nw;
		h = nh;
	}

	// The 1x1 mip is the texture mean (in the filtering space).
	const uint8* top = result->Mips.back().data();
	if (ColorSpace == ETextureColorSpace::Srgb)
	{
		const SSrgbTables& srgb = SrgbTables();
		result->Average = { srgb.ToLinear[top[0]], srgb.ToLinear[top[1]], srgb.ToLinear[top[2]], top[3] / 255.0f };
	}
	else
	{
		result->Average = { top[0] / 255.0f, top[1] / 255.0f, top[2] / 255.0f, top[3] / 255.0f };
	}
	result->bValid = true;
	DecodeSlots().release();
	return result;
}

CTextureManager::HTexture CTextureManager::Request (const std::filesystem::path& Path, ETextureColorSpace ColorSpace)
{
	if (Path.empty())
	{
		return InvalidHandle;
	}
	std::error_code ec;
	const std::filesystem::path normal = std::filesystem::weakly_canonical(Path, ec);
	const std::string key = (ec ? Path : normal).string() + "|" + std::to_string(static_cast<uint32>(ColorSpace));
	if (const auto it = Lookup.find(key); it != Lookup.end())
	{
		return it->second;
	}

	auto entry = std::make_unique<SEntry>();
	entry->Path = ec ? Path : normal;
	entry->ColorSpace = ColorSpace;
	entry->Pending = std::async(std::launch::async, &CTextureManager::Decode, entry->Path, ColorSpace);
	const HTexture handle = static_cast<HTexture>(Entries.size());
	Entries.push_back(std::move(entry));
	Lookup.emplace(key, handle);
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
			if (!entry->Pending.valid() ||
				entry->Pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			{
				continue;
			}
			entry->Ready = entry->Pending.get();
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
		const std::wstring name = entry->Path.filename().wstring();
		desc.Name = name.c_str();
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
