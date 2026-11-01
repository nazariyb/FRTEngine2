// The engine's builders and their versions (design §7.3): one row per builder, read by C++ (an X-macro:
// Assets/BuilderGuard.cpp) and by Premake's Lua (FRT.bat builder-hashes and check-builders,
// Premake/tools.lua), so there is one table and no copy of it to drift.
//
//   FRT_BUILDER(Name, "VersionGuid", "CommittedSourceHash", "glob;glob")
//
//   Name                 the builder's name as keys write it ("Builder" field), a string literal
//   VersionGuid          32 hex digits: the builder's version (FRT.bat assets newguid). A new GUID
//                        invalidates every output the builder ever made, on every machine; never an
//                        integer, which two branches would bump to the same value
//   CommittedSourceHash  16 hex digits: the hash of the builder's sources when VersionGuid was set,
//                        written by FRT.bat check-builders --update, never by hand. A GUID is paired
//                        with one hash, ever: a later change needs another new GUID
//   globs                the builder's sources, engine-relative, ';'-separated: everything whose
//                        change can change its output. The key infrastructure - the key encoder
//                        (KeyWriter.*, ByteStream.h) and BuilderGuard.h - is a source of every builder
//                        without being listed: FRT.bat adds it to each row (Premake/tools.lua,
//                        keyInfrastructureGlobs), so no row can forget it
//
// Every argument is a string literal; adjacent literals are joined, as in C++, so a long glob list
// splits over lines. Nothing else goes inside a row (no macros, no comments).
//
// The rule (BUILD003): whoever changes a builder's sources gives it a new VersionGuid and then runs
// FRT.bat check-builders --update. Until then the build notices on its own: every build hashes the
// globs' files into Intermediate/Generated/BuilderSourceHashes.gen.h, and a development build whose
// hash differs from the committed one keys that builder's outputs with a local-only discriminator
// (Assets/BuilderGuard.h), so an edited builder never serves or publishes stale values. FRT.bat verify
// fails until the GUID is bumped.
//
// No include guard: an X-macro table, included once per use with FRT_BUILDER defined.

FRT_BUILDER("Shader.Dxil", "6356584acd9c41a18b7b695fe38ea7e2", "3afe533586cc96fe",
	"Core/Source/Graphics/Render/RHI/ShaderCompiler.*;"
	"Core/Source/Graphics/Render/RHI/ShaderCache.*;"
	"Core/Source/Platform/TextDecoding.cpp")
