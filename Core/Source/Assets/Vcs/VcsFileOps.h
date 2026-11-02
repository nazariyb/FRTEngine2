#pragma once

#include <span>

#include "Assets/Vcs/VcsProvider.h"
#include "Containers/String.h"
#include "CoreTypes.h"
#include "Functional/FunctionRef.h"


/**
 * What the None and Git providers do alike (design §12.9 table): neither is told about a check-out or a delete, so both
 * come down to the file system. Private to the providers; later providers (Perforce) go through their VCS instead.
 */
namespace frt::assets
{
// Refuses the batch with ReadOnly, naming every read-only file, and never clears the attribute: a read-only file is a
// VCS's way of saying "not opened" or a person's of saying "leave it". A file that does not exist yet is fine.
[[nodiscard]] SVcsResult CheckOutFiles (std::span<const FString> Files);

// To the Recycle Bin, so a delete stays recoverable. A file already gone is done; the first that fails ends the batch.
[[nodiscard]] SVcsResult TrashFiles (std::span<const FString> Files);

// From and To name one file under the portable fold, in different spellings (§4.6).
[[nodiscard]] bool IsCaseOnlyRename (const SVcsMove& Move);

// A rename that never lands on another file, creating To's folder if needed.
[[nodiscard]] SVcsResult MoveFilePlain (const SVcsMove& Move);

/**
 * Moves the batch as one: an asset and its meta end up both moved or both where they were. Everything MoveOne would refuse
 * for a reason visible up front (a missing source, an occupied target) refuses before anything moves; if MoveOne fails
 * anyway, what moved already moves back, last first, through MoveOne. Bytes-identical From and To do nothing.
 */
[[nodiscard]] SVcsResult RunMoves (std::span<const SVcsMove> Moves, TFunctionRef<SVcsResult (const SVcsMove&)> MoveOne);
}
