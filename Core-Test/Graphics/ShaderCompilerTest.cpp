// Shader files as editors save them: with a UTF-8 BOM ("UTF-8 with signature", old Notepad), as
// UTF-16 ("Unicode") or as plain UTF-8. Each must compile, the source and its include alike, and
// reading them must leave the heap intact: DXC's own LoadFile, asked for UTF-8, frees a pointer it
// moved past the BOM.
#include <gtest/gtest.h>

#include <filesystem>
#include <system_error>

#include "Containers/String.h"
#include "Graphics/Render/RHI/ShaderCompiler.h"
#include "Platform/Platform.h"

#include <Windows.h>

using namespace frt;
using namespace frt::graphics::rhi;


namespace
{
// Non-ASCII comments in both, so a file read in the wrong encoding shows as an error, not as nothing.
constexpr FStringLiteral IncludeText =
	"// Спільне: the value the shader writes\n"
	"static const uint Answer = 42u;\n";

constexpr FStringLiteral MainText =
	"// Головний шейдер\n"
	"#include \"Common.hlsli\"\n"
	"RWStructuredBuffer<uint> Output : register(u0);\n"
	"[numthreads(1, 1, 1)]\n"
	"void main () { Output[0] = Answer; }\n";

// The file a Windows tool writes as "Unicode": UTF-16LE after its BOM.
FString MakeUtf16LeFile (FStringView Text)
{
	FString bytes("\xFF\xFE", 2u);
	const auto appendUnit = [&bytes] (uint32 Unit)
	{
		bytes.AppendChar(static_cast<char>(Unit & 0xFFu));
		bytes.AppendChar(static_cast<char>(Unit >> 8u));
	};
	for (const char32_t codePoint : Text.CodePoints())
	{
		if (codePoint < 0x10000u)
		{
			appendUnit(codePoint);
		}
		else
		{
			appendUnit(0xD800u + ((codePoint - 0x10000u) >> 10u));
			appendUnit(0xDC00u + ((codePoint - 0x10000u) & 0x3FFu));
		}
	}
	return bytes;
}

// In a directory with a non-ASCII name, of this process's own (PlatformTest.cpp has the reasons).
class ShaderCompilerTest : public ::testing::Test
{
protected:
	void SetUp () override
	{
		Dir = std::filesystem::temp_directory_path() / FString::Format("FRT Shader Тест {}", GetCurrentProcessId()).ToPath();
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
		ASSERT_TRUE(std::filesystem::create_directories(Dir, ec)) << FString::FromUtf8Lossy(ec.message()).CStr();
		ASSERT_TRUE(Compiler.Init(Dir));
	}

	void TearDown () override
	{
		std::error_code ec;
		std::filesystem::remove_all(Dir, ec);
	}

	// Writes Main.hlsl and the Common.hlsli it includes, compiles Main.hlsl as a compute shader.
	SShaderBinary WriteAndCompile (FStringView MainBytes, FStringView IncludeBytes)
	{
		const std::filesystem::path source = Dir / "Main.hlsl";
		EXPECT_TRUE(platform::WriteFileText(source, MainBytes));
		EXPECT_TRUE(platform::WriteFileText(Dir / "Common.hlsli", IncludeBytes));

		SShaderCompileDesc desc;
		desc.SourcePath = source;
		desc.EntryPoint = "main";
		desc.Profile = "cs_6_6";
		return Compiler.Compile(desc);
	}

	std::filesystem::path Dir;
	CShaderCompiler Compiler;
};
}


TEST_F(ShaderCompilerTest, CompilesFilesWithAUtf8ByteOrderMark)
{
	const SShaderBinary binary = WriteAndCompile(FString(Utf8Bom) + MainText, FString(Utf8Bom) + IncludeText);
	EXPECT_TRUE(binary.bSuccess) << binary.Log;
	EXPECT_EQ(binary.Dependencies.size(), 2u);   // the source and the include it opened

	// Again, now that the first blobs are gone: the corruption showed when DXC released them.
	EXPECT_TRUE(WriteAndCompile(FString(Utf8Bom) + MainText, IncludeText).bSuccess);
	EXPECT_TRUE(HeapValidate(GetProcessHeap(), 0u, nullptr));
}

TEST_F(ShaderCompilerTest, CompilesUtf16Files)
{
	const SShaderBinary binary = WriteAndCompile(MakeUtf16LeFile(MainText), MakeUtf16LeFile(IncludeText));
	EXPECT_TRUE(binary.bSuccess) << binary.Log;
	EXPECT_EQ(binary.Dependencies.size(), 2u);
	EXPECT_TRUE(HeapValidate(GetProcessHeap(), 0u, nullptr));
}

TEST_F(ShaderCompilerTest, CompilesUtf8FilesWithoutAByteOrderMark)
{
	const SShaderBinary binary = WriteAndCompile(MainText, IncludeText);
	EXPECT_TRUE(binary.bSuccess) << binary.Log;
	EXPECT_EQ(binary.Dependencies.size(), 2u);

	// A compile error is reported, not swallowed: the check above is not passing by accident.
	EXPECT_FALSE(WriteAndCompile(MainText, "static const uint Answer = ;\n").bSuccess);
}
