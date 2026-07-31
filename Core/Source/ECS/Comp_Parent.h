#pragma once
#include "CoreTypes.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/EntityId.h"


namespace frt
{
/**
 * "I have a parent." Present on every non-root entity in a hierarchy.
 *
 * Sibling links live here rather than in Comp_Children because they describe this
 * entity's position in its parent's list, not the list as a whole. Together with
 * Comp_Children::FirstChild they form an intrusive doubly-linked child list: no
 * allocation, POD, trivially relocatable, O(1) attach and detach.
 *
 * Depth is distance from a root - roots are 0, their children 1, and so on.
 * Sys_Transform iterates this pool in depth order so a parent's world transform is
 * always final before any child composes against it. Keeping the pool sorted by Depth
 * makes each level a contiguous range, which is also what makes a level safe to run in
 * parallel.
 *
 * Reparenting must rewrite Depth for the entity AND its entire subtree - that is the
 * operation Comp_Children exists to keep a subtree walk rather than a full pool scan.
 */
struct Comp_Parent
{
	EntityId Parent      = InvalidEntity;

	/** Previous / next child of the same parent. Invalid at the ends of the list. */
	EntityId PrevSibling = InvalidEntity;
	EntityId NextSibling = InvalidEntity;

	uint16   Depth       = 0u;
};
}

// Named rather than stringized: FRT_DECLARE_COMPONENT would capture the qualification
// exactly as written ("frt::Comp_Parent"), and this name ends up in save files.
FRT_DECLARE_COMPONENT_NAMED(frt::Comp_Parent, "Comp_Parent");
