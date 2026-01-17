#pragma once

#include <new>

#include "Allocator.h"
#include "Asserts.h"
#include "CoreTypes.h"


namespace frt::memory
{
namespace refs
{}


using namespace refs;

/**
 * @note: Work in progress.
 *
 * @brief The core principle is <b>to always understand ownership and lifetime of an object/memory just from its semantics</b>.
 *
 * @details As long as it doesn't introduce extra complexity in usage, and can be deducted from a ref semantic,
 * <b>the following should be guaranteed at any given place of code</b>:
 *	- it should be obvious whether an object is owned and is meant to be freed by this code
 *	- it should be obvious whether a passed object is meant for reading or writing
 *	- it should be obvious whether an object can be nullptr
 *	- it should be obvious whether non-owning ref will be nullified in case of object destruction
 *
 * @param TRefObserver : non-owning, wraps raw ptr, read-only  access. Allows to "observer" an object, hence the name, hence the read-only
 * @param TRefBorrowed : non-owning, wraps raw ptr, read-write access. "Borrows" ownership (kind of)
 * @param TRefIn       : non-owning, wraps raw ptr, read-only  access, can't be null
 * @param TRefInOpt    : non-owning, wraps raw ptr, read-only  access, can   be null
 * @param TRefOut      : non-owning, wraps raw ptr, read-write access, can't be null
 * @param TRefOutOpt   : non-owning, wraps raw ptr, read-write access, can   be null
 * @param TRefInOut    : non-owning, wraps raw ptr, read-write access, can't be null
 * @param TRefWeak     : non-owning, analogue to std::weak_ptr
 * @param TRefShared   : owning, analogue to std::shared_ptr
 * @param TRefUnique   : owning, similar to std::unique_ptr, but has extra cost as it also has control block and allows getting weak ptrs.
 *	This makes RefUnique's usage more clear as one don't have to use raw ptr or to move ownership just to use an object in some function.
 *
 * @todo multithreading support
 */
namespace refs
{
	template <typename T, bool bNullAllowed>
	struct TRefObserver;
	template <typename T, bool bNullAllowed>
	struct TRefBorrowed;

	template <typename T>
	using TRefIn = TRefObserver<T, false>;
	template <typename T>
	using TRefInOpt = TRefObserver<T, true>;
	template <typename T>
	using TRefOut = TRefBorrowed<T, false>;
	template <typename T>
	using TRefOutOpt = TRefBorrowed<T, true>;
	template <typename T>
	using TRefInOut = TRefOut<T>;

	template <typename T>
	struct TRefWeak;
	template <typename T>
	struct TRefShared;
	template <typename T>
	struct TRefUnique;


	template <typename T>
	struct TRefControlBlock
	{
		uint32 StrongRefCount = 1u;
		uint32 WeakRefCount = 1u;

		IAllocator* Allocator = nullptr; // TODO: 

		/**
		*
		*/
#if defined(FRT_STORE_CONTROLED_DATA_IN_PLACE)
	union { std::remove_cv_t<T> Data; };

	T* Ptr() { return reinterpret_cast<T*>(&Data); }
	static constexpr uint32 GetFullSize() { return sizeof(TRefControlBlock); }
#else
		T* Ptr () { return reinterpret_cast<T*>((uint8*)this + sizeof(TRefControlBlock)); }
		static constexpr uint32 GetFullSize () { return sizeof(TRefControlBlock) + sizeof(T); }
#endif

		TRefControlBlock () { new(Ptr()) T; }

		template <typename... Args>
		explicit TRefControlBlock (Args&&... InArgs) { new(Ptr()) T(std::forward<Args>(InArgs)...); }

		TRefControlBlock* CopyStrong ()
		{
			frt_assert(StrongRefCount > 0u);
			++StrongRefCount;
			return this;
		}

		TRefControlBlock* CopyWeak ()
		{
			frt_assert(StrongRefCount > 0u);
			++WeakRefCount;
			return this;
		}

		void DecrementStrong ()
		{
			frt_assert(StrongRefCount > 0u);
			if (--StrongRefCount == 0u)
			{
				Ptr()->~T();
				DecrementWeak();
			}
		}

		void DecrementWeak ()
		{
			frt_assert(WeakRefCount > 0u);
			if (--WeakRefCount == 0u)
			{
				Allocator->DeleteManaged(this);
			}
		}
	};


	/**
	* 
	* @tparam T Type of watched object
	* 
	*/
	template <typename T, bool bNullAllowed>
	struct TRefObserver
	{
		TRefObserver () requires bNullAllowed : Ptr{ nullptr } {}

		template <bool bOtherNullAllowed>
			requires (bNullAllowed == bOtherNullAllowed) || (bNullAllowed && !bOtherNullAllowed)
		TRefObserver (const TRefObserver<T, bOtherNullAllowed>& Other) : Ptr{ Other.Ptr } {}

		TRefObserver (const TRefObserver& Other) = default;

		template <bool bOtherNullAllowed>
			requires bNullAllowed && bOtherNullAllowed
		TRefObserver (TRefObserver&& Other) noexcept
			: Ptr{ Other.Ptr }
		{
			Other.Ptr = nullptr;
			(void)std::move(Other);
		}

		TRefObserver (TRefObserver&& Other) = default;

		template <bool bOtherNullAllowed>
			requires (bNullAllowed == bOtherNullAllowed) || (bNullAllowed && !bOtherNullAllowed)
		TRefObserver& operator= (const TRefObserver<T, bOtherNullAllowed>& Other)
		{
			Ptr = Other.Ptr;
			return *this;
		}

		TRefObserver& operator= (const TRefObserver& Other) = default;

		template <bool bOtherNullAllowed>
			requires bNullAllowed && bOtherNullAllowed
		TRefObserver& operator= (TRefObserver&& Other) noexcept
		{
			Ptr = Other.Ptr;
			Other.Ptr = nullptr;
			(void)std::move(Other);
			return *this;
		}

		TRefObserver& operator= (TRefObserver&& Other) = default;

		~TRefObserver () = default;

		TRefObserver (std::nullptr_t) requires bNullAllowed : Ptr{ nullptr } {}
		TRefObserver (std::nullptr_t) = delete;
		TRefObserver (const T* InPtr) : Ptr{ InPtr } { frt_assert(Ptr || bNullAllowed); }

		TRefObserver& operator= (std::nullptr_t) requires bNullAllowed
		{
			Ptr = nullptr;
			return *this;
		}

		TRefObserver& operator= (std::nullptr_t) = delete;

		TRefObserver& operator= (const T* NewPtr)
		{
			Ptr = NewPtr;
			frt_assert(Ptr || bNullAllowed);
			return *this;
		}

		const T* Get () const { return Ptr; }
		const T* operator-> () const { return Ptr; }
		const T& operator* () const { return *Ptr; }

		explicit constexpr operator bool () const { return !bNullAllowed || Ptr != nullptr; }
		explicit operator const T* () const { return Ptr; }

		void Reset (std::nullptr_t) requires bNullAllowed { Ptr = nullptr; }
		void Reset (std::nullptr_t) = delete;

		void Reset (const T* NewPtr)
		{
			Ptr = NewPtr;
			frt_assert(Ptr || bNullAllowed);
		}

	protected:
		const T* Ptr;
	};


	/**
	* 
	* @tparam T Type of borrowed object
	*/
	template <typename T, bool bNullAllowed>
	struct TRefBorrowed
	{
		TRefBorrowed () requires bNullAllowed : Ptr{ nullptr } {}

		template <bool bOtherNullAllowed>
			requires (bNullAllowed == bOtherNullAllowed) || (bNullAllowed && !bOtherNullAllowed)
		TRefBorrowed (const TRefBorrowed<T, bOtherNullAllowed>& Other) : Ptr{ Other.Ptr } {}

		TRefBorrowed (const TRefBorrowed& Other) = default;

		template <bool bOtherNullAllowed>
			requires bNullAllowed && bOtherNullAllowed
		TRefBorrowed (TRefBorrowed&& Other) noexcept
			: Ptr{ Other.Ptr }
		{
			Other.Ptr = nullptr;
			(void)std::move(Other);
		}

		TRefBorrowed (TRefBorrowed&& Other) = default;

		template <bool bOtherNullAllowed>
			requires (bNullAllowed == bOtherNullAllowed) || (bNullAllowed && !bOtherNullAllowed)
		TRefBorrowed& operator= (const TRefBorrowed<T, bOtherNullAllowed>& Other)
		{
			Ptr = Other.Ptr;
			return *this;
		}

		TRefBorrowed& operator= (const TRefBorrowed& Other) = default;

		template <bool bOtherNullAllowed>
			requires bNullAllowed && bOtherNullAllowed
		TRefBorrowed& operator= (TRefBorrowed&& Other) noexcept
		{
			Ptr = Other.Ptr;
			Other.Ptr = nullptr;
			(void)std::move(Other);
			return *this;
		}

		TRefBorrowed& operator= (TRefBorrowed&& Other) = default;

		~TRefBorrowed () = default;

		TRefBorrowed (std::nullptr_t) requires bNullAllowed : Ptr{ nullptr } {}
		TRefBorrowed (std::nullptr_t) = delete;
		TRefBorrowed (const T* InPtr) : Ptr{ InPtr } { frt_assert(Ptr || bNullAllowed); }

		TRefBorrowed& operator= (std::nullptr_t) requires bNullAllowed
		{
			Ptr = nullptr;
			return *this;
		}

		TRefBorrowed& operator= (std::nullptr_t) = delete;

		TRefBorrowed& operator= (const T* NewPtr)
		{
			Ptr = NewPtr;
			frt_assert(Ptr || bNullAllowed);
			return *this;
		}

		T* Get () { return Ptr; }
		T* operator-> () { return Ptr; }
		T& operator* () { return *Ptr; }

		const T* Get () const { return Ptr; }
		const T* operator-> () const { return Ptr; }
		const T& operator* () const { return *Ptr; }

		explicit constexpr operator bool () const { return !bNullAllowed || Ptr != nullptr; }
		explicit operator const T* () const { return Ptr; }

		void Reset (std::nullptr_t) requires bNullAllowed { Ptr = nullptr; }
		void Reset (std::nullptr_t) = delete;

		void Reset (const T* NewPtr)
		{
			Ptr = NewPtr;
			frt_assert(Ptr || bNullAllowed);
		}

	protected:
		T* Ptr;
	};


	template <typename T>
	struct TRefShared
	{
		TRefShared () = default;

		TRefShared (const TRefShared& Other) { Control = Other.Control->CopyStrong(); }

		TRefShared& operator= (const TRefShared& Other)
		{
			if (this == &Other)
			{
				return *this;
			}
			if (Control)
			{
				Control->DecrementStrong();
			}
			Control = Other.Control->CopyStrong();
			return *this;
		}

		TRefShared (TRefShared&& Other) noexcept { Move(std::move(Other)); }

		TRefShared& operator= (TRefShared&& Other) noexcept
		{
			if (Control)
			{
				Control->DecrementStrong();
			}
			Move(std::move(Other));
			return *this;
		}

		void Move (TRefShared&& Other) noexcept
		{
			Control = Other.Control;
			Other.Control = nullptr;
			(void)std::move(Other);
		}

		~TRefShared () { Release(); }

		TRefShared (TRefControlBlock<T>* InControl) : Control{ InControl } {}

		TRefWeak<T> GetWeak () const { return TRefWeak<T>(Control->CopyWeak()); }
		TRefObserver<T, false> GetObserver () const { return TRefObserver<T, false>(Ptr()); }
		TRefBorrowed<T, false> GetBorrowed () const { return TRefBorrowed<T, false>(Ptr()); }

		T* GetRawIgnoringLifetime () { return Ptr(); }
		T* operator-> () { return Ptr(); }
		T& operator* () { return *Ptr(); }

		const T* GetRawIgnoringLifetime () const { return Ptr(); }
		const T* operator-> () const { return Ptr(); }
		const T& operator* () const { return *Ptr(); }

		explicit constexpr operator bool () const { return Control && Control->StrongRefCount > 0u; }
		explicit operator const T* () const { return Ptr(); }

		void Release ()
		{
			if (Control)
			{
				Control->DecrementStrong();
			}
			Control = nullptr;
		}

		void Reset (const TRefControlBlock<T>* NewPtr = nullptr)
		{
			if (Control)
			{
				Control->DecrementStrong();
			}
			Control = NewPtr;
		}

	protected:
		T* Ptr () { return Control->Ptr(); }
		TRefControlBlock<T>* Control = nullptr;
	};


	template <typename T>
	struct TRefWeak
	{
		TRefWeak () = default;

		TRefWeak (const TRefWeak& Other) : Control{ Other.Control->CopyWeak() } {}

		TRefWeak& operator= (const TRefWeak& Other)
		{
			if (this == &Other)
			{
				return *this;
			}
			if (Control)
			{
				Control->DecrementWeak();
			}
			Control = Other.Control->CopyWeak();
			return *this;
		}

		TRefWeak (TRefWeak&& Other) noexcept { Move(std::move(Other)); }

		TRefWeak& operator= (TRefWeak&& Other) noexcept
		{
			if (Control)
			{
				Control->DecrementWeak();
			}
			Move(std::move(Other));
			return *this;
		}

		void Move (TRefWeak&& Other) noexcept
		{
			Control = Other.Control;
			Other.Control = nullptr;
			(void)std::move(Other);
		}

		~TRefWeak ()
		{
			if (Control)
			{
				Control->DecrementWeak();
			}
		}

		TRefWeak (TRefControlBlock<T>* InControl) : Control{ InControl } {}

		TRefWeak& operator= (TRefControlBlock<T>* InControl)
		{
			if (Control == InControl)
			{
				return *this;
			}
			if (Control)
			{
				Control->DecrementWeak();
			}
			Control = InControl;
			return *this;
		}

		TRefObserver<T, false> GetObserver () const { return TRefObserver<T, false>(Ptr()); }
		TRefBorrowed<T, false> GetBorrowed () const { return TRefBorrowed<T, false>(Ptr()); }

		T* GetRawIgnoringLifetime () { return Ptr(); }
		T* operator-> () { return Ptr(); }
		T& operator* () { return *Ptr(); }

		const T* GetRawIgnoringLifetime () const { return Ptr(); }
		const T* operator-> () const { return Control->Ptr(); }
		const T& operator* () const { return *Ptr(); }

		explicit constexpr operator bool () const { return Ptr() != nullptr; }
		explicit operator const T* () const { return Ptr(); }

		TRefShared<T> Lock () { return TRefShared<T>(Control->CopyStrong()); }

		void Reset (TRefControlBlock<T>* NewControl = nullptr)
		{
			if (Control)
			{
				Control->DecrementWeak();
			}
			Control = NewControl;
		}

	protected:
		T* Ptr () { return Control->Ptr(); }
		const T* Ptr () const { return Control->Ptr(); }
		TRefControlBlock<T>* Control;
	};


	template <typename T>
	struct TRefUnique
	{
		TRefUnique () = default;

		TRefUnique (const TRefUnique& Other) = delete;
		TRefUnique& operator= (const TRefUnique& Other) = delete;

		TRefUnique (TRefUnique&& Other) noexcept { Move(std::move(Other)); }

		TRefUnique& operator= (TRefUnique&& Other) noexcept
		{
			Move(std::move(Other));
			return *this;
		}

		void Move (TRefUnique&& Other) noexcept
		{
			Control = Other.Control;
			Other.Control = nullptr;
			(void)std::move(Other);
		}

		~TRefUnique () { Release(); }

		TRefUnique (TRefControlBlock<T>* InControl) : Control{ InControl } {}

		TRefWeak<T> GetWeak () const { return TRefWeak<T>(Control->CopyWeak()); }
		TRefObserver<T, false> GetObserver () const { return TRefObserver<T, false>(Ptr()); }
		TRefBorrowed<T, false> GetBorrowed () const { return TRefBorrowed<T, false>(Ptr()); }

		T* GetRawIgnoringLifetime () { return Ptr(); }
		T* operator-> () { return Ptr(); }
		T& operator* () { return *Ptr(); }

		const T* GetRawIgnoringLifetime () const { return Ptr(); }
		const T* operator-> () const { return Ptr(); }
		const T& operator* () const { return *Ptr(); }

		explicit constexpr operator bool () const { return Control && Control->StrongRefCount > 0u; }
		explicit operator const T* () const { return Ptr(); }

		void Release ()
		{
			if (Control)
			{
				Control->DecrementStrong();
			}
			Control = nullptr;
		}

		void Reset (const TRefControlBlock<T>* NewPtr = nullptr)
		{
			if (Control)
			{
				Control->DecrementStrong();
			}
			Control = NewPtr;
		}

	protected:
		T* Ptr () { return Control->Ptr(); }
		TRefControlBlock<T>* Control;
	};
}
}
