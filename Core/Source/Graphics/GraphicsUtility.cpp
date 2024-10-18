#include "GraphicsUtility.h"

#include <cstdio>

#include "Asserts.h"

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
}
