// TMulticastDelegate calls its subscribers in the order they were added, survives subscribers that
// subscribe and unsubscribe while it broadcasts (with the outcome the class comment promises), and
// ReleaseModule drops exactly the subscriptions that would run an unloading module's code - with a
// fake image range, and with a real DLL loaded and unloaded under them.
#include <gtest/gtest.h>

#include "TestCommon.h"

#include <filesystem>
#include <memory>
#include <new>
#include <vector>

#include "Containers/String.h"
#include "Functional/MulticastDelegate.h"
#include "Platform/Platform.h"
#include "TestModule/TestModule.h"

#include <Windows.h>

using namespace frt;


namespace
{
void AppendSeven (std::vector<int>& Calls)
{
	Calls.push_back(7);
}

struct SListener
{
	int NumCalls = 0;

	void OnEvent (std::vector<int>& Calls)
	{
		++NumCalls;
		Calls.push_back(100);
	}

	void OnEventConst (std::vector<int>& Calls) const
	{
		Calls.push_back(200);
	}
};

// Counts how many of its copies are alive: a removed subscription's callable must be destroyed, and
// exactly once.
struct SLifetime
{
	explicit SLifetime (int& InAlive) noexcept
		: Alive(&InAlive)
	{
		++*Alive;
	}

	SLifetime (const SLifetime& Other) noexcept
		: Alive(Other.Alive)
	{
		++*Alive;
	}

	SLifetime& operator= (const SLifetime&) = delete;

	~SLifetime ()
	{
		--*Alive;
	}

	int* Alive;
};

using FDelegate = TMulticastDelegate<std::vector<int>&>;
}


TEST(MulticastDelegate, CallsSubscribersInTheOrderTheyWereAdded)
{
	FDelegate delegate;
	std::vector<int> calls;
	SListener listener;
	const SListener constListener;

	delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(1); });
	delegate.Add(&AppendSeven);
	delegate.Add(&listener, &SListener::OnEvent);
	delegate.Add(&constListener, &SListener::OnEventConst);
	delegate += [] (std::vector<int>& Calls) { Calls.push_back(2); };
	EXPECT_EQ(delegate.GetNum(), 5u);

	delegate.Broadcast(calls);
	delegate.Broadcast(calls);
	EXPECT_EQ(calls, (std::vector<int>{ 1, 7, 100, 200, 2, 1, 7, 100, 200, 2 }));
	EXPECT_EQ(listener.NumCalls, 2);
}

TEST(MulticastDelegate, PassesArgumentsByTheirDeclaredTypes)
{
	TMulticastDelegate<int, const FString&> delegate;
	FString seen;
	int sum = 0;
	delegate.Add([&seen, &sum] (int Value, const FString& Text) { sum += Value; seen.Append(Text); });
	delegate.Add([&sum] (int Value, const FString&) { sum += Value * 10; });
	delegate.Broadcast(3, FString("ab"));
	EXPECT_EQ(sum, 33);
	EXPECT_EQ(seen, "ab");

	TMulticastDelegate<> noArguments;
	int calls = 0;
	noArguments += [&calls] () { ++calls; };
	noArguments.Broadcast();
	EXPECT_EQ(calls, 1);
}

TEST(MulticastDelegate, RemoveEndsOneSubscriptionByItsHandle)
{
	FDelegate delegate;
	std::vector<int> calls;
	const SDelegateHandle first = delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(1); });
	const SDelegateHandle second = delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(2); });
	const SDelegateHandle third = delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(3); });
	EXPECT_TRUE(first.IsValid());
	EXPECT_NE(first, second);
	EXPECT_TRUE(delegate.Contains(second));

	EXPECT_TRUE(delegate.Remove(second));
	EXPECT_FALSE(delegate.Contains(second));
	EXPECT_FALSE(delegate.Remove(second));          // already gone
	EXPECT_FALSE(delegate.Remove(SDelegateHandle()));
	delegate.Broadcast(calls);
	EXPECT_EQ(calls, (std::vector<int>{ 1, 3 }));

	// Handles are unique in the process: another delegate's handle names nothing here.
	FDelegate other;
	const SDelegateHandle foreign = other.Add([] (std::vector<int>&) {});
	EXPECT_FALSE(delegate.Contains(foreign));
	EXPECT_FALSE(delegate.Remove(foreign));
	EXPECT_EQ(delegate.GetNum(), 2u);
	EXPECT_TRUE(delegate.Remove(third));
	EXPECT_TRUE(delegate.Remove(first));
	EXPECT_TRUE(delegate.IsEmpty());
}

TEST(MulticastDelegate, AnEmptyCallableSubscribesNothing)
{
	FDelegate delegate;
	void (*null) (std::vector<int>&) = nullptr;
	EXPECT_FALSE(delegate.Add(null).IsValid());
	EXPECT_FALSE(delegate.Add(FDelegate::FFunction()).IsValid());
	EXPECT_TRUE(delegate.IsEmpty());
}

TEST(MulticastDelegate, RemovedCallablesAreDestroyedExactlyOnce)
{
	int alive = 0;
	{
		FDelegate delegate;
		const SLifetime lifetime(alive);
		const SDelegateHandle handle = delegate.Add([lifetime] (std::vector<int>&) {});
		delegate.Add([lifetime] (std::vector<int>&) {});
		EXPECT_EQ(alive, 3);
		delegate.Remove(handle);
		EXPECT_EQ(alive, 2);
		delegate.Clear();
		EXPECT_EQ(alive, 1);
		delegate.Add([lifetime] (std::vector<int>&) {});
	}
	EXPECT_EQ(alive, 0);
}

TEST(MulticastDelegate, ASubscriberMayRemoveItselfDuringTheBroadcast)
{
	FDelegate delegate;
	std::vector<int> calls;
	int alive = 0;
	SDelegateHandle self;
	{
		const SLifetime lifetime(alive);
		self = delegate.Add([&delegate, &self, lifetime] (std::vector<int>& Calls)
		{
			Calls.push_back(1);
			EXPECT_TRUE(delegate.Remove(self));
			// Still alive: it is running.
			EXPECT_GE(*lifetime.Alive, 1);
		});
	}
	delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(2); });
	EXPECT_EQ(alive, 1);

	delegate.Broadcast(calls);
	EXPECT_EQ(alive, 0);   // destroyed when the broadcast ended
	EXPECT_EQ(delegate.GetNum(), 1u);
	delegate.Broadcast(calls);
	EXPECT_EQ(calls, (std::vector<int>{ 1, 2, 2 }));
}

TEST(MulticastDelegate, ASubscriberRemovedDuringTheBroadcastIsNotCalledByIt)
{
	FDelegate delegate;
	std::vector<int> calls;
	SDelegateHandle first;
	SDelegateHandle third;
	first = delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(1); });
	delegate.Add([&delegate, &first, &third] (std::vector<int>& Calls)
	{
		Calls.push_back(2);
		delegate.Remove(first);   // already called this time
		delegate.Remove(third);   // not yet: skipped
		EXPECT_EQ(delegate.GetNum(), 1u);
	});
	third = delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(3); });

	delegate.Broadcast(calls);
	EXPECT_EQ(calls, (std::vector<int>{ 1, 2 }));
	delegate.Broadcast(calls);
	EXPECT_EQ(calls, (std::vector<int>{ 1, 2, 2 }));
}

TEST(MulticastDelegate, ASubscriberAddedDuringTheBroadcastIsFirstCalledByTheNextOne)
{
	FDelegate delegate;
	std::vector<int> calls;
	bool bAdded = false;
	delegate.Add([&delegate, &bAdded] (std::vector<int>& Calls)
	{
		Calls.push_back(1);
		if (!bAdded)
		{
			bAdded = true;
			delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(9); });
			EXPECT_EQ(delegate.GetNum(), 3u);
		}
	});
	delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(2); });

	delegate.Broadcast(calls);
	EXPECT_EQ(calls, (std::vector<int>{ 1, 2 }));
	delegate.Broadcast(calls);
	EXPECT_EQ(calls, (std::vector<int>{ 1, 2, 1, 2, 9 }));
}

TEST(MulticastDelegate, ASubscriberAddedAndRemovedDuringOneBroadcastNeverRuns)
{
	FDelegate delegate;
	std::vector<int> calls;
	delegate.Add([&delegate] (std::vector<int>& Calls)
	{
		Calls.push_back(1);
		const SDelegateHandle added = delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(9); });
		EXPECT_TRUE(delegate.Contains(added));
		EXPECT_TRUE(delegate.Remove(added));
		EXPECT_FALSE(delegate.Contains(added));
	});

	delegate.Broadcast(calls);
	delegate.Broadcast(calls);
	EXPECT_EQ(calls, (std::vector<int>{ 1, 1 }));
	EXPECT_EQ(delegate.GetNum(), 1u);
}

TEST(MulticastDelegate, NestedBroadcastsSeeTheSameChangesAndApplyThemOnce)
{
	FDelegate delegate;
	std::vector<int> calls;
	int depth = 0;
	SDelegateHandle last;
	delegate.Add([&delegate, &depth, &last] (std::vector<int>& Calls)
	{
		Calls.push_back(10 + depth);
		if (depth == 0)
		{
			++depth;
			delegate.Remove(last);   // the nested broadcast must skip it too
			delegate.Broadcast(Calls);
			--depth;
		}
	});
	last = delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(99); });

	delegate.Broadcast(calls);
	EXPECT_EQ(calls, (std::vector<int>{ 10, 11 }));
	EXPECT_EQ(delegate.GetNum(), 1u);
}

TEST(MulticastDelegate, ClearDuringTheBroadcastStopsEveryoneLeft)
{
	FDelegate delegate;
	std::vector<int> calls;
	delegate.Add([&delegate] (std::vector<int>& Calls)
	{
		Calls.push_back(1);
		delegate.Clear();
		EXPECT_TRUE(delegate.IsEmpty());
	});
	delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(2); });

	delegate.Broadcast(calls);
	delegate.Broadcast(calls);
	EXPECT_EQ(calls, (std::vector<int>{ 1 }));
}

TEST(MulticastDelegate, ReleaseModuleDropsSubscriptionsWhoseCodeOrTargetIsInTheImage)
{
	FDelegate delegate;
	std::vector<int> calls;
	int alive = 0;

	// An object in the "image": a buffer standing in for a module's static data (as in
	// ComponentPoolTest's registry test).
	alignas(SListener) unsigned char image[64] = {};
	SListener* inImage = new (image) SListener();

	// A function built from a lambda: its invoker stands in for code in the image.
	FDelegate::FFunction built([lifetime = SLifetime(alive)] (std::vector<int>& Calls) { Calls.push_back(5); });
	const char* invoker = static_cast<const char*>(built.GetInvoker());
	// A function pointer: its target.
	const char* target = reinterpret_cast<const char*>(&AppendSeven);

	delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(1); });
	delegate.Add(inImage, &SListener::OnEvent);
	delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(2); });
	delegate.Add(std::move(built));
	delegate.Add(&AppendSeven);
	const SDelegateHandle kept = delegate.Add([] (std::vector<int>& Calls) { Calls.push_back(3); });
	EXPECT_EQ(alive, 1);

	// A range matching nothing drops nothing.
	unsigned char elsewhere[8] = {};
	EXPECT_EQ(delegate.ReleaseModule(elsewhere, elsewhere + sizeof(elsewhere)), 0u);

	EXPECT_EQ(delegate.ReleaseModule(image, image + sizeof(image)), 1u);
	EXPECT_EQ(delegate.ReleaseModule(invoker, invoker + 1), 1u);
	EXPECT_EQ(alive, 0);   // destroyed by the release, while its code is still there
	EXPECT_EQ(delegate.ReleaseModule(target, target + 1), 1u);
	inImage->~SListener();

	delegate.Broadcast(calls);
	EXPECT_EQ(calls, (std::vector<int>{ 1, 2, 3 }));
	EXPECT_EQ(delegate.GetNum(), 3u);
	EXPECT_TRUE(delegate.Contains(kept));
}


// ---------------- A real module ----------------

namespace
{
// Core-TestModule.dll beside this executable, named as the build names it: Core-Test-<config>.exe
// loads Core-TestModule-<config>.dll (Release-Default: Core-Test.exe, Core-TestModule.dll).
std::filesystem::path GetTestModulePath ()
{
	const std::filesystem::path executable = platform::GetExecutablePath();
	const FString stem = FString::FromPath(executable.stem());
	constexpr FStringLiteral prefix = "Core-Test";
	if (!stem.StartsWith(prefix))
	{
		return {};
	}
	FString name("Core-TestModule");
	name.Append(stem.View().RightChop(prefix.Len())).Append(".dll");
	return executable.parent_path() / name.ToPath();
}

// The module's image: [base, base + SizeOfImage), as CGameModule::Unload computes it.
struct SImage
{
	const unsigned char* Begin = nullptr;
	const unsigned char* End = nullptr;
};

SImage GetImage (HMODULE Module)
{
	const auto* base = reinterpret_cast<const unsigned char*>(Module);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	return SImage{ base, base + headers->OptionalHeader.SizeOfImage };
}

template <typename T>
T GetExport (HMODULE Module, const char* Name)
{
	return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(Module, Name)));
}
}

TEST(MulticastDelegate, ReleaseModuleBeforeUnloadingARealDllLeavesOnlyOtherSubscribers)
{
	using namespace frt::test_module;

	const std::filesystem::path path = GetTestModulePath();
	ASSERT_FALSE(path.empty());
	HMODULE module = LoadLibraryW(path.c_str());
	ASSERT_NE(module, nullptr) << "cannot load " << FString::FromPath(path).CStr() << " (built with Core-Test)";
	const SImage image = GetImage(module);

	const auto makeSmall = GetExport<FMakeSmallLambda>(module, MakeSmallLambdaName);
	const auto makeLarge = GetExport<FMakeLargeLambda>(module, MakeLargeLambdaName);
	const auto addOne = GetExport<FAddOne>(module, AddOneName);
	const auto getCounter = GetExport<FGetCounter>(module, GetCounterName);
	ASSERT_TRUE(makeSmall && makeLarge && addOne && getCounter);

	TMulticastDelegate<int&> delegate;
	int ownCalls = 0;
	delegate.Add([&ownCalls] (int& Total) { ++ownCalls; Total += 10000; });

	// What the module builds: its code backs the functions, inline and in a block it allocated.
	FCounterFunction inlineFunction;
	makeSmall(&inlineFunction);
	FCounterFunction heapFunction;
	makeLarge(&heapFunction);
	EXPECT_TRUE(inlineFunction.IsInline());
	EXPECT_FALSE(heapFunction.IsInline());
	EXPECT_TRUE(inlineFunction.RefersToImage(image.Begin, image.End));
	EXPECT_TRUE(heapFunction.RefersToImage(image.Begin, image.End));
	delegate.Add(std::move(inlineFunction));
	delegate.Add(std::move(heapFunction));
	// What this executable builds around the module's function and object: only targets are there.
	delegate.Add(addOne);
	SCounter* counter = getCounter();
	delegate.Add(counter, &SCounter::Bump);
	const FCounterFunction own([] (int&) {});
	EXPECT_FALSE(own.RefersToImage(image.Begin, image.End));

	int total = 0;
	delegate.Broadcast(total);
	EXPECT_EQ(total, 10000 + 10 + 100 + 1 + 1000);
	EXPECT_EQ(counter->Calls, 1);

	EXPECT_EQ(delegate.ReleaseModule(image.Begin, image.End), 4u);
	ASSERT_TRUE(FreeLibrary(module));

	// Nothing of the module runs, or is destroyed, after it is gone.
	total = 0;
	delegate.Broadcast(total);
	EXPECT_EQ(total, 10000);
	EXPECT_EQ(ownCalls, 2);
	EXPECT_EQ(delegate.GetNum(), 1u);
}
