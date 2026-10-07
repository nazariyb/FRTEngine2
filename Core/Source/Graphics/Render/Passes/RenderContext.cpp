#include "Graphics/Render/RenderContext.h"

#include "EnginePaths.h"


namespace frt::graphics
{
rhi::SShaderCompileDesc MakeShaderDesc (
	std::string_view RelativePath,
	std::string_view EntryPoint,
	std::string_view Profile,
	std::initializer_list<rhi::SShaderDefine> Defines)
{
	rhi::SShaderCompileDesc desc;
	desc.SourcePath = paths::GetShaderRootDir() / std::filesystem::path(RelativePath);
	desc.EntryPoint = std::string(EntryPoint);
	desc.Profile = std::string(Profile);
	desc.Defines.assign(Defines.begin(), Defines.end());
	return desc;
}
}
