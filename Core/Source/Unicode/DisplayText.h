#pragma once

#include "Core.h"
#include "Containers/String.h"


namespace frt::unicode
{
/**
 * A name or path as spelled on disk, made fit to show: each WTF-8 surrogate (the 3-byte form of a lone UTF-16
 * surrogate, which Windows allows in names and EnumerateDirectory keeps) becomes one U+FFFD, and any other
 * ill-formed sequence what FString::FromUtf8Lossy makes of it. Valid UTF-8 comes back as it is.
 *
 * For text people read - a diagnostic's Message, a dump, a CLI line, a list panel - never for a key: the on-disk
 * spelling stays the key everywhere it is stored (a diagnostic's Subject included), so two names that differ only
 * in a surrogate stay apart (§13.4: a non-UTF-8 name is PATH002).
 */
[[nodiscard]] FRT_CORE_API FString ToDisplayText (FStringView OnDiskSpelling);
}
