#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "Core.h"
#include "Containers/StringFwd.h"


namespace frt
{
template <typename TSignature>
class TFunctionRef;

/**
 * A callable passed down a call and not kept: a visitor (ForEachDiagnostic), a predicate (PumpUntil),
 * a comparison. Two pointers, no allocation, no ownership (llvm::function_ref, UE's TFunctionRef;
 * Vittorio Romeo, "passing functions to functions").
 *
 * Why not std::function: it owns a copy of the callable, may allocate for it (from whichever heap
 * the module's CRT uses), and needs the callable to be copyable. A parameter that only calls its
 * argument before returning needs none of that, and a template parameter instead would put the
 * callee's body in every caller.
 *
 * It refers to the callable, so the callable must outlive it. As a parameter that holds by
 * construction: a lambda written in the call lives until the end of the full expression. Never
 * store one in a member or return one; keep those as TUniqueFunction (Functional/UniqueFunction.h).
 * Function pointers and function names are stored by value, so `TFunctionRef<int (int)> F = &Abs;`
 * does not dangle. A ref always refers to something: there is no empty state.
 *
 * Trivially copy-constructible; copies refer to the same callable. Not assignable: a ref names one
 * callable for its whole life (see operator=). Calling it calls the callable as a non-const lvalue,
 * so a mutable lambda passed in keeps its state changes.
 */
template <typename TRet, typename... TArgs>
class TFunctionRef<TRet (TArgs...)>
{
public:
	template <typename F>
		requires (!std::is_same_v<std::remove_cvref_t<F>, TFunctionRef>
			&& std::is_invocable_r_v<TRet, std::remove_reference_t<F>&, TArgs...>)
	TFunctionRef (F&& Callable FRT_LIFETIMEBOUND) noexcept
	{
		using TCallable = std::remove_reference_t<F>;
		if constexpr (std::is_function_v<TCallable>)
		{
			// A function name: its address is the callable, valid for the whole run.
			BindFunction(&Callable);
		}
		else if constexpr (std::is_pointer_v<std::remove_cv_t<TCallable>>
			&& std::is_function_v<std::remove_pointer_t<std::remove_cv_t<TCallable>>>)
		{
			// A function pointer: copied, so a temporary `&Function` does not dangle.
			frt_assert(Callable != nullptr)
			BindFunction(Callable);
		}
		else
		{
			Target.Object = const_cast<void*>(static_cast<const void*>(std::addressof(Callable)));
			Invoker = &InvokeObject<TCallable>;
		}
	}

	TFunctionRef (const TFunctionRef&) noexcept = default;
	// No assignment, as in UE: `Ref = [&] { ... };` would build a ref to the lambda temporary, copy
	// it in and leave Ref pointing at an object destroyed at the end of the statement, and the
	// lifetimebound check on the constructor does not see through the assignment.
	TFunctionRef& operator= (const TFunctionRef&) = delete;

	TRet operator() (TArgs... Args) const
	{
		return Invoker(Target, std::forward<TArgs>(Args)...);
	}

private:
	// A function pointer is not convertible to void* in standard C++: each kind has its member.
	union UTarget
	{
		void* Object;
		void (*Function) ();
	};

	using InvokerFunction = TRet (*) (UTarget, TArgs&&...);

	template <typename TFunction>
	void BindFunction (TFunction* Function) noexcept
	{
		Target.Function = reinterpret_cast<void (*) ()>(Function);
		Invoker = &InvokeFunction<TFunction>;
	}

	template <typename TCallable>
	static TRet InvokeObject (UTarget Bound, TArgs&&... Args)
	{
		TCallable& callable = *static_cast<TCallable*>(Bound.Object);
		if constexpr (std::is_void_v<TRet>)
		{
			std::invoke(callable, std::forward<TArgs>(Args)...);
		}
		else
		{
			return std::invoke(callable, std::forward<TArgs>(Args)...);
		}
	}

	template <typename TFunction>
	static TRet InvokeFunction (UTarget Bound, TArgs&&... Args)
	{
		TFunction* function = reinterpret_cast<TFunction*>(Bound.Function);
		if constexpr (std::is_void_v<TRet>)
		{
			std::invoke(function, std::forward<TArgs>(Args)...);
		}
		else
		{
			return std::invoke(function, std::forward<TArgs>(Args)...);
		}
	}

	UTarget Target;
	InvokerFunction Invoker;
};

// ABI: inline code compiled into every module that names it (game modules too, once a game-facing
// interface takes one). A change of its layout bumps FRT_GAME_API_VERSION (Engine/GameModule.h).
static_assert(sizeof(TFunctionRef<void ()>) == 2u * sizeof(void*));
static_assert(std::is_trivially_copy_constructible_v<TFunctionRef<void ()>>);
static_assert(std::is_trivially_destructible_v<TFunctionRef<void ()>>);
static_assert(!std::is_copy_assignable_v<TFunctionRef<void ()>>);
}
