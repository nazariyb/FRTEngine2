#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "Assets/Vcs/VcsProvider.h"
#include "Containers/String.h"
#include "Containers/StringMap.h"
#include "Platform/FileMove.h"
#include "Platform/FileSystem.h"


namespace frt::test
{
/**
 * The fake every command is tested against (design §15 I26): it records each call as one entry, so a test can say that an
 * asset and its meta went through in the same CheckOut, Add, Move or Delete, and it refuses on demand, so a test can say
 * that nothing was written after a refusal. It spawns nothing and needs no repository. Move and Delete do what they say on
 * the disk, since a writer goes on to rely on it; the others touch nothing. CheckOut refuses a read-only file with ReadOnly
 * naming it, as both real providers do (VcsFileOps.h's CheckOutFiles), so a test sees the refusal a host would; a test of
 * something else can turn that off (SetRefuseReadOnly). Safe from a worker.
 */
class CRecordingVcsProvider final : public assets::IVcsProvider
{
public:
	struct SCall
	{
		FString Name;                 // "CheckOut", "Add", "Move", "Delete", "GetCommittedFacts", "StatusOf"
		std::vector<FString> Files;   // Move: From, To, From, To...
	};

	bool Detect (const FString&) override { return true; }
	bool IsBusy () const override { const std::lock_guard lock(Mutex); return bBusy; }
	bool IsRewritingTree () const override { const std::lock_guard lock(Mutex); return bRewriting; }

	assets::SVcsResult CheckOut (std::span<const FString> Files) override
	{
		assets::SVcsResult result = Record("CheckOut", Files);
		if (result.Error != assets::EVcsError::None || !bRefuseReadOnly)
		{
			return result;
		}
		FString readOnly;
		for (const FString& file : Files)
		{
			const std::optional<platform::SFileStat> stat = platform::Stat(file.ToPath());
			if (stat.has_value() && stat->bReadOnly && !stat->bDirectory)
			{
				readOnly.AppendFormat("{}'{}'", readOnly.IsEmpty() ? "" : ", ", file);
			}
		}
		if (!readOnly.IsEmpty())
		{
			return { assets::EVcsError::ReadOnly, FString::Format("read-only, so not written: {}", readOnly) };
		}
		return result;
	}

	assets::SVcsResult Add (std::span<const FString> Files) override
	{
		return Record("Add", Files);
	}

	assets::SVcsResult Move (std::span<const assets::SVcsMove> Moves) override
	{
		std::vector<FString> files;
		for (const assets::SVcsMove& move : Moves)
		{
			files.push_back(move.From);
			files.push_back(move.To);
		}
		assets::SVcsResult result = Record("Move", files);
		if (result.Error != assets::EVcsError::None)
		{
			return result;
		}
		for (const assets::SVcsMove& move : Moves)
		{
			std::error_code ec;
			std::filesystem::create_directories(move.To.ToPath().parent_path(), ec);
			FString error;
			if (move.From != move.To && !platform::MoveFileNoReplace(move.From.ToPath(), move.To.ToPath(), &error))
			{
				return { assets::EVcsError::Failed, error };
			}
		}
		return {};
	}

	assets::SVcsResult Delete (std::span<const FString> Files) override
	{
		assets::SVcsResult result = Record("Delete", Files);
		if (result.Error == assets::EVcsError::None)
		{
			for (const FString& file : Files)
			{
				std::error_code ec;
				std::filesystem::remove(file.ToPath(), ec);
			}
		}
		return result;
	}

	assets::SVcsResult GetCommittedFacts (std::span<const FString> Files, std::vector<bool>& OutCommitted) override
	{
		assets::SVcsResult result = Record("GetCommittedFacts", Files);
		const std::lock_guard lock(Mutex);
		OutCommitted.clear();
		for (const FString& file : Files)
		{
			OutCommitted.push_back(Committed.contains(file));
		}
		return result;
	}

	assets::SVcsResult StatusOf (std::span<const FString> Files, std::vector<assets::SVcsStatus>& Out) override
	{
		assets::SVcsResult result = Record("StatusOf", Files);
		const std::lock_guard lock(Mutex);
		Out.clear();
		for (const FString& file : Files)
		{
			const auto found = Statuses.find(file);
			Out.push_back(found != Statuses.end() ? found->second : assets::SVcsStatus {});
		}
		return result;
	}

	assets::SVcsResult GetStaged (std::vector<FString>& OutFiles) override
	{
		const std::lock_guard lock(Mutex);
		OutFiles = Staged;
		return {};
	}

	[[nodiscard]] FStringView GetName () const override { return "Recording"; }

	// The next and every later call named Name fails with Error; EVcsError::None takes the refusal back.
	void Refuse (FStringView Name, assets::EVcsError Error, FStringView Message = "refused by the test")
	{
		const std::lock_guard lock(Mutex);
		Refusals.insert_or_assign(FString(Name), assets::SVcsResult { Error, FString(Message) });
	}

	void SetBusy (bool bInBusy, bool bInRewriting = false)
	{
		const std::lock_guard lock(Mutex);
		bBusy = bInBusy;
		bRewriting = bInRewriting;
	}

	// False: CheckOut takes a read-only file as a VCS that opens it for edit would (and leaves its attribute alone).
	void SetRefuseReadOnly (bool bRefuse)
	{
		const std::lock_guard lock(Mutex);
		bRefuseReadOnly = bRefuse;
	}

	void SetCommitted (std::span<const FString> Files)
	{
		const std::lock_guard lock(Mutex);
		Committed.clear();
		Committed.insert(Files.begin(), Files.end());
	}

	void SetStatus (const FString& File, assets::SVcsStatus Status)
	{
		const std::lock_guard lock(Mutex);
		Statuses.insert_or_assign(File, std::move(Status));
	}

	void SetStaged (std::vector<FString> Files)
	{
		const std::lock_guard lock(Mutex);
		Staged = std::move(Files);
	}

	[[nodiscard]] std::vector<SCall> GetCalls () const
	{
		const std::lock_guard lock(Mutex);
		return Calls;
	}

	// The calls that write: what "asset and meta in one call" and "nothing after a refusal" are about.
	[[nodiscard]] std::vector<SCall> GetWriteCalls () const
	{
		std::vector<SCall> writes;
		for (SCall& call : GetCalls())
		{
			if (call.Name == "CheckOut" || call.Name == "Add" || call.Name == "Move" || call.Name == "Delete")
			{
				writes.push_back(std::move(call));
			}
		}
		return writes;
	}

	void ClearCalls ()
	{
		const std::lock_guard lock(Mutex);
		Calls.clear();
	}

private:
	assets::SVcsResult Record (FStringView Name, std::span<const FString> Files)
	{
		const std::lock_guard lock(Mutex);
		Calls.push_back(SCall { FString(Name), std::vector<FString>(Files.begin(), Files.end()) });
		const auto refusal = Refusals.find(Name);
		return refusal != Refusals.end() ? refusal->second : assets::SVcsResult {};
	}

	mutable std::mutex Mutex;
	std::vector<SCall> Calls;
	TStringMap<assets::SVcsResult> Refusals;
	FStringSet Committed;
	TStringMap<assets::SVcsStatus> Statuses;
	std::vector<FString> Staged;
	bool bBusy = false;
	bool bRewriting = false;
	bool bRefuseReadOnly = true;
};
}
