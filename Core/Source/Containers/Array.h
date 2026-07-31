#pragma once

#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "Math/MathUtility.h"
#include "Memory/Memory.h"


namespace frt
{
/**
* Basic implementation of dynamic array with some small modifications. Key points:
*	- IndexStrategy is used for convenient getters for i-th element counting backwards, or for circular indexing
*	- Until element is added, or unless specified otherwise, no memory is allocated (on the contrary to std::vector)
*	- Compatible with STL
*
* @TODO:
*	- Allocator
*	- Convertion from and to std::vector
*	- Swap functions
*
* @tparam TElementType 
* @tparam TAllocator
*/
template <typename TElementType, typename TAllocator = memory::DefaultPool>
class TArray
{
public:
	using IndexType = int64;

	using IndexStrategy = math::SIndexStrategy;

	TArray ();
	TArray (const TArray& Other);
	TArray (TArray&& Other) noexcept;
	TArray& operator= (const TArray& Other);
	TArray& operator= (TArray&& Other) noexcept;
	~TArray ();

	// constructor with initializer list
	TArray (std::initializer_list<TElementType> InList);
	TArray (const std::vector<TElementType>& InVector);
	TArray& operator= (std::initializer_list<TElementType> InList);
	TArray& operator= (const std::vector<TElementType>& InVector);

	// Allocators
	TArray (uint32 InCapacity);
	void SetCapacity (uint32 InCapacity);

	template <bool bExtendIfNeeded = true>
	uint32 SetSize (uint32 InSize);

	template <bool bExtendIfNeeded = true>
	uint32 SetSize (uint32 InSize, const TElementType& InInitWithValue);

	// TODO: handle InSize < Size
	template <bool bExtendIfNeeded = true>
	uint32 SetSizeUninitialized (uint32 InSize);

	void ShrinkToFit ();

	void ReAlloc (uint64 InCapacity);
	void Free ();
	// ~Allocators

	// Adders
	TElementType& Add ();
	TElementType& Add (const TElementType& InElement);
	TElementType& Add (TElementType&& InElement);
	TElementType& AddUnique (const TElementType& InElement);
	TElementType& AddUnique (TElementType&& InElement);

	template <IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
	void Insert (const TElementType& InElement, IndexType InIndex);

	template <IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
	void Insert (TElementType&& InElement, IndexType InIndex);

	template <typename... Args>
	void InsertEmplace (IndexType InIndex, Args&&... InArgs);

	template <typename... Args>
	TElementType& Emplace (Args... InArgs);

	void Append (const TArray& InArray);
	void Append (TArray&& InArray);
	// ~Adders

	// Removers
	template <bool bKeepOrder = true>
	bool Remove (const TElementType& InElement);

	template <bool bKeepOrder = true>
	uint32 RemoveAll (const TElementType& InElement);

	template <bool bKeepOrder = true, IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
	void RemoveAt (IndexType InIndex);

	/**
	* Destruct all elements, set size to 0, but do not free memory.
	*/
	void Clear ();

	/**
	 * @TODO: add shrink option/version
	 */
	void Reset (uint32 WantedCapacity);
	// ~Removers

	// Getters
	TElementType* GetData ();
	const TElementType* GetData () const;

	uint32 GetSize () const;
	uint32 Count () const;
	uint32 GetCapacity () const;

	bool IsEmpty () const;
	bool Contains (const TElementType& InElement) const;
	IndexType GetMaxIndex () const;

	template <IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
	bool IsIndexValid (IndexType InIndex) const;

	template <IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
	TElementType& Get (IndexType InIndex);

	template <IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
	const TElementType& Get (IndexType InIndex) const;

	TElementType& operator[] (IndexType InIndex);
	const TElementType& operator[] (IndexType InIndex) const;

	uint32 Find (const TElementType& InElement) const;

	TElementType& First ();
	const TElementType& First () const;
	TElementType& Last ();
	const TElementType& Last () const;
	// ~Getters

	// STL compatibility
	using value_type = TElementType;
	using size_type = uint32;
	using difference_type = int64;
	using reference = TElementType&;
	using const_reference = const TElementType&;

	TElementType* begin () { return Data; }
	const TElementType* begin () const { return Data; }
	TElementType* end () { return Data + Size; }
	const TElementType* end () const { return Data + Size; }

	size_type size () const { return Size; }
	bool empty () const { return Size == 0; }
	// ~STL

public:
	static constexpr float GrowthFactor = 1.5f;
	static constexpr uint32 MinAllocation = 2u;

private:
	static uint32 GetGrowCapacity (uint32 CurrentCapacity);

	TElementType* Data;
	uint32 Size;
	uint32 Capacity;
};
}


namespace frt
{
using ArrayIndexStrategy = math::SIndexStrategy;

template <typename ElementType, typename TAllocator>
TArray<ElementType, TAllocator>::TArray ()
	: Data(nullptr)
	, Size(0u)
	, Capacity(0u)
{}

template <typename ElementType, typename TAllocator>
TArray<ElementType, TAllocator>::TArray (const TArray& Other)
	: Data(nullptr)
	, Size(0u)
	, Capacity(0u)
{
	*this = Other;
}

template <typename ElementType, typename TAllocator>
TArray<ElementType, TAllocator>::TArray (TArray&& Other) noexcept
	: Data(Other.Data)
	, Size(Other.Size)
	, Capacity(Other.Capacity)
{
	Other.Data = nullptr;
	Other.Size = 0u;
	Other.Capacity = 0u;
}

template <typename ElementType, typename TAllocator>
TArray<ElementType, TAllocator>& TArray<ElementType, TAllocator>::operator= (const TArray& Other)
{
	if (this == &Other)
	{
		return *this;
	}

	if (Size > 0u)
	{
		Clear();
	}

	// ReAlloc BEFORE adopting Other's count. Clear() has already emptied this array, so
	// relocating must move zero elements; setting Size first told ReAlloc to move Other's
	// worth of elements out of storage that had just been destroyed.
	ReAlloc(Other.Capacity);
	Size = Other.Size;

	for (uint32 i = 0; i < Size; i++)
	{
		new(Data + i) ElementType(*(Other.Data + i));
	}

	return *this;
}

template <typename ElementType, typename TAllocator>
TArray<ElementType, TAllocator>& TArray<ElementType, TAllocator>::operator= (TArray&& Other) noexcept
{
	if (this == &Other)
	{
		return *this;
	}

	// Free, not Clear: Clear destroys the elements but leaves the buffer allocated, and
	// the next line overwrites the only pointer to it.
	Free();

	Data = Other.Data;
	Size = Other.Size;
	Capacity = Other.Capacity;

	Other.Data = nullptr;
	Other.Size = 0u;
	Other.Capacity = 0u;

	return *this;
}

template <typename TElementType, typename TAllocator>
uint32 TArray<TElementType, TAllocator>::GetGrowCapacity (uint32 CurrentCapacity)
{
	return static_cast<uint32>(CurrentCapacity * GrowthFactor);
}

template <typename ElementType, typename TAllocator>
TArray<ElementType, TAllocator>::~TArray ()
{
	Free();
}

template <typename TElementType, typename TAllocator>
TArray<TElementType, TAllocator>::TArray (std::initializer_list<TElementType> InList)
	: Data(nullptr)
	, Size(0u)
	, Capacity(0u)
{
	ReAlloc(InList.size());

	for (const auto& elem : InList)
	{
		Add(elem);
	}
}

template <typename TElementType, typename TAllocator>
TArray<TElementType, TAllocator>::TArray (const std::vector<TElementType>& InVector)
	: Data(nullptr)
	, Size(0u)
	, Capacity(0u)
{
	ReAlloc(InVector.size());

	for (const auto& elem : InVector)
	{
		Add(elem);
	}
}

template <typename TElementType, typename TAllocator>
TArray<TElementType, TAllocator>& TArray<TElementType, TAllocator>::operator= (
	std::initializer_list<TElementType> InList)
{
	Clear();

	ReAlloc(InList.size());

	for (const auto& elem : InList)
	{
		Add(elem);
	}

	return *this;
}

template <typename TElementType, typename TAllocator>
TArray<TElementType, TAllocator>& TArray<TElementType, TAllocator>::operator= (
	const std::vector<TElementType>& InVector)
{
	Clear();

	ReAlloc(InVector.size());

	for (const auto& elem : InVector)
	{
		Add(elem);
	}

	return *this;
}

template <typename ElementType, typename TAllocator>
TArray<ElementType, TAllocator>::TArray (uint32 InCapacity)
	: Data(nullptr)
	, Size(0u)
	, Capacity(InCapacity)
{
	ReAlloc(InCapacity);
}

template <typename ElementType, typename TAllocator>
void TArray<ElementType, TAllocator>::SetCapacity (uint32 InCapacity)
{
	ReAlloc(InCapacity);
}

template <typename ElementType, typename TAllocator>
template <bool bExtendIfNeeded>
uint32 TArray<ElementType, TAllocator>::SetSize (uint32 InSize)
{
	return SetSize<bExtendIfNeeded>(InSize, ElementType());
}

template <typename ElementType, typename TAllocator>
template <bool bExtendIfNeeded>
uint32 TArray<ElementType, TAllocator>::SetSize (uint32 InSize, const ElementType& InInitWithValue)
{
	const uint32 OldSize = Size;
	SetSizeUninitialized<bExtendIfNeeded>(InSize);
	for (uint32 i = OldSize; i < Size; ++i)
	{
		new(Data + i) ElementType(InInitWithValue);
	}

	return Size;
}

template <typename ElementType, typename TAllocator>
template <bool bExtendIfNeeded>
uint32 TArray<ElementType, TAllocator>::SetSizeUninitialized (uint32 InSize)
{
	const bool bExceedsCapacity = InSize > Capacity;
	if (bExceedsCapacity && bExtendIfNeeded)
	{
		ReAlloc(math::Max(InSize, GetGrowCapacity(Capacity) + 1u));
	}

	const uint32 NumToAdd = (!bExceedsCapacity || bExtendIfNeeded) ? InSize - Size : Capacity - Size;
	return Size += NumToAdd;
}

template <typename ElementType, typename TAllocator>
void TArray<ElementType, TAllocator>::ShrinkToFit ()
{
	ReAlloc(Size);
}

template <typename ElementType, typename TAllocator>
void TArray<ElementType, TAllocator>::ReAlloc (uint64 InCapacity)
{
	frt_assert(InCapacity >= Size);
	frt_assert(InCapacity <= (std::numeric_limits<uint32>::max)());

	const uint32 newCapacity = static_cast<uint32>(math::Max(InCapacity, static_cast<uint64>(MinAllocation)));

	if constexpr (std::is_trivially_copyable_v<ElementType>)
	{
		// Let the allocator relocate the block. Raw movement is correct for a trivially
		// copyable type, and the pool may be able to grow the block in place by merging
		// it with a free neighbour instead of copying at all.
		//
		// Still cannot shrink - the allocator decides.
		Data = (ElementType*)TAllocator::GetPrimaryInstance()->ReAllocate(Data, sizeof(ElementType) * newCapacity);
	}
	else
	{
		// A raw copy would leave the old and new objects both owning the same resources,
		// and then destroy neither. Move-construct into fresh storage and destroy the
		// originals instead.
		//
		// The branch is compile-time, so a trivially copyable element pays nothing for
		// this path existing.
		ElementType* newData =
			(ElementType*)TAllocator::GetPrimaryInstance()->Allocate(sizeof(ElementType) * newCapacity);
		frt_assert(newData != nullptr);

		for (uint32 i = 0u; i < Size; ++i)
		{
			new(newData + i) ElementType(std::move(*(Data + i)));
			(Data + i)->~ElementType();
		}

		if (Data != nullptr)
		{
			TAllocator::GetPrimaryInstance()->Free(Data);
		}

		Data = newData;
	}

	Capacity = newCapacity;
}

template <typename ElementType, typename TAllocator>
void TArray<ElementType, TAllocator>::Free ()
{
	if (Size > 0u)
	{
		Clear();
	}

	TAllocator::GetPrimaryInstance()->Free(Data);
	Data = nullptr;
}

template <typename ElementType, typename TAllocator>
ElementType& TArray<ElementType, TAllocator>::Add ()
{
	if (Size == Capacity)
	{
		ReAlloc(GetGrowCapacity(Capacity));
	}

	auto* newElem = new(Data + Size) ElementType;
	++Size;
	return *newElem;
}

template <typename ElementType, typename TAllocator>
ElementType& TArray<ElementType, TAllocator>::Add (const ElementType& InElement)
{
	if (Size == Capacity)
	{
		ReAlloc(GetGrowCapacity(Capacity));
	}

	auto* newElem = new(Data + Size) ElementType(InElement);
	++Size;
	return *newElem;
}

template <typename ElementType, typename TAllocator>
ElementType& TArray<ElementType, TAllocator>::Add (ElementType&& InElement)
{
	if (Size == Capacity)
	{
		ReAlloc(GetGrowCapacity(Capacity));
	}

	auto* newElem = new(Data + Size) ElementType(std::move(InElement));
	++Size;
	return *newElem;
}

template <typename ElementType, typename TAllocator>
ElementType& TArray<ElementType, TAllocator>::AddUnique (const ElementType& InElement)
{
	const uint32 Index = Find(InElement);
	if (Index < Size)
	{
		return *(Data + Index);
	}

	if (Size == Capacity)
	{
		ReAlloc(GetGrowCapacity(Capacity));
	}

	auto* newElem = new(Data + Size) ElementType(InElement);
	++Size;
	return *newElem;
}

template <typename ElementType, typename TAllocator>
ElementType& TArray<ElementType, TAllocator>::AddUnique (ElementType&& InElement)
{
	const uint32 Index = Find(InElement);
	if (Index < Size)
	{
		return *(Data + Index);
	}

	if (Size == Capacity)
	{
		ReAlloc(GetGrowCapacity(Capacity));
	}

	auto* newElem = new(Data + Size) ElementType(std::move(InElement));
	++Size;
	return *newElem;
}

template <typename ElementType, typename TAllocator>
template <ArrayIndexStrategy::EType TIndexType>
void TArray<ElementType, TAllocator>::Insert (const ElementType& InElement, IndexType InIndex)
{
	const bool bAppend = InIndex == Size;
	frt_assert(bAppend || IsIndexValid<TIndexType>(InIndex));
	const uint32 Index = bAppend ? Size : static_cast<uint32>(ArrayIndexStrategy::ConvertToDefault<TIndexType>(InIndex, *this));

	if (Size == Capacity)
	{
		ReAlloc(GetGrowCapacity(Capacity));
	}

	for (uint32 i = Size; i > Index; --i)
	{
		new(Data + i) ElementType(std::move(*(Data + i - 1)));
	}
	++Size;

	new(Data + Index) ElementType(InElement);
}

template <typename ElementType, typename TAllocator>
template <ArrayIndexStrategy::EType TIndexType>
void TArray<ElementType, TAllocator>::Insert (ElementType&& InElement, IndexType InIndex)
{
	const bool bAppend = InIndex == Size;
	frt_assert(bAppend || IsIndexValid<TIndexType>(InIndex));
	const uint32 Index = bAppend ? Size : static_cast<uint32>(ArrayIndexStrategy::ConvertToDefault<TIndexType>(InIndex, *this));

	if (Size == Capacity)
	{
		ReAlloc(GetGrowCapacity(Capacity));
	}

	for (uint32 i = Size; i > Index; --i)
	{
		new(Data + i) ElementType(std::move(*(Data + i - 1)));
	}
	++Size;

	new(Data + Index) ElementType(std::move(InElement));
}

template <typename ElementType, typename TAllocator>
template <typename... Args>
void TArray<ElementType, TAllocator>::InsertEmplace (IndexType InIndex, Args&&... InArgs)
{
	frt_assert(InIndex >= 0 && InIndex <= Size);
	const uint32 Index = static_cast<uint32>(InIndex);

	if (Size == Capacity)
	{
		ReAlloc(GetGrowCapacity(Capacity));
	}

	for (uint32 i = Size; i > Index; --i)
	{
		new(Data + i) ElementType(std::move(*(Data + i - 1)));
	}
	++Size;

	new(Data + Index) ElementType(std::forward<Args>(InArgs)...);
}

template <typename ElementType, typename TAllocator>
template <typename... Args>
ElementType& TArray<ElementType, TAllocator>::Emplace (Args... InArgs)
{
	if (Size == Capacity)
	{
		ReAlloc(GetGrowCapacity(Capacity));
	}

	auto* newElem = new(Data + Size) ElementType(std::forward<Args>(InArgs)...);
	++Size;
	return *newElem;
}

template <typename ElementType, typename TAllocator>
void TArray<ElementType, TAllocator>::Append (const TArray& InArray)
{
	const uint32 NewSize = Size + InArray.Size;
	const uint32 NewCapacity = math::Max(NewSize, GetGrowCapacity(Capacity) + 1u);
	ReAlloc(NewCapacity);

	for (uint32 i = Size; i < NewSize; ++i)
	{
		new(Data + i) ElementType(*(InArray.Data + (i - Size)));
	}

	Size = NewSize;
}

template <typename ElementType, typename TAllocator>
void TArray<ElementType, TAllocator>::Append (TArray&& InArray)
{
	const uint32 NewSize = Size + InArray.Size;
	const uint32 NewCapacity = math::Max(NewSize, GetGrowCapacity(Capacity) + 1u);
	ReAlloc(NewCapacity);

	for (uint32 i = Size; i < NewSize; ++i)
	{
		new(Data + i) ElementType(std::move(*(InArray.Data + i)));
	}

	Size = NewSize;

	InArray.Size = 0u;
	InArray.Free();
	(void)std::move(InArray);
}

template <typename ElementType, typename TAllocator>
template <bool bKeepOrder>
bool TArray<ElementType, TAllocator>::Remove (const ElementType& InElement)
{
	const uint32 Index = Find(InElement);
	if (IsIndexValid(Index))
	{
		RemoveAt<bKeepOrder>(Index);
		return true;
	}

	return false;
}

template <typename ElementType, typename TAllocator>
template <bool bKeepOrder>
uint32 TArray<ElementType, TAllocator>::RemoveAll (const ElementType& InElement)
{
	uint32 RemovedNum = 0u;
	if constexpr (!bKeepOrder)
	{
		uint32 Index = Find(InElement);
		while (IsIndexValid(Index))
		{
			RemoveAt<bKeepOrder>(Index);
			Index = Find(InElement);
			++RemovedNum;
		}
	}
	else
	{
		for (uint32 i = 0u; i < Size; ++i)
		{
			if (*(Data + i) == InElement)
			{
				(Data + i)->~ElementType();
				++RemovedNum;
			}
			else
			{
				new(Data + i - RemovedNum) ElementType(std::move(*(Data + i)));
			}
		}
		Size -= RemovedNum;
	}
	return RemovedNum;
}

template <typename ElementType, typename TAllocator>
template <bool bKeepOrder, ArrayIndexStrategy::EType TIndexType>
void TArray<ElementType, TAllocator>::RemoveAt (IndexType InIndex)
{
	frt_assert(IsIndexValid<TIndexType>(InIndex));
	const uint32 Index = static_cast<uint32>(ArrayIndexStrategy::ConvertToDefault<TIndexType>(InIndex, *this));
	const uint32 LastIndex = Size - 1u;

	// Move-ASSIGN over the live elements, then destroy exactly the slot that ends up
	// vacated. Placement-new over a live object would skip its destructor, and destroying
	// Index up front made the no-keep-order case move out of an object it had just
	// destroyed whenever Index was already the last one. Both are invisible for trivially
	// copyable elements and corrupting for anything that owns a resource.
	if constexpr (bKeepOrder)
	{
		for (uint32 i = Index; i < LastIndex; ++i)
		{
			*(Data + i) = std::move(*(Data + i + 1u));
		}
	}
	else if (Index != LastIndex)
	{
		*(Data + Index) = std::move(*(Data + LastIndex));
	}

	(Data + LastIndex)->~ElementType();
	--Size;
}

template <typename ElementType, typename TAllocator>
void TArray<ElementType, TAllocator>::Clear ()
{
	for (uint32 i = 0u; i < Size; ++i)
	{
		(Data + i)->~ElementType();
	}
	Size = 0u;
}

template <typename TElementType, typename TAllocator>
void TArray<TElementType, TAllocator>::Reset (uint32 WantedCapacity)
{
	Clear();

	if (WantedCapacity > Capacity)
	{
		ReAlloc(WantedCapacity);
	}
}

template <typename ElementType, typename TAllocator>
ElementType* TArray<ElementType, TAllocator>::GetData ()
{
	return Data;
}

template <typename ElementType, typename TAllocator>
const ElementType* TArray<ElementType, TAllocator>::GetData () const
{
	return Data;
}

template <typename ElementType, typename TAllocator>
uint32 TArray<ElementType, TAllocator>::GetSize () const
{
	return Size;
}

template <typename ElementType, typename TAllocator>
uint32 TArray<ElementType, TAllocator>::Count () const
{
	return Size;
}

template <typename ElementType, typename TAllocator>
uint32 TArray<ElementType, TAllocator>::GetCapacity () const
{
	return Capacity;
}

template <typename ElementType, typename TAllocator>
bool TArray<ElementType, TAllocator>::IsEmpty () const
{
	return Size == 0u;
}

template <typename ElementType, typename TAllocator>
bool TArray<ElementType, TAllocator>::Contains (const ElementType& InElement) const
{
	for (uint32 i = 0u; i < Size; ++i)
	{
		if (*(Data + i) == InElement)
		{
			return true;
		}
	}
	return false;
}

template <typename TElementType, typename TAllocator>
typename TArray<TElementType, TAllocator>::IndexType TArray<TElementType, TAllocator>::GetMaxIndex () const
{
	return Size - 1u;
}

template <typename ElementType, typename TAllocator>
template <ArrayIndexStrategy::EType TIndexType>
bool TArray<ElementType, TAllocator>::IsIndexValid (IndexType InIndex) const
{
	return ArrayIndexStrategy::IsValid<TIndexType>(InIndex, *this);
}

template <typename ElementType, typename TAllocator>
template <ArrayIndexStrategy::EType TIndexType>
ElementType& TArray<ElementType, TAllocator>::Get (IndexType InIndex)
{
	return const_cast<ElementType&>(static_cast<const TArray&>(*this).Get<TIndexType>(InIndex));
}

template <typename ElementType, typename TAllocator>
template <ArrayIndexStrategy::EType TIndexType>
const ElementType& TArray<ElementType, TAllocator>::Get (IndexType InIndex) const
{
	frt_assert(IsIndexValid<TIndexType>(InIndex));
	return *(Data + ArrayIndexStrategy::ConvertToDefault<TIndexType>(InIndex, *this));
}

template <typename ElementType, typename TAllocator>
ElementType& TArray<ElementType, TAllocator>::operator[] (IndexType InIndex)
{
	return const_cast<ElementType&>(static_cast<const TArray&>(*this).operator[](InIndex));
}

template <typename ElementType, typename TAllocator>
const ElementType& TArray<ElementType, TAllocator>::operator[] (IndexType InIndex) const
{
	return Get<ArrayIndexStrategy::IS_Default>(InIndex);
}

template <typename ElementType, typename TAllocator>
uint32 TArray<ElementType, TAllocator>::Find (const ElementType& InElement) const
{
	for (uint32 i = 0u; i < Size; ++i)
	{
		if (*(Data + i) == InElement)
		{
			return i;
		}
	}

	return Count();
}

template <typename ElementType, typename TAllocator>
ElementType& TArray<ElementType, TAllocator>::First ()
{
	return const_cast<ElementType&>(static_cast<const TArray&>(*this).First());
}

template <typename ElementType, typename TAllocator>
const ElementType& TArray<ElementType, TAllocator>::First () const
{
	return Get(0u);
}

template <typename ElementType, typename TAllocator>
ElementType& TArray<ElementType, TAllocator>::Last ()
{
	return const_cast<ElementType&>(static_cast<const TArray&>(*this).Last());
}

template <typename ElementType, typename TAllocator>
const ElementType& TArray<ElementType, TAllocator>::Last () const
{
	return Get<ArrayIndexStrategy::IS_Circular>(-1);
}


// Must cover the allocator parameter too. Specializing only TArray<T> left every array
// with a non-default allocator failing the Indexable concept, which broke Get() and
// IsIndexValid() for it - the allocator parameter was effectively unusable.
template <typename T, typename TAllocator>
struct concepts::SIsIndexable<TArray<T, TAllocator>> : std::true_type
{};
}
