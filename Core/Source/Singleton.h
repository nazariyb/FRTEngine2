#pragma once

#include "Core.h"


#define FRT_SINGLETON_GETTERS(Type)\
	static Type& GetInstance()\
	{\
		return *dynamic_cast<Type*>(_instance);\
	}\
	static Type* GetInstancePtr()\
	{\
		return dynamic_cast<Type*>(_instance);\
	}

#define FRT_SINGLETON_DEFINE_INSTANCE(Type)\
	Type* Singleton<Type>::_instance(nullptr);


NAMESPACE_FRT_START

template<class T>
class FRT_CORE_API Singleton
{
public:
	Singleton(const Singleton&) = delete;
	Singleton& operator=(const Singleton&) = delete;

	Singleton()
	{
		// static_assert(_instance == nullptr, "Singleton already exists");
		_instance = static_cast<T*>(this);
	}

	~Singleton()
	{
		// assert(_instance != nullptr, "Singleton doesn't exist");
		_instance = nullptr;
	}

	FRT_SINGLETON_GETTERS(T)

protected:
	static T* _instance;
};

NAMESPACE_FRT_END
