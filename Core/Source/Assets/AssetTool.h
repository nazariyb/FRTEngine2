#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>

#include "Core.h"
#include "Input/InputActions.h"


namespace frt::assets
{
struct SAssetValidationResult
{
	bool bOk = false;
	std::string Message;
};

FRT_CORE_API SAssetValidationResult ValidateInputActionMap (const std::filesystem::path& Path);
FRT_CORE_API SAssetValidationResult ValidateInputAction (const std::filesystem::path& Path);
FRT_CORE_API bool CreateInputAction (
	const std::filesystem::path& Path,
	input::EInputActionKind Kind = input::EInputActionKind::Button,
	std::string* OutError = nullptr,
	bool bOverwrite = false);
FRT_CORE_API bool CreateInputActionMap (
	const std::filesystem::path& Path,
	std::string* OutError = nullptr,
	bool bOverwrite = false);
FRT_CORE_API bool DumpInputAction (
	const std::filesystem::path& Path,
	std::ostream& OutStream,
	std::string* OutError = nullptr);
FRT_CORE_API bool DumpInputActionMap (
	const std::filesystem::path& Path,
	std::ostream& OutStream,
	std::string* OutError = nullptr);
}
