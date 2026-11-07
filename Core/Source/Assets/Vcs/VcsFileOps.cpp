#include "Assets/Vcs/VcsFileOps.h"

#include <filesystem>
#include <optional>
#include <system_error>

#include "Log/Log.h"
#include "Platform/FileMove.h"
#include "Platform/FileSystem.h"
#include "Unicode/DisplayText.h"


namespace frt::assets
{
SVcsResult CheckOutFiles (std::span<const FString> Files)
{
	FString readOnly;
	for (const FString& file : Files)
	{
		const std::optional<platform::SFileStat> stat = platform::Stat(file.ToPath());
		if (stat.has_value() && stat->bReadOnly && !stat->bDirectory)
		{
			readOnly.AppendFormat("{}'{}'", readOnly.IsEmpty() ? "" : ", ", unicode::ToDisplayText(file));
		}
	}
	if (readOnly.IsEmpty())
	{
		return {};
	}
	return { EVcsError::ReadOnly, FString::Format("read-only, so not written: {}", readOnly) };
}

SVcsResult TrashFiles (std::span<const FString> Files)
{
	for (const FString& file : Files)
	{
		const std::filesystem::path path = file.ToPath();
		FString error;
		if (platform::Stat(path).has_value() && !platform::MoveToTrash(path, &error))
		{
			return { EVcsError::Failed,
				FString::Format("cannot move '{}' to the Recycle Bin: {}", unicode::ToDisplayText(file), error) };
		}
	}
	return {};
}

bool IsCaseOnlyRename (const SVcsMove& Move)
{
	const FString from = FString::FromPath(platform::NormalizePath(Move.From.ToPath()), EPathStyle::Generic);
	const FString to = FString::FromPath(platform::NormalizePath(Move.To.ToPath()), EPathStyle::Generic);
	return from != to && from.FoldCase() == to.FoldCase();
}

SVcsResult MoveFilePlain (const SVcsMove& Move)
{
	const std::filesystem::path to = Move.To.ToPath();
	std::error_code ec;
	std::filesystem::create_directories(to.parent_path(), ec);   // a failure here is the move's failure below, with its reason
	FString error;
	if (!platform::MoveFileNoReplace(Move.From.ToPath(), to, &error))
	{
		return { EVcsError::Failed, unicode::ToDisplayText(error) };
	}
	return {};
}

SVcsResult RunMoves (std::span<const SVcsMove> Moves, TFunctionRef<SVcsResult (const SVcsMove&)> MoveOne)
{
	for (const SVcsMove& move : Moves)
	{
		if (move.From == move.To)
		{
			continue;
		}
		const std::optional<platform::SFileStat> from = platform::Stat(move.From.ToPath());
		if (!from.has_value())
		{
			return { EVcsError::Failed, FString::Format("cannot move '{}': it does not exist", unicode::ToDisplayText(move.From)) };
		}
		// The same file in another spelling is a case-only rename, not an occupied target.
		const std::optional<platform::SFileStat> to = platform::Stat(move.To.ToPath());
		if (to.has_value() && !(to->Identity == from->Identity))
		{
			return { EVcsError::Failed,
				FString::Format("cannot move '{}': '{}' exists", unicode::ToDisplayText(move.From), unicode::ToDisplayText(move.To)) };
		}
	}

	for (size_t moved = 0u; moved < Moves.size(); ++moved)
	{
		if (Moves[moved].From == Moves[moved].To)
		{
			continue;
		}
		SVcsResult result = MoveOne(Moves[moved]);
		if (result.Error == EVcsError::None)
		{
			continue;
		}
		for (size_t undo = moved; undo-- > 0u;)
		{
			if (Moves[undo].From == Moves[undo].To)
			{
				continue;
			}
			const SVcsResult back = MoveOne(SVcsMove { Moves[undo].To, Moves[undo].From });
			if (back.Error != EVcsError::None)
			{
				log::Error(log::Assets, "A move failed, and '{}' could not go back to '{}': {}", unicode::ToDisplayText(Moves[undo].To),
					unicode::ToDisplayText(Moves[undo].From), back.Message);
				result.Message.AppendFormat("; '{}' stays at '{}'", unicode::ToDisplayText(Moves[undo].From),
					unicode::ToDisplayText(Moves[undo].To));
			}
		}
		return result;
	}
	return {};
}
}
