#pragma once

#include <filesystem>
#include <iosfwd>

#include "Core.h"
#include "Containers/String.h"
#include "Input/InputActions.h"


namespace frt::assets
{
struct SAssetValidationResult
{
	bool bOk = false;
	FString Message;
};

FRT_CORE_API SAssetValidationResult ValidateInputActionMap (const std::filesystem::path& Path);
FRT_CORE_API SAssetValidationResult ValidateInputAction (const std::filesystem::path& Path);
FRT_CORE_API bool CreateInputAction (
	const std::filesystem::path& Path,
	input::EInputActionKind Kind = input::EInputActionKind::Button,
	FString* OutError = nullptr,
	bool bOverwrite = false);
FRT_CORE_API bool CreateInputActionMap (
	const std::filesystem::path& Path,
	FString* OutError = nullptr,
	bool bOverwrite = false);
FRT_CORE_API bool DumpInputAction (
	const std::filesystem::path& Path,
	std::ostream& OutStream,
	FString* OutError = nullptr);
FRT_CORE_API bool DumpInputActionMap (
	const std::filesystem::path& Path,
	std::ostream& OutStream,
	FString* OutError = nullptr);
}
