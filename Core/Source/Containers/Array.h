#pragma once

#include "Memory/Memory.h"
#include "Math/MathUtility.h"

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

		TArray();
		TArray(const TArray& Other);
		TArray(TArray&& Other) noexcept;
		TArray& operator=(const TArray& Other);
		TArray& operator=(TArray&& Other) noexcept;
		~TArray();

		// constructor with initializer list
		TArray(std::initializer_list<TElementType> InList);
		TArray(const std::vector<TElementType>& InVector);
		TArray& operator=(std::initializer_list<TElementType> InList);
		TArray& operator=(const std::vector<TElementType>& InVector);

		// Allocators
		TArray(uint32 InCapacity);
		void SetCapacity(uint32 InCapacity);

		template <bool bExtendIfNeeded = true>
		uint32 SetSize(uint32 InSize);

		template <bool bExtendIfNeeded = true>
		uint32 SetSize(uint32 InSize, const TElementType& InInitWithValue);

		// TODO: handle InSize < Size
		template <bool bExtendIfNeeded = true>
		uint32 SetSizeUninitialized(uint32 InSize);

		void ShrinkToFit();

		void ReAlloc(uint32 InCapacity);
		void Free();
		// ~Allocators

		// Adders
		TElementType& Add();
		TElementType& Add(const TElementType& InElement);
		TElementType& Add(TElementType&& InElement);
		TElementType& AddUnique(const TElementType& InElement);
		TElementType& AddUnique(TElementType&& InElement);

		template <IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
		void Insert(const TElementType& InElement, IndexType InIndex);

		template <IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
		void Insert(TElementType&& InElement, IndexType InIndex);

		template <typename... Args>
		void InsertEmplace(IndexType InIndex, Args&&... InArgs);

		template <typename... Args>
		TElementType& Emplace(Args... InArgs);

		void Append(const TArray& InArray);
		void Append(TArray&& InArray);
		// ~Adders

		// Removers
		template <bool bKeepOrder = true>
		bool Remove(const TElementType& InElement);

		template <bool bKeepOrder = true>
		uint32 RemoveAll(const TElementType& InElement);

		template <bool bKeepOrder = true, IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
		void RemoveAt(IndexType InIndex);

		/**
		 * Destruct all elements, set size to 0, but do not free memory.
		 */
		void Clear();
		// ~Removers

		// Getters
		TElementType* GetData();
		const TElementType* GetData() const;

		uint32 GetSize() const;
		uint32 Count() const;
		uint32 GetCapacity() const;

		bool IsEmpty() const;
		bool Contains(const TElementType& InElement) const;
		IndexType GetMaxIndex() const;

		template <IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
		bool IsIndexValid(IndexType InIndex) const;

		template <IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
		TElementType& Get(IndexType InIndex);

		template <IndexStrategy::EType TIndexType = IndexStrategy::IS_Default>
		const TElementType& Get(IndexType InIndex) const;

		TElementType& operator[](IndexType InIndex);
		const TElementType& operator[](IndexType InIndex) const;

		uint32 Find(const TElementType& InElement) const;

		TElementType& First();
		const TElementType& First() const;
		TElementType& Last();
		const TElementType& Last() const;
		// ~Getters

		// STL compatability
		using value_type = TElementType;
		using size_type = uint32;
		using difference_type = int64;
		using reference = TElementType&;
		using const_reference = const TElementType&;

		TElementType* begin() { return Data; }
		const TElementType* begin() const { return Data; }
		TElementType* end() { return Data + Size; }
		const TElementType* end() const { return Data + Size; }

		size_type size() const { return Size; }
		bool empty() const { return Size == 0; }
		// ~STL

	public:
		static constexpr float GrowthFactor = 1.5f;
		static constexpr uint32 MinAllocation = 2u;

	private:
		TElementType* Data;
		uint32 Size;
		uint32 Capacity;
	};
}


namespace frt
{
	using ArrayIndexStrategy = math::SIndexStrategy;

	template <typename ElementType, typename TAllocator>
	TArray<ElementType, TAllocator>::TArray()
		: Data(nullptr), Size(0u), Capacity(0u)
	{}

	template <typename ElementType, typename TAllocator>
	TArray<ElementType, TAllocator>::TArray(const TArray& Other)
		: Data(nullptr), Size(Other.Size), Capacity(Other.Capacity)
	{
		*this = Other;
	}

	template <typename ElementType, typename TAllocator>
	TArray<ElementType, TAllocator>::TArray(TArray&& Other) noexcept
		: Data(Other.Data), Size(Other.Size), Capacity(Other.Capacity)
	{
		Other.Data = nullptr;
		Other.Size = 0u;
		Other.Capacity = 0u;
	}

	template <typename ElementType, typename TAllocator>
	TArray<ElementType, TAllocator>& TArray<ElementType, TAllocator>::operator=(const TArray& Other)
	{
		if (this == &Other)
		{
			return *this;
		}

		Clear();

		Size = Other.Size;
		ReAlloc(Other.Capacity);

		for (uint32 i = 0; i < Size; i++)
		{
			*(Data + i) = *(Other.Data + i);
		}

		return *this;
	}

	template <typename ElementType, typename TAllocator>
	TArray<ElementType, TAllocator>& TArray<ElementType, TAllocator>::operator=(TArray&& Other) noexcept
	{
		Clear();

		Data = Other.Data;
		Size = Other.Size;
		Capacity = Other.Capacity;

		Other.Data = nullptr;
		Other.Size = 0u;
		Other.Capacity = 0u;

		return *this;
	}

	template <typename ElementType, typename TAllocator>
	TArray<ElementType, TAllocator>::~TArray()
	{
		Free();
	}

	template <typename TElementType, typename TAllocator>
	TArray<TElementType, TAllocator>::TArray(std::initializer_list<TElementType> InList)
	{
		Capacity = InList.size();
		ReAlloc(Capacity);

		for (const auto& elem : InList)
		{
			Add(elem);
		}
	}

	template <typename TElementType, typename TAllocator>
	TArray<TElementType, TAllocator>::TArray(const std::vector<TElementType>& InVector)
	{
		Capacity = InVector.size();
		ReAlloc(Capacity);

		for (const auto& elem : InVector)
		{
			Add(elem);
		}
	}

	template <typename TElementType, typename TAllocator>
	TArray<TElementType, TAllocator>& TArray<TElementType, TAllocator>::operator=(
		std::initializer_list<TElementType> InList)
	{
		Clear();

		Capacity = InList.size();
		ReAlloc(Capacity);

		for (const auto& elem : InList)
		{
			Add(elem);
		}

		return *this;
	}

	template <typename TElementType, typename TAllocator>
	TArray<TElementType, TAllocator>& TArray<TElementType, TAllocator>::operator=(
		const std::vector<TElementType>& InVector)
	{
		Clear();

		Capacity = InVector.size();
		ReAlloc(Capacity);

		for (const auto& elem : InVector)
		{
			Add(elem);
		}

		return *this;
	}

	template <typename ElementType, typename TAllocator>
	TArray<ElementType, TAllocator>::TArray(uint32 InCapacity)
		: Data(nullptr), Size(0u), Capacity(InCapacity)
	{
		ReAlloc(InCapacity);
	}

	template <typename ElementType, typename TAllocator>
	void TArray<ElementType, TAllocator>::SetCapacity(uint32 InCapacity)
	{
		ReAlloc(InCapacity);
	}

	template <typename ElementType, typename TAllocator>
	template <bool bExtendIfNeeded>
	uint32 TArray<ElementType, TAllocator>::SetSize(uint32 InSize)
	{
		return SetSize<bExtendIfNeeded>(InSize, ElementType());
	}

	template <typename ElementType, typename TAllocator>
	template <bool bExtendIfNeeded>
	uint32 TArray<ElementType, TAllocator>::SetSize(uint32 InSize, const ElementType& InInitWithValue)
	{
		const uint32 OldSize = Size;
		SetSizeUninitialized<bExtendIfNeeded>(InSize);
		for (uint32 i = OldSize; i < Size; ++i)
		{
			*(Data + i) = InInitWithValue;
		}

		return Size;
	}

	template <typename ElementType, typename TAllocator>
	template <bool bExtendIfNeeded>
	uint32 TArray<ElementType, TAllocator>::SetSizeUninitialized(uint32 InSize)
	{
		const bool bExceedsCapacity = InSize > Capacity;
		if (bExceedsCapacity && bExtendIfNeeded)
		{
			ReAlloc(math::Max(InSize, (uint32)(Capacity * GrowthFactor) + 1u));
		}

		const uint32 NumToAdd = (!bExceedsCapacity || bExtendIfNeeded) ? InSize - Size : Capacity - Size;
		return Size += NumToAdd;
	}

	template <typename ElementType, typename TAllocator>
	void TArray<ElementType, TAllocator>::ShrinkToFit()
	{
		ReAlloc(Size);
	}

	template <typename ElementType, typename TAllocator>
	void TArray<ElementType, TAllocator>::ReAlloc(uint32 InCapacity)
	{
		frt_assert(InCapacity >= Size);

		// As a quick first implementation, we rely on allocator/pool to realloc.
		// It does the job, and, in fact, it may avoid reallocation and just merge our block with the next one if it's available.
		// This potentially gives some efficiency, but it has its flaws:
		// * We can't shrink
		// * Move-constructors aren't called which may be or not be a problem depending on the type

		Capacity = math::Max(InCapacity, MinAllocation);
		Data = (ElementType*)TAllocator::GetPrimaryInstance()->ReAllocate(Data, sizeof(ElementType) * Capacity);
	}

	template <typename ElementType, typename TAllocator>
	void TArray<ElementType, TAllocator>::Free()
	{
		if (Size > 0u)
		{
			Clear();
		}

		TAllocator::GetPrimaryInstance()->Free(Data);
		Data = nullptr;
	}

	template <typename ElementType, typename TAllocator>
	ElementType& TArray<ElementType, TAllocator>::Add()
	{
		if (Size == Capacity)
		{
			ReAlloc(Capacity * GrowthFactor);
		}

		auto* newElem = new (Data + Size) ElementType;
		++Size;
		return *newElem;
	}

	template <typename ElementType, typename TAllocator>
	ElementType& TArray<ElementType, TAllocator>::Add(const ElementType& InElement)
	{
		if (Size == Capacity)
		{
			ReAlloc(Capacity * GrowthFactor);
		}

		++Size;
		new (Data + Size - 1u) ElementType(InElement);
		return *(Data + Size - 1u);
	}

	template <typename ElementType, typename TAllocator>
	ElementType& TArray<ElementType, TAllocator>::Add(ElementType&& InElement)
	{
		if (Size == Capacity)
		{
			ReAlloc(Capacity * GrowthFactor);
		}

		auto* newElem = new (Data + Size) ElementType(std::move(InElement));
		++Size;
		return *newElem;
	}

	template <typename ElementType, typename TAllocator>
	ElementType& TArray<ElementType, TAllocator>::AddUnique(const ElementType& InElement)
	{
		const uint32 Index = Find(InElement);
		if (Index < Size)
		{
			return *(Data + Index);
		}

		if (Size == Capacity)
		{
			ReAlloc(Capacity * GrowthFactor);
		}

		auto* newElem = new (Data + Size) ElementType(InElement);
		++Size;
		return *newElem;
	}

	template <typename ElementType, typename TAllocator>
	ElementType& TArray<ElementType, TAllocator>::AddUnique(ElementType&& InElement)
	{
		const int32 Index = Find(InElement);
		if (Index < Size)
		{
			return *(Data + Index);
		}

		if (Size == Capacity)
		{
			ReAlloc(Capacity * GrowthFactor);
		}

		auto* newElem = new (Data + Size) ElementType(std::move(InElement));
		++Size;
		return *newElem;
	}

	template <typename ElementType, typename TAllocator>
	template <ArrayIndexStrategy::EType TIndexType = ArrayIndexStrategy::IS_Default>
	void TArray<ElementType, TAllocator>::Insert(const ElementType& InElement, IndexType InIndex)
	{
		frt_assert(IsIndexValid<TIndexType>(InIndex));

		if (Size == Capacity)
		{
			ReAlloc(Capacity * GrowthFactor);
		}

		for (uint32 i = Size; i > InIndex; --i)
		{
			*(Data + i) = std::move(Data + i - 1);
		}
		++Size;

		new (Data + InIndex) ElementType(InElement);
	}

	template <typename ElementType, typename TAllocator>
	template <ArrayIndexStrategy::EType TIndexType = ArrayIndexStrategy::IS_Default>
	void TArray<ElementType, TAllocator>::Insert(ElementType&& InElement, IndexType InIndex)
	{
		frt_assert(IsIndexValid<TIndexType>(InIndex));

		if (Size == Capacity)
		{
			ReAlloc(Capacity * GrowthFactor);
		}

		for (uint32 i = Size; i > InIndex; --i)
		{
			*(Data + i) = std::move(*(Data + i - 1));
		}
		++Size;

		new (Data + InIndex) ElementType(std::move(InElement));
	}

	template <typename ElementType, typename TAllocator>
	template <typename ... Args>
	void TArray<ElementType, TAllocator>::InsertEmplace(IndexType InIndex, Args&&... InArgs)
	{
		frt_assert(InIndex < Size);

		if (Size == Capacity)
		{
			ReAlloc(Capacity * GrowthFactor);
		}

		for (uint32 i = Size; i > InIndex; --i)
		{
			*(Data + i) = std::move(*(Data + i - 1));
		}
		++Size;

		new (Data + InIndex) ElementType(std::forward<Args>(InArgs)...);
	}

	template <typename ElementType, typename TAllocator>
	template <typename ... Args>
	ElementType& TArray<ElementType, TAllocator>::Emplace(Args... InArgs)
	{
		if (Size == Capacity)
		{
			ReAlloc(Capacity * GrowthFactor);
		}

		++Size;
		new (Data + Size) ElementType(std::forward<Args>(InArgs)...);
		return *(Data + Size);
	}

	template <typename ElementType, typename TAllocator>
	void TArray<ElementType, TAllocator>::Append(const TArray& InArray)
	{
		const uint32 NewSize = Size + InArray.Size;
		const uint32 NewCapacity = math::Max(NewSize, (uint32)(Capacity * GrowthFactor) + 1u);
		ReAlloc(NewCapacity);

		for (uint32 i = Size; i < NewSize; ++i)
		{
			new (Data + i) ElementType(*(InArray.Data + (i - Size)));
		}

		Size = NewSize;
	}

	template <typename ElementType, typename TAllocator>
	void TArray<ElementType, TAllocator>::Append(TArray&& InArray)
	{
		const uint32 NewSize = Size + InArray.Size;
		const uint32 NewCapacity = math::Max(NewSize, (uint32)(Capacity * GrowthFactor) + 1u);
		ReAlloc(NewCapacity);

		for (uint32 i = Size; i < NewSize; ++i)
		{
			new (Data + i) ElementType(std::move(*(InArray.Data + i)));
		}

		Size = NewSize;

		InArray.Size = 0u;
		InArray.Free();
		(void)std::move(InArray);
	}

	template <typename ElementType, typename TAllocator>
	template <bool bKeepOrder>
	bool TArray<ElementType, TAllocator>::Remove(const ElementType& InElement)
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
	uint32 TArray<ElementType, TAllocator>::RemoveAll(const ElementType& InElement)
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
					new (Data + i - RemovedNum) ElementType(std::move(*(Data + i)));
				}
			}
			Size -= RemovedNum;
		}
		return RemovedNum;
	}

	template <typename ElementType, typename TAllocator>
	template <bool bKeepOrder, ArrayIndexStrategy::EType TIndexType>
	void TArray<ElementType, TAllocator>::RemoveAt(IndexType InIndex)
	{
		frt_assert(IsIndexValid(InIndex));

		(Data + InIndex)->~ElementType();

		if constexpr (bKeepOrder)
		{
			for (uint32 i = InIndex + 1u; i < Size; ++i)
			{
				new (Data + i - 1u) ElementType(std::move(*(Data + i)));
			}
		}
		else
		{
			new (Data + InIndex) ElementType(std::move(*(Data + Size - 1u)));
		}

		--Size;
	}

	template <typename ElementType, typename TAllocator>
	void TArray<ElementType, TAllocator>::Clear()
	{
		for (uint32 i = 0u; i < Size; ++i)
		{
			(Data + i)->~ElementType();
		}
		Size = 0u;
	}

	template <typename ElementType, typename TAllocator>
	ElementType* TArray<ElementType, TAllocator>::GetData()
	{
		return Data;
	}

	template <typename ElementType, typename TAllocator>
	const ElementType* TArray<ElementType, TAllocator>::GetData() const
	{
		return Data;
	}

	template <typename ElementType, typename TAllocator>
	uint32 TArray<ElementType, TAllocator>::GetSize() const
	{
		return Size;
	}

	template <typename ElementType, typename TAllocator>
	uint32 TArray<ElementType, TAllocator>::Count() const
	{
		return Size;
	}

	template <typename ElementType, typename TAllocator>
	uint32 TArray<ElementType, TAllocator>::GetCapacity() const
	{
		return Capacity;
	}

	template <typename ElementType, typename TAllocator>
	bool TArray<ElementType, TAllocator>::IsEmpty() const
	{
		return Size == 0u;
	}

	template <typename ElementType, typename TAllocator>
	bool TArray<ElementType, TAllocator>::Contains(const ElementType& InElement) const
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
	typename TArray<TElementType, TAllocator>::IndexType TArray<TElementType, TAllocator>::GetMaxIndex() const
	{
		return Size - 1u;
	}

	template <typename ElementType, typename TAllocator>
	template <ArrayIndexStrategy::EType TIndexType = ArrayIndexStrategy::IS_Default>
	bool TArray<ElementType, TAllocator>::IsIndexValid(IndexType InIndex) const
	{
		return ArrayIndexStrategy::IsValid<TIndexType>(InIndex, *this);
	}

	template <typename ElementType, typename TAllocator>
	template <ArrayIndexStrategy::EType TIndexType = ArrayIndexStrategy::IS_Default>
	ElementType& TArray<ElementType, TAllocator>::Get(IndexType InIndex)
	{
		return const_cast<ElementType&>(static_cast<const TArray&>(*this).Get<TIndexType>(InIndex));
	}

	template <typename ElementType, typename TAllocator>
	template <ArrayIndexStrategy::EType TIndexType = ArrayIndexStrategy::IS_Default>
	const ElementType& TArray<ElementType, TAllocator>::Get(IndexType InIndex) const
	{
		frt_assert(IsIndexValid<TIndexType>(InIndex));
		return *(Data + ArrayIndexStrategy::ConvertToDefault<TIndexType>(InIndex, *this));
	}

	template <typename ElementType, typename TAllocator>
	ElementType& TArray<ElementType, TAllocator>::operator[](IndexType InIndex)
	{
		return const_cast<ElementType&>(static_cast<const TArray&>(*this).operator[](InIndex));
	}

	template <typename ElementType, typename TAllocator>
	const ElementType& TArray<ElementType, TAllocator>::operator[](IndexType InIndex) const
	{
		return Get<ArrayIndexStrategy::IS_Default>(InIndex);
	}

	template <typename ElementType, typename TAllocator>
	uint32 TArray<ElementType, TAllocator>::Find(const ElementType& InElement) const
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
	ElementType& TArray<ElementType, TAllocator>::First()
	{
		return const_cast<ElementType&>(static_cast<const TArray&>(*this).First());
	}

	template <typename ElementType, typename TAllocator>
	const ElementType& TArray<ElementType, TAllocator>::First() const
	{
		return Get(0u);
	}

	template <typename ElementType, typename TAllocator>
	ElementType& TArray<ElementType, TAllocator>::Last()
	{
		return const_cast<ElementType&>(static_cast<const TArray&>(*this).Last());
	}

	template <typename ElementType, typename TAllocator>
	const ElementType& TArray<ElementType, TAllocator>::Last() const
	{
		return Get<ArrayIndexStrategy::IS_Circular>(-1);
	}

	template <typename T>
	struct concepts::SIsIndexable<TArray<T>> : std::true_type {};
}
