#pragma once

#include <type_traits>
#include <utility>
#include <vector>

#include "Core.h"
#include "CoreTypes.h"
#include "Functional/UniqueFunction.h"


namespace frt
{
/**
 * One subscription to a TMulticastDelegate: what Add returns and Remove takes. Ids come from one
 * process-wide counter, so a handle never names a subscription of another delegate (Remove on the
 * wrong delegate finds nothing). Default-constructed: no subscription.
 */
struct SDelegateHandle
{
	uint64 Id = 0u;

	[[nodiscard]] constexpr bool IsValid () const noexcept { return Id != 0u; }
	constexpr void Reset () noexcept { Id = 0u; }

	[[nodiscard]] friend constexpr bool operator== (SDelegateHandle A, SDelegateHandle B) noexcept = default;
};
}


namespace frt::detail
{
// A new subscription id, never 0, unique in the process: exported, so every module draws from the
// same counter.
[[nodiscard]] FRT_CORE_API uint64 NewDelegateHandleId () noexcept;
}


namespace frt
{
/**
 * An event with any number of subscribers: engine systems and game modules subscribe, the owner
 * broadcasts (UE's multicast delegates, C# events). It replaces CEvent, whose unsubscribe could not
 * compile: subscriptions are named by handles, never by comparing callables.
 *
 * Order. Subscribers are called in the order they were added.
 *
 * Changes during a broadcast (a subscriber reacting by subscribing or unsubscribing, the usual case
 * for one-shot listeners) are safe and deterministic:
 *	- Remove stops the subscription at once: if it has not been called yet in this broadcast, it is
 *	  not. Its callable is destroyed when the outermost broadcast ends, never while it may be running
 *	  (a subscriber may remove itself).
 *	- Add takes effect after the broadcast: the new subscriber is first called by the next one, after
 *	  every earlier subscriber (C# copies its invocation list for the same reason).
 *	- A subscriber may broadcast the same delegate again (nested broadcasts see the same changes).
 * Destroying or moving the delegate itself from one of its subscribers is not allowed.
 *
 * Modules. ReleaseModule drops every subscription that would run code of an unloading module: its
 * callable was built there (a lambda the module wrote, TUniqueFunction::RefersToImage) or its target
 * lies there (a pointer to one of the module's functions, an object bound with Add(Object, Method)
 * in the module's static data). The owner of a delegate that game modules subscribe to calls it before
 * the module's FreeLibrary, as CGameModule::Unload calls CComponentRegistry::ReleaseModule (spec §6.7
 * step 7, through the module-release listeners that arrive with M5); no engine delegate has game
 * subscribers yet.
 *
 * Not thread-safe, by design: main-thread use, like the systems that own delegates. Work done on
 * workers reaches the main thread through a queue the owner drains, and the owner broadcasts there.
 * Callables are stored as TUniqueFunction (CRT heap, never the engine's memory pool), and the lists
 * are std::vector.
 */
template <typename... TArgs>
class TMulticastDelegate
{
	static_assert(!(std::is_rvalue_reference_v<TArgs> || ...),
		"every subscriber receives the same arguments: pass by value or by reference, never as an rvalue reference");

public:
	using FFunction = TUniqueFunction<void (TArgs...)>;

	TMulticastDelegate () = default;
	TMulticastDelegate (const TMulticastDelegate&) = delete;
	TMulticastDelegate& operator= (const TMulticastDelegate&) = delete;
	// Never during a broadcast of either delegate.
	TMulticastDelegate (TMulticastDelegate&&) noexcept = default;
	TMulticastDelegate& operator= (TMulticastDelegate&&) noexcept = default;

	~TMulticastDelegate ()
	{
		frt_assert(BroadcastDepth == 0u)
	}

	// Subscribes any callable invocable with the arguments (its result is ignored). An empty callable
	// (a null function pointer, an empty FFunction) subscribes nothing and returns an invalid handle.
	// An FFunction is stored as it is, so the module that built it stays known to ReleaseModule.
	template <typename F>
		requires (std::is_constructible_v<FFunction, F>)
	SDelegateHandle Add (F&& Callable)
	{
		return AddEntry(FFunction(std::forward<F>(Callable)), nullptr);
	}

	// Subscribes Object->Method. Object must stay alive until the subscription is removed; it is the
	// subscription's target for ReleaseModule.
	template <typename TObject>
	SDelegateHandle Add (TObject* Object, void (TObject::*Method) (TArgs...))
	{
		frt_assert(Object != nullptr && Method != nullptr)
		return AddEntry(FFunction([Object, Method] (TArgs... Args) { (Object->*Method)(std::forward<TArgs>(Args)...); }),
			Object);
	}

	template <typename TObject>
	SDelegateHandle Add (const TObject* Object, void (TObject::*Method) (TArgs...) const)
	{
		frt_assert(Object != nullptr && Method != nullptr)
		return AddEntry(FFunction([Object, Method] (TArgs... Args) { (Object->*Method)(std::forward<TArgs>(Args)...); }),
			Object);
	}

	// Add, for subscribers that live as long as the delegate and never unsubscribe (CEvent's syntax).
	template <typename F>
		requires (std::is_constructible_v<FFunction, F>)
	SDelegateHandle operator+= (F&& Callable)
	{
		return Add(std::forward<F>(Callable));
	}

	// Ends the subscription. False when the handle names no live subscription of this delegate
	// (invalid, already removed, or another delegate's). See the class comment for broadcasts.
	bool Remove (SDelegateHandle Handle)
	{
		if (!Handle.IsValid())
		{
			return false;
		}

		for (size_t i = 0u; i < Entries.size(); ++i)
		{
			if (Entries[i].Id != Handle.Id)
			{
				continue;
			}
			if (BroadcastDepth > 0u)
			{
				// It may be running: compacted, and destroyed, when the outermost broadcast ends.
				Entries[i].Id = 0u;
				++NumRemovedInBroadcast;
				return true;
			}
			// Destroyed after the list is consistent again: its destructor may use this delegate.
			FFunction removed = std::move(Entries[i].Function);
			Entries.erase(Entries.begin() + static_cast<std::ptrdiff_t>(i));
			return true;
		}

		for (size_t i = 0u; i < Pending.size(); ++i)
		{
			if (Pending[i].Id == Handle.Id)
			{
				// Added during this broadcast and never called by it: nothing runs it now.
				FFunction removed = std::move(Pending[i].Function);
				Pending.erase(Pending.begin() + static_cast<std::ptrdiff_t>(i));
				return true;
			}
		}
		return false;
	}

	// Removes every subscription (as Remove does, one by one).
	void Clear ()
	{
		std::vector<SEntry> removed;
		removed.swap(Pending);
		if (BroadcastDepth > 0u)
		{
			for (SEntry& entry : Entries)
			{
				if (entry.Id != 0u)
				{
					entry.Id = 0u;
					++NumRemovedInBroadcast;
				}
			}
			return;
		}
		std::vector<SEntry> entries;
		entries.swap(Entries);
		NumRemovedInBroadcast = 0u;
	}

	[[nodiscard]] bool Contains (SDelegateHandle Handle) const noexcept
	{
		if (!Handle.IsValid())
		{
			return false;
		}
		for (const SEntry& entry : Entries)
		{
			if (entry.Id == Handle.Id)
			{
				return true;
			}
		}
		for (const SEntry& entry : Pending)
		{
			if (entry.Id == Handle.Id)
			{
				return true;
			}
		}
		return false;
	}

	// Live subscriptions, including those added during the current broadcast.
	[[nodiscard]] uint32 GetNum () const noexcept
	{
		return static_cast<uint32>(Entries.size() - NumRemovedInBroadcast + Pending.size());
	}

	[[nodiscard]] bool IsEmpty () const noexcept { return GetNum() == 0u; }

	// Calls every subscriber, in order, with the same arguments (copied once into this call; each
	// subscriber receives them as its own parameter types say).
	void Broadcast (TArgs... Args)
	{
		SBroadcastScope scope(*this);
		// Entries neither grows nor shrinks during a broadcast: adds wait in Pending, removes only
		// clear the id. Indexed, not iterated: a nested broadcast reads the same vector.
		const size_t count = Entries.size();
		for (size_t i = 0u; i < count; ++i)
		{
			if (Entries[i].Id != 0u)
			{
				Entries[i].Function(Args...);
			}
		}
	}

	// A module is about to unload; [ImageBegin, ImageEnd) is its image. Destroys every subscription
	// whose callable was built in it or whose target lies in it (see the class comment), while its
	// code is still loaded, and returns how many. Never during a broadcast.
	uint32 ReleaseModule (const void* ImageBegin, const void* ImageEnd)
	{
		frt_assert(BroadcastDepth == 0u)

		std::vector<SEntry> released;
		const auto inImage = [ImageBegin, ImageEnd] (const SEntry& Entry)
		{
			return Entry.Function.RefersToImage(ImageBegin, ImageEnd)
				|| detail::IsAddressInImage(Entry.Target, ImageBegin, ImageEnd);
		};
		Compact(Entries, released, inImage);
		Compact(Pending, released, inImage);
		// Destroyed here, with the lists consistent: the destructors run the module's code.
		return static_cast<uint32>(released.size());
	}

private:
	struct SEntry
	{
		FFunction Function;
		const void* Target = nullptr;   // the object of Add(Object, Method); a function pointer's is in Function
		uint64 Id = 0u;                 // 0: removed during a broadcast, compacted when it ends
	};

	// Counts nested broadcasts; the outermost one's end applies what they deferred.
	struct SBroadcastScope
	{
		explicit SBroadcastScope (TMulticastDelegate& InDelegate) noexcept
			: Delegate(InDelegate)
		{
			++Delegate.BroadcastDepth;
		}

		~SBroadcastScope ()
		{
			if (--Delegate.BroadcastDepth == 0u)
			{
				Delegate.FinishBroadcast();
			}
		}

		SBroadcastScope (const SBroadcastScope&) = delete;
		SBroadcastScope& operator= (const SBroadcastScope&) = delete;

		TMulticastDelegate& Delegate;
	};

	SDelegateHandle AddEntry (FFunction&& Function, const void* Target)
	{
		if (!Function)
		{
			return {};
		}
		SEntry entry;
		entry.Function = std::move(Function);
		entry.Target = Target;
		entry.Id = detail::NewDelegateHandleId();
		const SDelegateHandle handle{ entry.Id };
		(BroadcastDepth > 0u ? Pending : Entries).push_back(std::move(entry));
		return handle;
	}

	// Moves the entries matching Predicate (in order) out of List into Out, keeping the order of the
	// rest.
	template <typename TPredicate>
	static void Compact (std::vector<SEntry>& List, std::vector<SEntry>& Out, const TPredicate& Predicate)
	{
		size_t kept = 0u;
		for (size_t i = 0u; i < List.size(); ++i)
		{
			if (Predicate(List[i]))
			{
				Out.push_back(std::move(List[i]));
			}
			else
			{
				if (kept != i)
				{
					// Slot `kept` was moved from already: assigning destroys nothing.
					List[kept] = std::move(List[i]);
				}
				++kept;
			}
		}
		List.resize(kept);
	}

	void FinishBroadcast ()
	{
		std::vector<SEntry> removed;
		if (NumRemovedInBroadcast > 0u)
		{
			Compact(Entries, removed, [] (const SEntry& Entry) { return Entry.Id == 0u; });
			NumRemovedInBroadcast = 0u;
		}
		if (!Pending.empty())
		{
			std::vector<SEntry> added;
			added.swap(Pending);
			for (SEntry& entry : added)
			{
				Entries.push_back(std::move(entry));
			}
		}
		// The removed callables are destroyed last, with the lists consistent.
	}

	std::vector<SEntry> Entries;   // in Add order
	std::vector<SEntry> Pending;   // added during the current broadcast, in Add order
	uint32 BroadcastDepth = 0u;
	uint32 NumRemovedInBroadcast = 0u;   // entries of Entries with Id 0
};
}
