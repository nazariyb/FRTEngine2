#include "GraphicsUtility.h"

#include <algorithm>
#include <cstdio>
#include <dxgi.h>
#include <ranges>
#include <vector>

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

SDisplayOptions GetDisplayOptions (IDXGIAdapter1* InAdapter)
{
	SDisplayOptions result;

	IDXGIOutput* output = nullptr;

	result.OutputsNum = 0;
	while (InAdapter->EnumOutputs(result.OutputsNum, &output) != DXGI_ERROR_NOT_FOUND)
	{
		++result.OutputsNum;

		DXGI_OUTPUT_DESC outputDesc;
		output->GetDesc(&outputDesc);
		result.OutputsNames.emplace_back(outputDesc.DeviceName);
		const RECT& rect = outputDesc.DesktopCoordinates;
		result.OutputsRects.push_back({ rect.left, rect.top, rect.right, rect.bottom });

		auto& outputModes = result.OutputsModes.emplace_back();
		for (const DXGI_MODE_DESC& mode : GetOutputDisplayModes(output, DXGI_FORMAT_R8G8B8A8_UNORM))
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

	return result;
}
}
