#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

#include "Core.h"
#include "CoreTypes.h"


namespace frt
{
template <typename TSignature>
class TUniqueFunction;
}


namespace frt::detail
{
enum class EFunctionOp : uint8
{
	Relocate,   // move-construct the callable at Self from Other's, then destroy Other's
	Destroy,    // destroy the callable at Self
	Target,     // the function a function-pointer callable points to; nullptr for any other callable
	IsOnHeap,   // non-null when the callable lives in a heap block, nullptr when it is inline
};

// One per stored callable type and storage kind, instantiated in the module that built the function.
using FFunctionManager = const void* (*) (EFunctionOp Op, void* Self, void* Other) noexcept;

// As integers: comparing pointers into different objects with < is unspecified.
[[nodiscard]] inline bool IsAddressInImage (const void* Address, const void* ImageBegin, const void* ImageEnd) noexcept
{
	const uintptr_t address = reinterpret_cast<uintptr_t>(Address);
	return address >= reinterpret_cast<uintptr_t>(ImageBegin) && address < reinterpret_cast<uintptr_t>(ImageEnd);
}

template <typename T>
struct TIsUniqueFunction : std::false_type
{};

template <typename TSignature>
struct TIsUniqueFunction<TUniqueFunction<TSignature>> : std::true_type
{};

// A function pointer (what a function name decays to).
template <typename T>
inline constexpr bool IsFunctionPointer = std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>;
}


namespace frt
{
/**
 * An owned callable that may be kept: a completion callback (CStreamableHandle::OnComplete), a
 * delegate's subscription (TMulticastDelegate), a deferred command. Move-only, like C++23's
 * std::move_only_function and UE's TUniqueFunction, so a lambda may capture move-only state (a
 * handle, a TRefUnique) and nothing is ever copied behind the caller's back.
 *
 * Storage. A callable of up to InlineSize bytes (four pointers: a `this` plus a few values, or an
 * FString and a pointer) whose alignment is at most a pointer's and whose move cannot throw lives
 * inside the object; anything else in a block from the CRT heap (new / delete). Never from the
 * engine's memory pool: TLSF is single-threaded and does not exist before the engine starts or after
 * it stops, while functions are built on workers and outlive both ends. The block is allocated and
 * freed by code of the module that built the function (see below), so it always returns to the
 * heap it came from.
 *
 * Modules. Building a TUniqueFunction from a callable instantiates its invoker and manager (move,
 * destroy) in the module where the callable's type is known - for a lambda, the module that wrote
 * it. A function built in a game module therefore cannot even be destroyed once that module is
 * unloaded. GetInvoker exposes the call thunk, and RefersToImage checks the invoker, the manager and
 * a function pointer's target against a module's image, so whoever holds functions (a delegate, the
 * streamable manager) can destroy the module's ones before FreeLibrary, as CComponentRegistry does
 * with component types (spec §6.7 step 4).
 *
 * Calling is const, as with std::function: the callable is called as a non-const lvalue (a mutable
 * lambda keeps its state). Calling an empty function asserts. Not thread-safe: one owner at a time
 * (moving a function to another thread is fine).
 */
template <typename TRet, typename... TArgs>
class TUniqueFunction<TRet (TArgs...)>
{
public:
	static constexpr size_t InlineSize = 4u * sizeof(void*);
	static constexpr size_t InlineAlignment = alignof(void*);

	// Where a callable of type TCallable is stored: inside the object, or in a heap block.
	template <typename TCallable>
	static constexpr bool IsStoredInline = sizeof(TCallable) <= InlineSize && alignof(TCallable) <= InlineAlignment
		&& std::is_nothrow_move_constructible_v<TCallable>;

	TUniqueFunction () noexcept = default;
	TUniqueFunction (std::nullptr_t) noexcept
	{}

	// Any callable invocable with TArgs whose result converts to TRet (anything, for void). A null
	// function pointer gives an empty function.
	template <typename F>
		requires (!detail::TIsUniqueFunction<std::remove_cvref_t<F>>::value
			&& std::is_constructible_v<std::decay_t<F>, F>
			&& std::is_invocable_r_v<TRet, std::decay_t<F>&, TArgs...>)
	TUniqueFunction (F&& Callable)
	{
		using TCallable = std::decay_t<F>;
		if constexpr (std::is_pointer_v<TCallable> || std::is_member_pointer_v<TCallable>)
		{
			if (Callable == nullptr)
			{
				return;
			}
		}

		if constexpr (IsStoredInline<TCallable>)
		{
			::new (static_cast<void*>(Storage)) TCallable(std::forward<F>(Callable));
			Invoker = &InvokeInline<TCallable>;
			Manager = &ManageInline<TCallable>;
		}
		else
		{
			TCallable* block = new TCallable(std::forward<F>(Callable));
			::new (static_cast<void*>(Storage)) TCallable*(block);
			Invoker = &InvokeHeap<TCallable>;
			Manager = &ManageHeap<TCallable>;
		}
	}

	TUniqueFunction (const TUniqueFunction&) = delete;
	TUniqueFunction& operator= (const TUniqueFunction&) = delete;

	TUniqueFunction (TUniqueFunction&& Other) noexcept
	{
		TakeFrom(Other);
	}

	TUniqueFunction& operator= (TUniqueFunction&& Other) noexcept
	{
		if (this != &Other)
		{
			Reset();
			TakeFrom(Other);
		}
		return *this;
	}

	TUniqueFunction& operator= (std::nullptr_t) noexcept
	{
		Reset();
		return *this;
	}

	~TUniqueFunction ()
	{
		Reset();
	}

	TRet operator() (TArgs... Args) const
	{
		frt_assert(Invoker != nullptr)
		return Invoker(const_cast<std::byte*>(Storage), std::forward<TArgs>(Args)...);
	}

	[[nodiscard]] explicit operator bool () const noexcept { return Invoker != nullptr; }
	[[nodiscard]] bool IsSet () const noexcept { return Invoker != nullptr; }
	[[nodiscard]] friend bool operator== (const TUniqueFunction& Function, std::nullptr_t) noexcept
	{
		return Function.Invoker == nullptr;
	}

	// Destroys the callable (through the code of the module that built it); the function is empty
	// afterwards. Safe if the callable's destructor uses this function: it is empty by then.
	void Reset () noexcept
	{
		if (Manager != nullptr)
		{
			const detail::FFunctionManager manager = Manager;
			Invoker = nullptr;
			Manager = nullptr;
			manager(detail::EFunctionOp::Destroy, Storage, nullptr);
		}
	}

	// The call thunk, instantiated in the module that built this function; nullptr when empty.
	[[nodiscard]] const void* GetInvoker () const noexcept
	{
		return reinterpret_cast<const void*>(Invoker);
	}

	// The function a function-pointer callable points to; nullptr for any other callable, and when
	// empty. A pointer to a game function built into a function by engine code has an engine
	// invoker: the target is what lies in the game's image.
	[[nodiscard]] const void* GetTarget () const noexcept
	{
		return Manager != nullptr ? Manager(detail::EFunctionOp::Target, const_cast<std::byte*>(Storage), nullptr) : nullptr;
	}

	// The function would run code of the module whose image is [ImageBegin, ImageEnd): its invoker or
	// manager lies there, or its target does. False when empty. Call it before the module unloads.
	[[nodiscard]] bool RefersToImage (const void* ImageBegin, const void* ImageEnd) const noexcept
	{
		if (Manager == nullptr)
		{
			return false;
		}
		return detail::IsAddressInImage(GetInvoker(), ImageBegin, ImageEnd)
			|| detail::IsAddressInImage(reinterpret_cast<const void*>(Manager), ImageBegin, ImageEnd)
			|| detail::IsAddressInImage(GetTarget(), ImageBegin, ImageEnd);
	}

	// The callable lives inside this object (no heap block). False when empty. For tests and stats.
	[[nodiscard]] bool IsInline () const noexcept
	{
		return Manager != nullptr && Manager(detail::EFunctionOp::IsOnHeap, const_cast<std::byte*>(Storage), nullptr) == nullptr;
	}

private:
	using FInvoker = TRet (*) (void* Storage, TArgs&&...);

	void TakeFrom (TUniqueFunction& Other) noexcept
	{
		if (Other.Manager != nullptr)
		{
			Other.Manager(detail::EFunctionOp::Relocate, Storage, Other.Storage);
			Invoker = Other.Invoker;
			Manager = Other.Manager;
			Other.Invoker = nullptr;
			Other.Manager = nullptr;
		}
	}

	template <typename TCallable>
	static TRet Call (TCallable& Callable, TArgs&&... Args)
	{
		if constexpr (std::is_void_v<TRet>)
		{
			std::invoke(Callable, std::forward<TArgs>(Args)...);
		}
		else
		{
			return std::invoke(Callable, std::forward<TArgs>(Args)...);
		}
	}

	template <typename TCallable>
	static const void* TargetOf (const TCallable& Callable) noexcept
	{
		if constexpr (detail::IsFunctionPointer<TCallable>)
		{
			return reinterpret_cast<const void*>(Callable);
		}
		else
		{
			(void)Callable;
			return nullptr;
		}
	}

	template <typename TCallable>
	static TRet InvokeInline (void* Self, TArgs&&... Args)
	{
		return Call(*std::launder(static_cast<TCallable*>(Self)), std::forward<TArgs>(Args)...);
	}

	template <typename TCallable>
	static TRet InvokeHeap (void* Self, TArgs&&... Args)
	{
		return Call(**std::launder(static_cast<TCallable**>(Self)), std::forward<TArgs>(Args)...);
	}

	// Relocate: Self is raw storage, Other holds the callable. Every other operation: Self holds it.
	template <typename TCallable>
	static const void* ManageInline (detail::EFunctionOp Op, void* Self, void* Other) noexcept
	{
		switch (Op)
		{
		case detail::EFunctionOp::Relocate:
		{
			TCallable* source = std::launder(static_cast<TCallable*>(Other));
			::new (Self) TCallable(std::move(*source));
			source->~TCallable();
			return nullptr;
		}
		case detail::EFunctionOp::Destroy:
			std::launder(static_cast<TCallable*>(Self))->~TCallable();
			return nullptr;
		case detail::EFunctionOp::Target:
			return TargetOf(*std::launder(static_cast<TCallable*>(Self)));
		case detail::EFunctionOp::IsOnHeap:
			return nullptr;
		}
		return nullptr;
	}

	template <typename TCallable>
	static const void* ManageHeap (detail::EFunctionOp Op, void* Self, void* Other) noexcept
	{
		switch (Op)
		{
		case detail::EFunctionOp::Relocate:
			// The block changes owner; the callable itself never moves.
			::new (Self) TCallable*(*std::launder(static_cast<TCallable**>(Other)));
			return nullptr;
		case detail::EFunctionOp::Destroy:
			delete *std::launder(static_cast<TCallable**>(Self));
			return nullptr;
		case detail::EFunctionOp::Target:
			return TargetOf(**std::launder(static_cast<TCallable**>(Self)));
		case detail::EFunctionOp::IsOnHeap:
			return *std::launder(static_cast<TCallable**>(Self));
		}
		return nullptr;
	}

	alignas(InlineAlignment) std::byte Storage[InlineSize] = {};
	FInvoker Invoker = nullptr;
	detail::FFunctionManager Manager = nullptr;
};

// ABI: inline code compiled into every module that builds one (game modules too, once a game-facing
// interface takes one). A change of its layout or storage rule bumps FRT_GAME_API_VERSION
// (Engine/GameModule.h).
static_assert(sizeof(TUniqueFunction<void ()>) == 6u * sizeof(void*));
static_assert(alignof(TUniqueFunction<void ()>) == alignof(void*));
}
