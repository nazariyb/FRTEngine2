// Readers on other threads see only whole deltas (design §5.9, §15 I16): eight threads acquire views while
// the owner thread commits 10k deltas, and every view they get satisfies invariants that a half-applied
// delta would break - the counter record's two tags, the number of records and the newest item all agree
// with the view's revision. Also: Acquire keeps a view alive and unchanged however many commits follow.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "RegistryTestFixture.h"

using namespace frt;
using namespace frt::assets;
using namespace frt::assets::registry_test;


namespace
{
// Commit i (1-based) sets the counter's Width and Height to i and adds item i with Width i.
SRegistryDelta MakeStep (uint32 Step)
{
	SRegistryDelta delta;
	SRecordUpsert counter = MakeRecord("/Project/Counter.png");
	counter.Tags.push_back(MakeIntTag("Width", Step));
	counter.Tags.push_back(MakeIntTag("Height", Step));
	delta.RecordUpserts.push_back(std::move(counter));
	const FString item = FString::Format("/Project/Items/{}/i{}.png", Step % 32u, Step);
	delta.FileUpserts.push_back(MakeFile(item, Step));
	SRecordUpsert record = MakeRecord(item);
	record.Tags.push_back(MakeIntTag("Width", Step));
	record.Edges.push_back(MakePathEdge("/Project/Counter.png"));
	delta.RecordUpserts.push_back(std::move(record));
	return delta;
}
}

TEST(RegistryConcurrencyTest, ReadersOnEightThreadsSeeOnlyWholeDeltas)
{
	// 10k in optimized builds (§15 I16); Debug commits some ten times slower, so it checks 2k.
	constexpr uint32 commits = FRT_BUILD_DEBUG ? 2000u : 10000u;
	CRegistryStore store;
	store.Commit(MakeMountsDelta());
	const uint64 baseRevision = store.GetRevision();

	std::atomic<bool> bDone = false;
	std::atomic<uint32> failures = 0u;
	std::atomic<uint64> checkedViews = 0u;
	std::vector<std::thread> readers;
	std::vector<uint64> distinctRevisions(8u, 0u);
	for (uint32 t = 0u; t < 8u; ++t)
	{
		readers.emplace_back([&, t] ()
		{
			uint64 lastRevision = ~0ull;
			while (!bDone.load(std::memory_order_acquire))
			{
				const std::shared_ptr<const CRegistryView> view = store.Acquire();
				const uint64 step = view->GetRevision() - baseRevision;
				if (view->GetRevision() != lastRevision)
				{
					lastRevision = view->GetRevision();
					++distinctRevisions[t];
				}
				if (step == 0u)
				{
					continue;
				}
				bool bOk = view->GetRecordCount() == step + 1u;
				const SRecordId counter = view->FindByPath("/Project/Counter.png").value_or(SRecordId {});
				STagValue width;
				STagValue height;
				bOk = bOk && view->GetTag(counter, "Width", width) && view->GetTag(counter, "Height", height);
				bOk = bOk && width.AsInt64() == static_cast<int64>(step) && height.AsInt64() == static_cast<int64>(step);
				const SRecordId item = view->FindByPath(FString::Format("/Project/Items/{}/i{}.png", step % 32u, step)).value_or(SRecordId {});
				STagValue itemWidth;
				bOk = bOk && view->GetTag(item, "Width", itemWidth) && itemWidth.AsInt64() == static_cast<int64>(step);
				const SRecordId next = view->FindByPath(FString::Format("/Project/Items/{}/i{}.png", (step + 1u) % 32u, step + 1u)).value_or(SRecordId {});
				bOk = bOk && !next.IsValid();
				// The item's edge resolves to the counter, and the counter's referencers are every item so far.
				uint32 referencers = 0u;
				view->ForEachReferencer(counter, EDepFlags {}, EDepFlags {}, [&] (const SDependencyView&) { ++referencers; });
				bOk = bOk && referencers == step;
				if (!bOk)
				{
					failures.fetch_add(1u);
				}
				checkedViews.fetch_add(1u);
			}
		});
	}

	for (uint32 step = 1u; step <= commits; ++step)
	{
		store.Commit(MakeStep(step));
	}
	bDone.store(true, std::memory_order_release);
	for (std::thread& reader : readers)
	{
		reader.join();
	}

	EXPECT_EQ(failures.load(), 0u);
	EXPECT_GT(checkedViews.load(), 0u);
	uint64 revisionsSeen = 0u;
	for (const uint64 count : distinctRevisions)
	{
		revisionsSeen += count;
	}
	RecordProperty("CheckedViews", static_cast<int>(checkedViews.load()));
	RecordProperty("RevisionsSeen", static_cast<int>(revisionsSeen));
	std::printf("[registry] 8 readers checked %llu views over %u commits (%llu distinct revisions seen)\n",
		static_cast<unsigned long long>(checkedViews.load()), commits, static_cast<unsigned long long>(revisionsSeen));
	EXPECT_EQ(store.Live().GetRecordCount(), commits + 1u);
}

TEST(RegistryConcurrencyTest, AnAcquiredViewStaysUnchangedWhileCommitsContinue)
{
	CRegistryStore store;
	SRegistryDelta delta = MakeMountsDelta();
	AddTexture(delta, "/Project/a.png", 1, 1);
	store.Commit(std::move(delta));
	const std::shared_ptr<const CRegistryView> held = store.Acquire();
	const FString dumpBefore = Dump(*held);
	for (uint32 i = 0u; i < 200u; ++i)
	{
		SRegistryDelta edit;
		AddTexture(edit, "/Project/a.png", i + 2u, i + 2u);
		AddTexture(edit, FString::Format("/Project/n{}.png", i), i, i);
		if (i % 50u == 49u)
		{
			edit.FolderRemoves.push_back("/Project");
		}
		store.Commit(std::move(edit));
	}
	store.CompactGraph();
	EXPECT_EQ(Dump(*held), dumpBefore);
	EXPECT_EQ(held->GetRevision(), 1u);
}
