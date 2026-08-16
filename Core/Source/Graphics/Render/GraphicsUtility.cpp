#include "GraphicsUtility.h"

#include <algorithm>
#include <cstdio>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <ranges>
#include <vector>
#include <wrl/client.h>

#include "Asserts.h"
#include "CoreTypes.h"
#include "Exception.h"
#include "RenderCommonTypes.h"


namespace frt::graphics
{
D3D12_SHADER_BYTECODE Dx12LoadShader (const char* Filename)
{
	D3D12_SHADER_BYTECODE result;

	FILE* file;
	(void)fopen_s(&file, Filename, "rb");
	frt_assert(file);
	(void)fseek(file, 0, SEEK_END);
	result.BytecodeLength = ftell(file);
	(void)fseek(file, 0, SEEK_SET);

	void* fileData = malloc(result.BytecodeLength);
	(void)fread(fileData, 1, result.BytecodeLength, file);
	result.pShaderBytecode = fileData;

	(void)fclose(file);

	return result;
}

void GetAdapterOutputs (IDXGIAdapter1* InAdapter, std::vector<IDXGIOutput*>& OutOutputs)
{
	IDXGIOutput* output = nullptr;

	uint32 i = 0;
	while (InAdapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND)
	{
		++i;
		OutOutputs.push_back(output);
	}
}

std::vector<DXGI_MODE_DESC> GetOutputDisplayModes (IDXGIOutput* InOutput, DXGI_FORMAT InFormat)
{
	uint32 count = 0;
	uint32 flags = 0;

	THROW_IF_FAILED(InOutput->GetDisplayModeList(InFormat, flags, &count, nullptr));

	std::vector<DXGI_MODE_DESC> outModes;
	outModes.resize(count);
	THROW_IF_FAILED(InOutput->GetDisplayModeList(InFormat, flags, &count, outModes.data()));
	return outModes;
}

namespace
{
// Fills OutOptions from the outputs attached to InAdapter, and reports how many there
// were. Leaves OutOptions empty when the adapter drives no display.
uint8 CollectAdapterOutputs (IDXGIAdapter1* InAdapter, SDisplayOptions& OutOptions)
{
	Microsoft::WRL::ComPtr<IDXGIOutput> output;

	OutOptions.OutputsNum = 0;
	while (InAdapter->EnumOutputs(OutOptions.OutputsNum, output.ReleaseAndGetAddressOf())
		!= DXGI_ERROR_NOT_FOUND)
	{
		++OutOptions.OutputsNum;

		DXGI_OUTPUT_DESC outputDesc;
		output->GetDesc(&outputDesc);
		OutOptions.OutputsNames.emplace_back(outputDesc.DeviceName);
		const RECT& rect = outputDesc.DesktopCoordinates;
		OutOptions.OutputsRects.push_back({ rect.left, rect.top, rect.right, rect.bottom });

		auto& outputModes = OutOptions.OutputsModes.emplace_back();
		for (const DXGI_MODE_DESC& mode : GetOutputDisplayModes(output.Get(), DXGI_FORMAT_R8G8B8A8_UNORM))
		{
			outputModes.emplace_back(
				SOutputModeInfo
				{
					.Width = mode.Width,
					.Height = mode.Height,
					.Numerator = mode.RefreshRate.Numerator,
					.Denominator = mode.RefreshRate.Denominator
				});
		}
	}

	return OutOptions.OutputsNum;
}
}

SDisplayOptions GetDisplayOptions (IDXGIAdapter1* InAdapter)
{
	SDisplayOptions result;

	if (CollectAdapterOutputs(InAdapter, result) > 0)
	{
		return result;
	}

	// A hybrid-graphics laptop wires the panel to the integrated GPU, so the discrete
	// adapter we render on owns no DXGI outputs at all. Presenting still works - the
	// driver does the cross-adapter copy - but mode enumeration has to come from
	// whichever adapter actually drives a display, otherwise OutputsNum stays 0 and
	// every OutputIndex lookup in SDisplayOptions asserts.
	Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
	if (FAILED(InAdapter->GetParent(IID_PPV_ARGS(&factory))))
	{
		return result;
	}

	Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
	for (uint32 i = 0;
		factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
		++i)
	{
		if (CollectAdapterOutputs(adapter.Get(), result) > 0)
		{
			return result;
		}
	}

	return result;
}
}
