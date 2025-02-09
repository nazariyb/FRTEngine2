#pragma once

template<typename T>
struct TGuardValue
{
	TGuardValue() = delete;

	TGuardValue(T& InVariable, T TempValue)
		: Variable(InVariable), OldValue(InVariable)
	{
		Variable = TempValue;
	}

	~TGuardValue()
	{
		Variable = OldValue;
	}

private:
	T& Variable;
	T OldValue;
};

#define FRT_DELETE_COPY_OPS(Type)\
	Type(const Type&) = delete;\
	Type& operator=(const Type&) = delete;

#define FRT_DELETE_MOVE_OPS(Type)\
	Type(Type&&) = delete;\
	Type& operator=(Type&&) = delete;

#define FRT_DELETE_COPY_AND_MOVE_OPS(Type)\
	FRT_DELETE_COPY_OPS(Type)\
	FRT_DELETE_MOVE_OPS(Type)

#define FRT_DEFAULT_COPY_OPS(Type)\
	Type(const Type&) = default;\
	Type& operator=(const Type&) = default;

#define FRT_DEFAULT_MOVE_OPS(Type)\
	Type(Type&&) = default;\
	Type& operator=(Type&&) = default;

#define FRT_DEFAULT_COPY_AND_MOVE_OPS(Type)\
	FRT_DEFAULT_COPY_OPS(Type)\
	FRT_DEFAULT_MOVE_OPS(Type)
