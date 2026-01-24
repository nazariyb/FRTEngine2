#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "Assets/AssetTool.h"


namespace
{
enum class EAssetType
{
	InputAction,
	InputActionMap
};

std::string NormalizeToken (std::string_view Value)
{
	std::string normalized;
	normalized.reserve(Value.size());
	for (char c : Value)
	{
		if (c == '_' || c == '-' || c == ' ')
		{
			continue;
		}
		if (c >= 'A' && c <= 'Z')
		{
			c = static_cast<char>(c - 'A' + 'a');
		}
		normalized.push_back(c);
	}
	return normalized;
}

bool IsHelp (std::string_view Value)
{
	const std::string normalized = NormalizeToken(Value);
	return normalized == "help" || normalized == "h";
}

bool TryParseAssetType (std::string_view Value, EAssetType* OutType)
{
	const std::string normalized = NormalizeToken(Value);
	if (normalized == "inputaction" || normalized == "action")
	{
		*OutType = EAssetType::InputAction;
		return true;
	}

	if (normalized == "inputactions"
		|| normalized == "inputactionmap"
		|| normalized == "actionmap")
	{
		*OutType = EAssetType::InputActionMap;
		return true;
	}

	return false;
}

bool TryParseActionKind (std::string_view Value, frt::input::EInputActionKind* OutKind)
{
	const std::string normalized = NormalizeToken(Value);
	if (normalized == "button")
	{
		*OutKind = frt::input::EInputActionKind::Button;
		return true;
	}
	if (normalized == "axis")
	{
		*OutKind = frt::input::EInputActionKind::Axis;
		return true;
	}
	return false;
}

std::filesystem::path ResolveInputPath (EAssetType Type, const std::filesystem::path& InputPath)
{
	if (InputPath.is_absolute() || InputPath.has_parent_path())
	{
		std::filesystem::path resolved = InputPath;
		if (!resolved.has_extension())
		{
			resolved.replace_extension(
				Type == EAssetType::InputAction ? ".frtinputaction" : ".frtinputmap");
		}
		return resolved;
	}

	std::filesystem::path resolved = std::filesystem::path("Core") / "Content" / "Input" / InputPath;
	if (!resolved.has_extension())
	{
		resolved.replace_extension(
			Type == EAssetType::InputAction ? ".frtinputaction" : ".frtinputmap");
	}
	return resolved;
}

void PrintUsage ()
{
	std::cout
		<< "AssetTool\n"
		<< "Usage:\n"
		<< "  AssetTool validate <type> <path>\n"
		<< "  AssetTool dump <type> <path>\n"
		<< "  AssetTool create <type> <path> [--force] [--type Button|Axis]\n"
		<< "\n"
		<< "Types:\n"
		<< "  input_action\n"
		<< "  input_action_map\n"
		<< "\n"
		<< "Notes:\n"
		<< "  If <path> has no directory, Core/Content/Input is used.\n";
}
}


int main (int argc, char** argv)
{
	if (argc < 2 || IsHelp(argv[1]))
	{
		PrintUsage();
		return 1;
	}

	const std::string command = NormalizeToken(argv[1]);
	if (command != "validate" && command != "dump" && command != "create")
	{
		std::cerr << "Unknown command: " << argv[1] << "\n";
		PrintUsage();
		return 1;
	}

	if (argc < 4)
	{
		std::cerr << "Missing arguments for command: " << argv[1] << "\n";
		PrintUsage();
		return 1;
	}

	EAssetType assetType = EAssetType::InputActionMap;
	if (!TryParseAssetType(argv[2], &assetType))
	{
		std::cerr << "Unknown asset type: " << argv[2] << "\n";
		PrintUsage();
		return 1;
	}

	const std::filesystem::path path = ResolveInputPath(assetType, argv[3]);

	if (command == "validate")
	{
		frt::assets::SAssetValidationResult result;
		if (assetType == EAssetType::InputAction)
		{
			result = frt::assets::ValidateInputAction(path);
		}
		else
		{
			result = frt::assets::ValidateInputActionMap(path);
		}

		if (!result.bOk)
		{
			std::cerr << result.Message << "\n";
			return 2;
		}

		std::cout << "OK\n";
		return 0;
	}

	if (command == "create")
	{
		bool overwrite = false;
		frt::input::EInputActionKind kind = frt::input::EInputActionKind::Button;

		if (argc > 4)
		{
			for (int argIndex = 4; argIndex < argc; ++argIndex)
			{
				const std::string option = argv[argIndex];
				const std::string normalized = NormalizeToken(option);
				if (normalized == "force" || normalized == "f")
				{
					overwrite = true;
					continue;
				}

				if (normalized == "type")
				{
					if (argIndex + 1 >= argc)
					{
						std::cerr << "Missing value for --type\n";
						PrintUsage();
						return 1;
					}

					if (!TryParseActionKind(argv[argIndex + 1], &kind))
					{
						std::cerr << "Unknown action type: " << argv[argIndex + 1] << "\n";
						PrintUsage();
						return 1;
					}

					++argIndex;
					continue;
				}

				const std::string_view typePrefix = "--type=";
				if (option.rfind(typePrefix, 0) == 0)
				{
					const std::string value = option.substr(typePrefix.size());
					if (!TryParseActionKind(value, &kind))
					{
						std::cerr << "Unknown action type: " << value << "\n";
						PrintUsage();
						return 1;
					}
					continue;
				}

				std::cerr << "Unknown option: " << option << "\n";
				PrintUsage();
				return 1;
			}
		}

		std::string error;
		if (assetType == EAssetType::InputAction)
		{
			if (!frt::assets::CreateInputAction(path, kind, &error, overwrite))
			{
				std::cerr << error << "\n";
				return 2;
			}
		}
		else if (!frt::assets::CreateInputActionMap(path, &error, overwrite))
		{
			std::cerr << error << "\n";
			return 2;
		}

		std::cout << "Created " << path.string() << "\n";
		return 0;
	}

	std::string error;
	if (assetType == EAssetType::InputAction)
	{
		if (!frt::assets::DumpInputAction(path, std::cout, &error))
		{
			std::cerr << error << "\n";
			return 2;
		}
	}
	else if (!frt::assets::DumpInputActionMap(path, std::cout, &error))
	{
		std::cerr << error << "\n";
		return 2;
	}

	return 0;
}
