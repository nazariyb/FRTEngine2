#include <gtest/gtest.h>

#include "TestCommon.h"

#include <string>
#include <type_traits>

#include "ECS/ComponentPool.h"
#include "ECS/CoreComponents.h"

using namespace frt;

namespace
{
EntityId E (uint32 InIndex, uint32 InGeneration = 0u)
{
	return EntityId(InIndex, InGeneration);
}

/**
 * Deliberately non-trivial, to keep the pool's support for resource-owning components
 * covered now that Comp_Name is a fixed inline buffer.
 *
 * The name is distinctive because the registry keys on it: two different types sharing a
 * name would share an id and therefore one pool, reinterpreted as both.
 */
struct SComp_TestString
{
	std::string Value;
};
}

FRT_DECLARE_COMPONENT_NAMED(SComp_TestString, "Test_StringComponent");

static_assert(!std::is_trivially_copyable_v<SComp_TestString>,
	"this type exists to exercise the non-trivial pool path");


// ---------------------------------------------------------------------------------
// EntityId
// ---------------------------------------------------------------------------------

TEST(EntityId, PacksIndexAndGeneration)
{
	const EntityId e(12345u, 7u);

	EXPECT_EQ(e.GetIndex(), 12345u);
	EXPECT_EQ(e.GetGeneration(), 7u);
	EXPECT_TRUE(e.IsValid());
}

TEST(EntityId, GenerationParticipatesInIdentity)
{
	EXPECT_NE(EntityId(5u, 1u), EntityId(5u, 2u));
	EXPECT_EQ(EntityId(5u, 1u), EntityId(5u, 1u));
}

TEST(EntityId, BoundaryValuesRoundTrip)
{
	const EntityId maxE(EntityId::MaxIndex, EntityId::MaxGeneration);

	EXPECT_EQ(maxE.GetIndex(), EntityId::MaxIndex);
	EXPECT_EQ(maxE.GetGeneration(), EntityId::MaxGeneration);
	EXPECT_TRUE(maxE.IsValid());
}

TEST(EntityId, InvalidHandleIsNotValid)
{
	EXPECT_FALSE(InvalidEntity.IsValid());
	EXPECT_EQ(sizeof(EntityId), 4u);
}


// ---------------------------------------------------------------------------------
// CComponentRegistry
// ---------------------------------------------------------------------------------

TEST(ComponentRegistry, DistinctTypesGetDistinctStableIds)
{
	const ComponentId parentId = GetComponentId<Comp_Parent>();
	const ComponentId childId  = GetComponentId<Comp_Children>();
	const ComponentId localId  = GetComponentId<Comp_LocalTransform>();

	EXPECT_NE(parentId, childId);
	EXPECT_NE(parentId, localId);
	EXPECT_EQ(GetComponentId<Comp_Parent>(), parentId);
}

TEST(ComponentRegistry, RecordsTypeInfo)
{
	const SComponentInfo* info = CComponentRegistry::Get().Find(GetComponentId<Comp_Parent>());

	ASSERT_NE(info, nullptr);
	EXPECT_EQ(info->Size, sizeof(Comp_Parent));
	EXPECT_TRUE(info->bTriviallyRelocatable);
	EXPECT_STREQ(info->Name, "Comp_Parent");
}

TEST(ComponentRegistry, FindsByStableName)
{
	// The serialized identity must be the bare name - no namespace qualification, and
	// nothing that varies with FRT_REAL_PRECISION.
	EXPECT_NE(CComponentRegistry::Get().FindByName("Comp_Parent"), nullptr);
	EXPECT_NE(CComponentRegistry::Get().FindByName("Comp_LocalTransform"), nullptr);
	EXPECT_EQ(CComponentRegistry::Get().FindByName("frt::Comp_Parent"), nullptr);
	EXPECT_EQ(CComponentRegistry::Get().FindByName(nullptr), nullptr);
}

TEST(ComponentRegistry, RegistrationIsIdempotentByName)
{
	const ComponentId parentId = GetComponentId<Comp_Parent>();
	const uint32 before = CComponentRegistry::Get().GetCount();

	SComponentInfo duplicate;
	duplicate.Name = "Comp_Parent";
	duplicate.Size = sizeof(Comp_Parent);

	// Two modules registering the same type must agree, and the later one must not
	// append a second entry.
	EXPECT_EQ(CComponentRegistry::Get().Register(duplicate), parentId);
	EXPECT_EQ(CComponentRegistry::Get().GetCount(), before);
}

TEST(ComponentRegistry, CoreComponentsGetDeterministicIds)
{
	// TestEnvironment.cpp calls RegisterCoreComponents() before any test body runs, which
	// is the precondition this contract needs: the first GetComponentId<T>() for a type
	// decides its id forever, so registering late is silently a no-op.
	//
	// These are absolute on purpose. Appending to RegisterCoreComponents() keeps them
	// passing; inserting in the middle breaks them, which is the point - it would shift
	// every id below it and change the meaning of any query signature built from them.
	EXPECT_EQ(GetComponentId<Comp_LocalTransform>(), 0u);
	EXPECT_EQ(GetComponentId<Comp_WorldTransform>(), 1u);
	EXPECT_EQ(GetComponentId<Comp_Parent>(), 2u);
	EXPECT_EQ(GetComponentId<Comp_Children>(), 3u);
}

TEST(ComponentRegistry, RepeatedRegistrationDoesNotShiftIds)
{
	const uint32 before = CComponentRegistry::Get().GetCount();

	RegisterCoreComponents();

	EXPECT_EQ(CComponentRegistry::Get().GetCount(), before);
	EXPECT_EQ(GetComponentId<Comp_LocalTransform>(), 0u);
	EXPECT_EQ(GetComponentId<Comp_Children>(), 3u);
}

TEST(ComponentRegistry, NeedsNoAllocator)
{
	// Deliberately no FRT_TEST_MEMORY_POOL(): the registry must not allocate, because its
	// lifetime falls outside the engine pool's window at both ends. If it ever reaches
	// for an allocator again, this faults.
	EXPECT_NE(GetComponentId<Comp_Parent>(), InvalidComponentId);
	EXPECT_GT(CComponentRegistry::Get().GetCount(), 0u);
}


// ---------------------------------------------------------------------------------
// TComponentPool
// ---------------------------------------------------------------------------------

TEST(ComponentPool, StartsEmpty)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	EXPECT_EQ(pool.Count(), 0u);
	EXPECT_FALSE(pool.Contains(E(3)));
	EXPECT_EQ(pool.TryGet(E(3)), nullptr);
}

TEST(ComponentPool, AddAndReadBack)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	pool.Add(E(3)).SetTranslation(Vector3r(1, 0, 0));
	pool.Add(E(7)).SetTranslation(Vector3r(2, 0, 0));
	pool.Add(E(1)).SetTranslation(Vector3r(3, 0, 0));

	const TComponentPool<Comp_LocalTransform>& constPool = pool;

	EXPECT_EQ(pool.Count(), 3u);
	EXPECT_EQ(constPool.Get(E(3)).Translation.x, Real(1));
	EXPECT_EQ(constPool.Get(E(7)).Translation.x, Real(2));
	EXPECT_EQ(constPool.Get(E(1)).Translation.x, Real(3));
}

TEST(ComponentPool, RejectsStaleGeneration)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	pool.Add(E(3, 0));

	// Same index, different generation: the sparse slot round-trip through Dense is what
	// catches this.
	EXPECT_FALSE(pool.Contains(E(3, 1)));
	EXPECT_TRUE(pool.Contains(E(3, 0)));
}

TEST(ComponentPool, ReportsAbsentForNeverInsertedIndex)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	pool.Add(E(3));

	// Index 5 lives in an allocated page but was never inserted, so its sparse slot holds
	// whatever the page was initialised with.
	EXPECT_FALSE(pool.Contains(E(5)));
}

TEST(ComponentPool, AddOverwritesExistingComponent)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	pool.Add(E(3)).SetTranslation(Vector3r(1, 0, 0));
	pool.Add(E(3)).SetTranslation(Vector3r(9, 0, 0));

	const TComponentPool<Comp_LocalTransform>& constPool = pool;
	EXPECT_EQ(pool.Count(), 1u);
	EXPECT_EQ(constPool.Get(E(3)).Translation.x, Real(9));
}

TEST(ComponentPool, SwapAndPopKeepsRemainingEntitiesFindable)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	pool.Add(E(3)).SetTranslation(Vector3r(1, 0, 0));
	pool.Add(E(7)).SetTranslation(Vector3r(2, 0, 0));
	pool.Add(E(1)).SetTranslation(Vector3r(3, 0, 0));

	// Remove the FIRST element: the last one is swapped into its slot, and its sparse
	// entry has to follow.
	pool.Remove(E(3));

	const TComponentPool<Comp_LocalTransform>& constPool = pool;
	EXPECT_EQ(pool.Count(), 2u);
	EXPECT_FALSE(pool.Contains(E(3)));
	EXPECT_EQ(constPool.Get(E(7)).Translation.x, Real(2));
	EXPECT_EQ(constPool.Get(E(1)).Translation.x, Real(3));
}

TEST(ComponentPool, RemoveOfAbsentEntityIsNoOp)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	pool.Add(E(7));
	pool.Remove(E(3));
	pool.Remove(E(3));

	EXPECT_EQ(pool.Count(), 1u);
}

TEST(ComponentPool, TailRemovalNeedsNoSwap)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	pool.Add(E(3));
	pool.Add(E(7));
	pool.Remove(E(7));

	EXPECT_EQ(pool.Count(), 1u);
	EXPECT_TRUE(pool.Contains(E(3)));
}

TEST(ComponentPool, ReusableAfterEmptying)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	pool.Add(E(3));
	pool.Remove(E(3));
	ASSERT_EQ(pool.Count(), 0u);

	pool.Add(E(3)).SetTranslation(Vector3r(9, 0, 0));

	const TComponentPool<Comp_LocalTransform>& constPool = pool;
	EXPECT_EQ(pool.Count(), 1u);
	EXPECT_EQ(constPool.Get(E(3)).Translation.x, Real(9));
}

TEST(ComponentPool, SparsePagesSpanDiscontiguousIndices)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_Parent> pool;

	constexpr uint32 pageSize = TComponentPool<Comp_Parent>::SparsePageSize;

	const EntityId low(1u, 0u);
	const EntityId mid(pageSize + 5u, 0u);       // page 1
	const EntityId high(pageSize * 9u + 3u, 0u); // page 9, pages 2..8 never allocated

	pool.Add(low).Depth = 1u;
	pool.Add(mid).Depth = 2u;
	pool.Add(high).Depth = 3u;

	const TComponentPool<Comp_Parent>& constPool = pool;
	EXPECT_EQ(constPool.Get(low).Depth, 1u);
	EXPECT_EQ(constPool.Get(mid).Depth, 2u);
	EXPECT_EQ(constPool.Get(high).Depth, 3u);

	// An index inside a page that was never allocated must not fault.
	EXPECT_FALSE(pool.Contains(EntityId(pageSize * 5u, 0u)));

	pool.Remove(mid);
	EXPECT_FALSE(pool.Contains(mid));
	EXPECT_TRUE(pool.Contains(low));
	EXPECT_TRUE(pool.Contains(high));
}


// ---------------------------------------------------------------------------------
// Change tracking
// ---------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------
// Non-trivial components
// ---------------------------------------------------------------------------------

TEST(ComponentPool, StoresNonTriviallyCopyableComponents)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<SComp_TestString> pool;

	// Works only because TArray::ReAlloc move-constructs instead of relocating raw bytes
	// when the element type is non-trivial.
	for (uint32 i = 0u; i < 300u; ++i)
	{
		pool.Add(E(i)).Value = "entity-" + std::to_string(i);
	}

	const TComponentPool<SComp_TestString>& constPool = pool;

	ASSERT_EQ(pool.Count(), 300u);
	EXPECT_EQ(constPool.Get(E(0)).Value, "entity-0");
	EXPECT_EQ(constPool.Get(E(299)).Value, "entity-299") << "contents lost during pool growth";
}

TEST(ComponentPool, SwapAndPopMovesNonTrivialComponents)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<SComp_TestString> pool;

	pool.Add(E(3)).Value = "first";
	pool.Add(E(7)).Value = "second";
	pool.Add(E(1)).Value = "third";

	pool.Remove(E(3));

	const TComponentPool<SComp_TestString>& constPool = pool;

	ASSERT_EQ(pool.Count(), 2u);
	EXPECT_FALSE(pool.Contains(E(3)));
	EXPECT_EQ(constPool.Get(E(7)).Value, "second");
	EXPECT_EQ(constPool.Get(E(1)).Value, "third") << "swapped element's string was corrupted";
}


// ---------------------------------------------------------------------------------
// Comp_Name
// ---------------------------------------------------------------------------------

TEST(CompName, StaysTriviallyCopyableAndInline)
{
	// The reason it is a fixed buffer at all: nearly every entity carries a name, so the
	// cost is paid world-wide and the component has to stay memcpy-able.
	EXPECT_EQ(sizeof(Comp_Name), Comp_Name::Capacity);
	EXPECT_TRUE(std::is_trivially_copyable_v<Comp_Name>);
}

TEST(CompName, DefaultsToEmpty)
{
	const Comp_Name name;

	EXPECT_TRUE(name.IsEmpty());
	EXPECT_STREQ(name.Get(), "");
}

TEST(CompName, RoundTripsAName)
{
	Comp_Name name;
	name.Set("Sponza");

	EXPECT_STREQ(name.Get(), "Sponza");
	EXPECT_FALSE(name.IsEmpty());
	EXPECT_TRUE(name == "Sponza");
	EXPECT_FALSE(name == "Portal");
}

TEST(CompName, TruncatesRatherThanOverflowing)
{
	const std::string tooLong(Comp_Name::Capacity * 2u, 'x');

	Comp_Name name;
	name.Set(tooLong);

	// Truncation is deliberate: a name is a label, and failing over a long one would be
	// worse than showing a shortened one.
	EXPECT_EQ(name.AsView().size(), Comp_Name::Capacity - 1u);
	EXPECT_EQ(name.Get()[Comp_Name::Capacity - 1u], '\0') << "must stay null-terminated";
}

TEST(CompName, ExactlyFittingNameIsNotTruncated)
{
	const std::string exact(Comp_Name::Capacity - 1u, 'y');

	Comp_Name name(exact);

	EXPECT_EQ(name.AsView().size(), Comp_Name::Capacity - 1u);
	EXPECT_TRUE(name == exact);
}

TEST(CompName, SettingAShorterNameClearsTheRemainder)
{
	Comp_Name name("a-fairly-long-entity-name");
	name.Set("short");

	EXPECT_STREQ(name.Get(), "short") << "stale bytes leaked past the new terminator";
}

TEST(CompName, SurvivesPoolGrowthAndRemoval)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_Name> pool;

	for (uint32 i = 0u; i < 300u; ++i)
	{
		pool.Add(E(i), std::string("entity-") + std::to_string(i));
	}

	const TComponentPool<Comp_Name>& constPool = pool;
	ASSERT_EQ(pool.Count(), 300u);
	EXPECT_TRUE(constPool.Get(E(299)) == "entity-299");

	pool.Remove(E(0));
	EXPECT_TRUE(constPool.Get(E(299)) == "entity-299") << "swapped element corrupted";
}


TEST(ComponentPoolChangeTracking, MutableAccessMarksDirty)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	pool.Add(E(0));
	const uint64 mark = pool.GetVersion();

	EXPECT_FALSE(pool.IsDirtySince(E(0), mark));

	pool.Get(E(0)).SetTranslation(Vector3r(5, 0, 0));
	EXPECT_TRUE(pool.IsDirtySince(E(0), mark));
}

TEST(ComponentPoolChangeTracking, ConstAccessLeavesVersionAlone)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	pool.Add(E(0));
	const uint64 mark = pool.GetVersion();

	const TComponentPool<Comp_LocalTransform>& constPool = pool;
	(void)constPool.Get(E(0));

	EXPECT_EQ(pool.GetVersion(), mark);
	EXPECT_FALSE(pool.IsDirtySince(E(0), mark));
}

TEST(ComponentPoolChangeTracking, DistantWriteDoesNotDirtyOtherBlocks)
{
	FRT_TEST_MEMORY_POOL();
	TComponentPool<Comp_LocalTransform> pool;

	constexpr uint32 blockSize = TComponentPool<Comp_LocalTransform>::DirtyBlockSize;

	for (uint32 i = 0u; i <= blockSize * 2u; ++i)
	{
		pool.Add(E(i));
	}

	const uint64 mark = pool.GetVersion();
	const EntityId distant = E(blockSize + 1u);

	pool.Get(distant).SetTranslation(Vector3r(1, 1, 1));

	EXPECT_TRUE(pool.IsDirtySince(distant, mark));
	EXPECT_FALSE(pool.IsDirtySince(E(0), mark));
}
