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
