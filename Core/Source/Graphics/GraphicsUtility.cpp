#include "GraphicsUtility.h"

#include <cstdio>
#include <dxgi.h>
#include <vector>

#include "Asserts.h"
#include "CoreTypes.h"
#include "Exception.h"

namespace frt::graphics
{
	D3D12_SHADER_BYTECODE Dx12LoadShader(const char* Filename)
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

	void GetAdapterOutputs(IDXGIAdapter1* InAdapter, std::vector<IDXGIOutput*>& OutOutputs)
	{
		IDXGIOutput* output = nullptr;

		uint32 i = 0;
		while (InAdapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND)
		{
			++i;
			OutOutputs.push_back(output);
		}
	}

	std::vector<DXGI_MODE_DESC> GetOutputDisplayModes(IDXGIOutput* InOutput, DXGI_FORMAT InFormat, std::vector<DXGI_MODE_DESC>& OutModes)
	{
		uint32 count = 0;
		uint32 flags = 0;

		THROW_IF_FAILED(InOutput->GetDisplayModeList(InFormat, flags, &count, nullptr));
		OutModes.reserve(count);

		std::vector<DXGI_MODE_DESC> outModes(count);
		THROW_IF_FAILED(InOutput->GetDisplayModeList(InFormat, flags, &count, outModes.data()));
		return outModes;
	}
}
