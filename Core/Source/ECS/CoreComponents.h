#pragma once

#include "ECS/Comp_Children.h"
#include "ECS/Comp_Name.h"
#include "ECS/Comp_Parent.h"
#include "ECS/ComponentRegistry.h"
#include "Math/Comp_LocalTransform.h"
#include "Math/Comp_WorldTransform.h"


// Convenience header pulling in every engine component at once, plus the deterministic
// registration entry point.
//
// Every component declares its own traits in its own header, including the ones under
// Math/. That costs those headers an include of ComponentRegistry.h, which only needs
// Core.h and CoreTypes.h - both of which Math already pulls in - and it removes a real
// trap: registering elsewhere meant a translation unit could include a component, use it
// with the ECS, and fail to compile because the traits happened to live in a header it
// had no reason to include.
//
// A component's Name is its serialized identity and must never change casually; renaming
// one invalidates every saved scene that references it.


namespace frt
{
/**
 * Pins the engine's own component ids to a declared order. Call once at startup.
 *
 * Without this, ids are assigned in first-touch order, so a headless test that reaches
 * for Comp_Parent first ends up with different ids than the game reaching for
 * Comp_LocalTransform first. Nothing serializes ids - names are the stable identity - but
 * a query signature is a bitmask over ids, and an unstable signature is one that cannot
 * be logged, cached, or compared across runs.
 *
 * Lazy registration still works for everything else, so game-specific components, tools,
 * and tests need no ceremony. This only fixes the engine's own types in place.
 */
FRT_CORE_API void RegisterCoreComponents();
}
