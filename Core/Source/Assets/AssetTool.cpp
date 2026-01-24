#include "Assets/AssetTool.h"

#include <ostream>

#include "Input/InputActions.h"


namespace frt::assets
{
SAssetValidationResult ValidateInputActionMap (const std::filesystem::path& Path)
{
	SAssetValidationResult result;

	std::error_code ec;
	if (!std::filesystem::exists(Path, ec))
	{
		result.Message = "Input action map not found: " + Path.string();
		return result;
	}

	input::CInputActionMap map;
	if (!map.LoadFromFile(Path))
	{
		result.Message = "Failed to parse input action map: " + Path.string();
		return result;
	}

	result.bOk = true;
	return result;
}

SAssetValidationResult ValidateInputAction (const std::filesystem::path& Path)
{
	SAssetValidationResult result;

	std::error_code ec;
	if (!std::filesystem::exists(Path, ec))
	{
		result.Message = "Input action not found: " + Path.string();
		return result;
	}

	input::SInputActionDefinition action;
	if (!input::LoadInputActionDefinition(Path, &action))
	{
		result.Message = "Failed to parse input action: " + Path.string();
		return result;
	}

	result.bOk = true;
	return result;
}

bool CreateInputAction (
	const std::filesystem::path& Path,
	input::EInputActionKind Kind,
	std::string* OutError,
	bool bOverwrite)
{
	std::error_code ec;
	if (std::filesystem::exists(Path, ec))
	{
		if (!bOverwrite)
		{
			if (OutError)
			{
				*OutError = "Input action already exists: " + Path.string();
			}
			return false;
		}
	}

	if (Path.has_parent_path())
	{
		std::filesystem::create_directories(Path.parent_path(), ec);
	}

	input::SInputActionDefinition action;
	action.Name = Path.stem().string();
	action.Kind = Kind;

	if (!input::SaveInputActionDefinition(Path, action))
	{
		if (OutError)
		{
			*OutError = "Failed to write input action: " + Path.string();
		}
		return false;
	}

	return true;
}

bool CreateInputActionMap (
	const std::filesystem::path& Path,
	std::string* OutError,
	bool bOverwrite)
{
	std::error_code ec;
	if (std::filesystem::exists(Path, ec))
	{
		if (!bOverwrite)
		{
			if (OutError)
			{
				*OutError = "Input action map already exists: " + Path.string();
			}
			return false;
		}
	}

	if (Path.has_parent_path())
	{
		std::filesystem::create_directories(Path.parent_path(), ec);
	}

	input::CInputActionMap map;
	if (!map.SaveToFile(Path))
	{
		if (OutError)
		{
			*OutError = "Failed to write input action map: " + Path.string();
		}
		return false;
	}

	return true;
}

bool DumpInputAction (
	const std::filesystem::path& Path,
	std::ostream& OutStream,
	std::string* OutError)
{
	if (!OutStream.good())
	{
		if (OutError)
		{
			*OutError = "Output stream is not writable.";
		}
		return false;
	}

	input::SInputActionDefinition action;
	if (!input::LoadInputActionDefinition(Path, &action))
	{
		if (OutError)
		{
			*OutError = "Failed to parse input action: " + Path.string();
		}
		return false;
	}

	if (!input::SaveInputActionDefinitionToStream(OutStream, action))
	{
		if (OutError)
		{
			*OutError = "Failed to write input action output.";
		}
		return false;
	}

	return true;
}

bool DumpInputActionMap (
	const std::filesystem::path& Path,
	std::ostream& OutStream,
	std::string* OutError)
{
	if (!OutStream.good())
	{
		if (OutError)
		{
			*OutError = "Output stream is not writable.";
		}
		return false;
	}

	input::CInputActionMap map;
	if (!map.LoadFromFile(Path))
	{
		if (OutError)
		{
			*OutError = "Failed to parse input action map: " + Path.string();
		}
		return false;
	}

	if (!map.SaveToStream(OutStream))
	{
		if (OutError)
		{
			*OutError = "Failed to write input action map output.";
		}
		return false;
	}

	return true;
}
}
