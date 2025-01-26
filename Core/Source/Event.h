#pragma once

#include <functional>
#include <vector>

namespace frt
{
	template<typename ...TArgs>
	class CEvent
	{
	public:
		using FSubscriber = std::function<void(TArgs...)>;

		void Subscribe(const FSubscriber& Subscriber);
		void UnSubscribe(const FSubscriber& Subscriber);

		void operator+=(const FSubscriber& Subscriber);
		void operator-=(const FSubscriber& Subscriber);

		void Invoke(TArgs... Args);

	private:
		std::vector<FSubscriber> Subscribers;
	};


	template <typename ... TArgs>
	void CEvent<TArgs...>::Subscribe(const FSubscriber& Subscriber)
	{
		Subscribers.push_back(Subscriber);
	}

	template <typename ... TArgs>
	void CEvent<TArgs...>::UnSubscribe(const FSubscriber& Subscriber)
	{
		const auto it = std::find(Subscribers.begin(), Subscribers.end(), Subscriber);
		if (it != Subscribers.end())
		{
			Subscribers.erase(it);
		}
	}

	template <typename ... TArgs>
	void CEvent<TArgs...>::operator+=(const FSubscriber& Subscriber)
	{
		Subscribe(Subscriber);
	}

	template <typename ... TArgs>
	void CEvent<TArgs...>::operator-=(const FSubscriber& Subscriber)
	{
		Unsubscribe(Subscriber);
	}

	template <typename ... TArgs>
	void CEvent<TArgs...>::Invoke(TArgs... Args)
	{
		for (auto& Subscriber : Subscribers)
		{
			Subscriber(Args...);
		}
	}
}
