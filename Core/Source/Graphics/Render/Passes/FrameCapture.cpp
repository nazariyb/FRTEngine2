#include "Graphics/Render/Passes/FrameCapture.h"

#include <DirectXPackedVector.h>
#include <wincodec.h>

#include <cstdio>
#include <fstream>
#include <utility>

#include "Containers/String.h"

#pragma comment(lib, "windowscodecs.lib")


namespace frt::graphics
{
namespace
{
bool WritePng (const std::filesystem::path& Path, uint32 Width, uint32 Height, const std::vector<uint8>& Rgba)
{
	rhi::ComPtr<IWICImagingFactory> factory;
	const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool bUninit = SUCCEEDED(init);
	bool bOk = false;
	if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
	{
		rhi::ComPtr<IWICStream> stream;
		rhi::ComPtr<IWICBitmapEncoder> encoder;
		rhi::ComPtr<IWICBitmapFrameEncode> frame;
		if (SUCCEEDED(factory->CreateStream(&stream)) &&
			SUCCEEDED(stream->InitializeFromFilename(Path.c_str(), GENERIC_WRITE)) &&
			SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
			SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
			SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
			SUCCEEDED(frame->Initialize(nullptr)) &&
			SUCCEEDED(frame->SetSize(Width, Height)))
		{
			// BGRA is the PNG encoder's native 32-bit format; RGBA gets negotiated away.
			std::vector<uint8> bgra(Rgba);
			for (size_t i = 0; i + 3 < bgra.size(); i += 4)
			{
				std::swap(bgra[i], bgra[i + 2]);
			}
			WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
			if (SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA &&
				SUCCEEDED(frame->WritePixels(Height, Width * 4u, static_cast<UINT>(bgra.size()), bgra.data())) &&
				SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()))
			{
				bOk = true;
			}
		}
	}
	if (bUninit)
	{
		CoUninitialize();
	}
	return bOk;
}
}

void CFrameCapture::Initialize (rhi::CDevice& InDevice)
{
	Device = &InDevice;
}

void CFrameCapture::Shutdown ()
{
	if (!Device)
	{
		return;
	}
	for (SPending& p : Pending)
	{
		Device->Destroy(p.Readback);
	}
	Pending.clear();
	Device = nullptr;
}

void CFrameCapture::Record (
	ID3D12GraphicsCommandList* CommandList,
	ID3D12Resource* Source,
	D3D12_RESOURCE_STATES SourceState,
	const std::filesystem::path& Path)
{
	const D3D12_RESOURCE_DESC desc = Source->GetDesc();
	SPending capture;
	capture.Width = static_cast<uint32>(desc.Width);
	capture.Height = desc.Height;
	capture.Format = desc.Format;
	capture.Path = Path;
	capture.FenceValue = Device->GetSubmissionFenceValue();

	UINT64 totalBytes = 0u;
	Device->GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &capture.Footprint, nullptr, nullptr, &totalBytes);

	rhi::SBufferDesc readback;
	readback.Size = totalBytes;
	readback.HeapType = D3D12_HEAP_TYPE_READBACK;
	readback.Name = "FrameCaptureReadback";
	capture.Readback = Device->CreateBuffer(readback);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = Source;
	barrier.Transition.Subresource = 0;
	barrier.Transition.StateBefore = SourceState;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	if (SourceState != D3D12_RESOURCE_STATE_COPY_SOURCE)
	{
		CommandList->ResourceBarrier(1, &barrier);
	}

	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = capture.Readback.Resource.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = capture.Footprint;
	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = Source;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;
	CommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	if (SourceState != D3D12_RESOURCE_STATE_COPY_SOURCE)
	{
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		CommandList->ResourceBarrier(1, &barrier);
	}
	Pending.push_back(std::move(capture));
}

bool CFrameCapture::WriteFile (const SPending& Capture) const
{
	std::error_code ec;
	std::filesystem::create_directories(Capture.Path.parent_path(), ec);
	const uint8* base = Capture.Readback.Mapped;
	const uint32 rowPitch = Capture.Footprint.Footprint.RowPitch;
	const uint32 w = Capture.Width;
	const uint32 h = Capture.Height;

	switch (Capture.Format)
	{
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		{
			const bool bBgra = Capture.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
			std::vector<uint8> rgba(static_cast<size_t>(w) * h * 4u);
			for (uint32 y = 0; y < h; ++y)
			{
				const uint8* row = base + static_cast<size_t>(y) * rowPitch;
				for (uint32 x = 0; x < w; ++x)
				{
					uint8* o = &rgba[(static_cast<size_t>(y) * w + x) * 4u];
					o[0] = row[x * 4u + (bBgra ? 2u : 0u)];
					o[1] = row[x * 4u + 1u];
					o[2] = row[x * 4u + (bBgra ? 0u : 2u)];
					o[3] = 255u;
				}
			}
			return WritePng(Capture.Path, w, h, rgba);
		}
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		{
			const bool bHalf = Capture.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
			std::ofstream file(Capture.Path, std::ios::binary);
			if (!file)
			{
				return false;
			}
			const FString header = FString::Format("PF\n{} {}\n-1.0\n", w, h);
			file.write(header.GetData(), header.Len());
			std::vector<float> rgb(static_cast<size_t>(w) * 3u);
			for (uint32 yy = 0; yy < h; ++yy)
			{
				const uint32 y = h - 1u - yy; // PFM rows are stored bottom-up
				const uint8* row = base + static_cast<size_t>(y) * rowPitch;
				for (uint32 x = 0; x < w; ++x)
				{
					for (uint32 c = 0; c < 3u; ++c)
					{
						if (bHalf)
						{
							const uint16 hv = reinterpret_cast<const uint16*>(row)[x * 4u + c];
							rgb[x * 3u + c] = DirectX::PackedVector::XMConvertHalfToFloat(hv);
						}
						else
						{
							rgb[x * 3u + c] = reinterpret_cast<const float*>(row)[x * 4u + c];
						}
					}
				}
				file.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size() * sizeof(float)));
			}
			return true;
		}
		default:
			std::fprintf(stderr, "[capture] unsupported format %u for %s\n", static_cast<uint32>(Capture.Format), FString::FromPath(Capture.Path).CStr());
			return false;
	}
}

uint32 CFrameCapture::Poll (uint64 CompletedFenceValue)
{
	uint32 written = 0u;
	for (auto it = Pending.begin(); it != Pending.end();)
	{
		if (it->FenceValue > CompletedFenceValue)
		{
			++it;
			continue;
		}
		if (WriteFile(*it))
		{
			std::fprintf(stdout, "[capture] wrote %s\n", FString::FromPath(it->Path).CStr());
			std::fflush(stdout);
			++written;
		}
		else
		{
			std::fprintf(stderr, "[capture] FAILED %s\n", FString::FromPath(it->Path).CStr());
		}
		Device->Destroy(it->Readback);
		it = Pending.erase(it);
	}
	return written;
}
}
