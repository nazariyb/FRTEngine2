#include "Log/Log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "Platform/Platform.h"


namespace frt::log
{
namespace
{
struct SCategoryLevel
{
	char Name[MaxCategoryLength + 1u] = {};
	uint32 NameLength = 0u;
	ELevel MinLevel = ELevel::Info;
};

/**
 * Everything the log keeps, in fixed storage: the per-category levels are a short table searched
 * linearly (a handful of categories ever get a level of their own).
 *
 * Created on first use and never destroyed: a static initializer may log before anything else is
 * constructed, and an atexit handler or a static destructor after everything else is gone.
 */
struct SLogState
{
	std::mutex Mutex;
	SCategoryLevel Levels[MaxCategoryLevels];
	uint32 NumLevels = 0u;
	ELevel DefaultMinLevel = ELevel::Info;
	HookFunction Hook = nullptr;
	void* HookContext = nullptr;
	// The most verbose minimum of all: anything more verbose is dropped without the lock.
	std::atomic<uint8> MostVerboseMinLevel = static_cast<uint8>(ELevel::Info);
};

SLogState& GetState () noexcept
{
	static SLogState* const state = new SLogState();
	return *state;
}

FStringView ClampCategory (FStringView Category) noexcept
{
	// Truncate: never inside a code point.
	return Category.Len() <= MaxCategoryLength ? Category : Category.Truncate(MaxCategoryLength);
}

// Under the mutex.
SCategoryLevel* FindLevel (SLogState& State, FStringView Category) noexcept
{
	const FStringView name = ClampCategory(Category);
	for (uint32 i = 0u; i < State.NumLevels; ++i)
	{
		SCategoryLevel& level = State.Levels[i];
		if (FStringView(level.Name, level.NameLength) == name)
		{
			return &level;
		}
	}
	return nullptr;
}

// Under the mutex.
ELevel GetMinLevelLocked (SLogState& State, FStringView Category) noexcept
{
	const SCategoryLevel* level = FindLevel(State, Category);
	return level != nullptr ? level->MinLevel : State.DefaultMinLevel;
}

// Under the mutex, after every change of a level.
void UpdateMostVerbose (SLogState& State) noexcept
{
	ELevel mostVerbose = State.DefaultMinLevel;
	for (uint32 i = 0u; i < State.NumLevels; ++i)
	{
		if (State.Levels[i].MinLevel > mostVerbose)
		{
			mostVerbose = State.Levels[i].MinLevel;
		}
	}
	State.MostVerboseMinLevel.store(static_cast<uint8>(mostVerbose), std::memory_order_relaxed);
}

bool PassesQuickCheck (const SLogState& State, ELevel Level) noexcept
{
	return static_cast<uint8>(Level) <= State.MostVerboseMinLevel.load(std::memory_order_relaxed);
}
}


FStringLiteral GetLevelName (ELevel Level) noexcept
{
	switch (Level)
	{
	case ELevel::Error:
		return "error";
	case ELevel::Warning:
		return "warning";
	case ELevel::Info:
		return "info";
	case ELevel::Verbose:
		return "verbose";
	}
	return "unknown";
}

std::optional<ELevel> ParseLevel (FStringView Name) noexcept
{
	constexpr ELevel levels[] = { ELevel::Error, ELevel::Warning, ELevel::Info, ELevel::Verbose };
	for (const ELevel level : levels)
	{
		if (Name.Equals<ESearchCase::IgnoreCaseAscii>(GetLevelName(level)))
		{
			return level;
		}
	}
	return std::nullopt;
}

bool SetMinLevel (FStringView Category, ELevel Level) noexcept
{
	SLogState& state = GetState();
	const std::scoped_lock lock(state.Mutex);
	SCategoryLevel* level = FindLevel(state, Category);
	if (level == nullptr)
	{
		if (state.NumLevels == MaxCategoryLevels)
		{
			return false;
		}
		const FStringView name = ClampCategory(Category);
		level = &state.Levels[state.NumLevels++];
		std::memcpy(level->Name, name.GetData(), name.Len());
		level->Name[name.Len()] = '\0';
		level->NameLength = name.Len();
	}
	level->MinLevel = Level;
	UpdateMostVerbose(state);
	return true;
}

void ResetMinLevel (FStringView Category) noexcept
{
	SLogState& state = GetState();
	const std::scoped_lock lock(state.Mutex);
	SCategoryLevel* level = FindLevel(state, Category);
	if (level == nullptr)
	{
		return;
	}
	// Order does not matter: the last entry fills the gap.
	*level = state.Levels[state.NumLevels - 1u];
	state.Levels[--state.NumLevels] = SCategoryLevel();
	UpdateMostVerbose(state);
}

void SetDefaultMinLevel (ELevel Level) noexcept
{
	SLogState& state = GetState();
	const std::scoped_lock lock(state.Mutex);
	state.DefaultMinLevel = Level;
	UpdateMostVerbose(state);
}

void ResetAllMinLevels () noexcept
{
	SLogState& state = GetState();
	const std::scoped_lock lock(state.Mutex);
	for (uint32 i = 0u; i < state.NumLevels; ++i)
	{
		state.Levels[i] = SCategoryLevel();
	}
	state.NumLevels = 0u;
	state.DefaultMinLevel = ELevel::Info;
	UpdateMostVerbose(state);
}

ELevel GetMinLevel (FStringView Category) noexcept
{
	SLogState& state = GetState();
	const std::scoped_lock lock(state.Mutex);
	return GetMinLevelLocked(state, Category);
}

bool IsEnabled (FStringView Category, ELevel Level) noexcept
{
	SLogState& state = GetState();
	if (!PassesQuickCheck(state, Level))
	{
		return false;
	}
	const std::scoped_lock lock(state.Mutex);
	return Level <= GetMinLevelLocked(state, Category);
}

void Write (FStringView Category, ELevel Level, FStringView Message) noexcept
{
	SLogState& state = GetState();
	if (!PassesQuickCheck(state, Level))
	{
		return;
	}

	// Built before the lock: the formatting is the slow part, and FString allocates from the CRT.
	FString line;
	line.Reserve(Category.Len() + Message.Len() + 4u);
	line.AppendChar('[').Append(Category).Append("] ").Append(Message).AppendChar('\n');

	const std::scoped_lock lock(state.Mutex);
	if (Level > GetMinLevelLocked(state, Category))
	{
		return;
	}
	if (state.Hook != nullptr)
	{
		SMessage message;
		message.Category = Category;
		message.Level = Level;
		message.Text = Message;
		message.Line = line;
		state.Hook(state.HookContext, message);
		return;
	}
	std::fwrite(line.GetData(), 1u, line.Len(), stderr);
	platform::OutputDebugText(line);
}

HookFunction SetHook (HookFunction Hook, void* Context, void** OutPreviousContext) noexcept
{
	SLogState& state = GetState();
	const std::scoped_lock lock(state.Mutex);
	const HookFunction previous = state.Hook;
	if (OutPreviousContext != nullptr)
	{
		*OutPreviousContext = state.HookContext;
	}
	state.Hook = Hook;
	state.HookContext = Hook != nullptr ? Context : nullptr;
	return previous;
}
}
