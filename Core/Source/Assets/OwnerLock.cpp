#include "Assets/OwnerLock.h"

#include <chrono>

#include "Platform/Platform.h"
#include "Platform/Process.h"


namespace frt::assets
{
namespace
{
constexpr FStringLiteral PidKey = "Pid";
constexpr FStringLiteral ExeKey = "Exe";
constexpr FStringLiteral StartedKey = "Started";

// The last component of a path written as text, whichever separator it uses.
FStringView GetFileNamePart (FStringView Path)
{
	const uint32 separator = Path.FindLastOf("\\/");
	return separator == FStringView::NotFound ? Path : Path.RightChop(separator + 1u);
}
}


FString SLockOwner::Describe () const
{
	FString text(GetFileNamePart(Executable));
	if (text.IsEmpty())
	{
		text = "a process";
	}
	text.AppendFormat(", pid {}", ProcessId);
	if (!StartTime.IsEmpty())
	{
		// "2026-10-24T09:41:07Z" reads as "2026-10-24 09:41:07 UTC".
		FString shown(StartTime);
		if (shown.Len() == 20u && shown[10u] == 'T' && shown.EndsWith('Z'))
		{
			shown = FString(shown.View().Left(10u)) + " " + FString(shown.View().Mid(11u, 8u)) + " UTC";
		}
		text.AppendFormat(", started {}", shown);
	}
	return text;
}

std::filesystem::path GetOwnerLockPath (const std::filesystem::path& Root)
{
	return Root / "Local" / "Assets" / "Owner.lock";
}

SLockOwner GetThisProcessAsOwner ()
{
	SLockOwner owner;
	owner.ProcessId = platform::GetCurrentProcessId();
	owner.Executable = FString::FromPath(platform::GetExecutablePath());
	owner.StartTime = FormatUtcTime(platform::GetProcessStartTime());
	return owner;
}

FString FormatUtcTime (int64 FileTime)
{
	// File times count 100 ns ticks from 1601-01-01, the system clock seconds from 1970-01-01:
	// 11644473600 seconds apart. Floor division, so a time before 1970 rounds down like after it.
	constexpr int64 TicksPerSecond = 10'000'000;
	constexpr int64 SecondsFrom1601To1970 = 11'644'473'600;
	const int64 seconds = (FileTime >= 0 ? FileTime : FileTime - (TicksPerSecond - 1)) / TicksPerSecond - SecondsFrom1601To1970;
	const std::chrono::sys_seconds time { std::chrono::seconds(seconds) };
	return FString::Format("{:%Y-%m-%dT%H:%M:%SZ}", time);
}

FString FormatOwnerLock (const SLockOwner& Owner)
{
	return FString::Format("{}={}\n{}={}\n{}={}\n", PidKey, Owner.ProcessId, ExeKey, Owner.Executable, StartedKey, Owner.StartTime);
}

std::optional<SLockOwner> ParseOwnerLock (FStringView Text)
{
	SLockOwner owner;
	for (const FStringView line : Text.Lines())
	{
		const SStringSplit keyAndValue = line.SplitFirst('=');
		if (!keyAndValue)
		{
			continue;
		}
		if (keyAndValue.Left == PidKey)
		{
			owner.ProcessId = keyAndValue.Right.ToUInt32().value_or(0u);
		}
		else if (keyAndValue.Left == ExeKey)
		{
			owner.Executable = keyAndValue.Right;
		}
		else if (keyAndValue.Left == StartedKey)
		{
			owner.StartTime = keyAndValue.Right;
		}
	}
	if (owner.ProcessId == 0u)
	{
		return std::nullopt;
	}
	return owner;
}

std::optional<platform::CExclusiveFileLock> TryTakeOwnerLock (const std::filesystem::path& Root, FString* OutOwner, FString* OutError)
{
	FString holder;
	std::optional<platform::CExclusiveFileLock> lock = platform::TryLockFileExclusive(GetOwnerLockPath(Root),
		FormatOwnerLock(GetThisProcessAsOwner()), &holder, OutError);
	if (OutOwner != nullptr)
	{
		OutOwner->Clear();
		if (!lock.has_value() && !holder.IsEmpty())
		{
			const std::optional<SLockOwner> owner = ParseOwnerLock(holder);
			*OutOwner = owner.has_value() ? owner->Describe() : FString(holder.View().TrimStartAndEnd());
			if (OutOwner->IsEmpty())
			{
				*OutOwner = "an unknown process";
			}
		}
	}
	return lock;
}
}
