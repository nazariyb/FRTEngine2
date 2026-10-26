#pragma once

// The §5.11 benchmark generator (G3): a synthetic project committed through the real registry store, so every
// number is measured on the exact §5.2 schema - FILES, ASSETS, SUBENTRY, the per-type column tables and NAMES
// interning - rather than a thin stand-in. M3's types are frozen (Assets/AssetTypes.h) and none is a table row
// type, so the generator's rows are sub-entries of frt.Unknown table records with row labels (no ':', §3.6) and
// type frt.Model: its five Int64 keys stand for a row struct whose every field is a column.
//
// Content, per asset index (records are independent of the chunking: each draws from a hash of its index):
//  - textures (50%): Width/Height in {64 .. 8192}, Channels 3/4, HasAlpha; 0.5% carry an Analyze warning;
//  - legacy materials (20%): fourteen Build|Path texture edges (every 20th material uses texture 0; 1% of the
//    edges name a texture that does not exist), 30% flagged LegacyReferences;
//  - glTF models (10%): two mesh and two material sub-entries, a .bin companion and its edge, a Hard edge from
//    each material sub-entry and eight more from the model to materials;
//  - input actions (4%, three Path edges to other actions) and primary input maps (1%, Actions, four edges);
//  - raw audio files (15%);
//  - tables: Rows / 500 of them (taken from the asset count), 500 rows each, one Row|Schemaless edge per row to a
//    texture.
// Paths are /Project/<Category>/<Theme>/S<n>/<Prefix>_<Theme>_<Word>_<k>.<ext>, about 48 files per folder; every
// record carries one or two rule labels.

#include <vector>

#include "Assets/Registry/RegistryStore.h"
#include "Containers/String.h"


namespace frt::assets::query_bench
{
struct SGeneratorSpec
{
	uint32 Assets = 10000u;          // asset records, tables included
	uint32 Rows = 0u;                // table rows (sub-entries), 500 per table
	uint32 Seed = 20261026u;
	uint32 ChunkWeight = 50000u;     // records + sub-entries per committed delta
};

struct SGeneratorReport
{
	uint32 Textures = 0u;
	uint32 Materials = 0u;
	uint32 Models = 0u;
	uint32 Actions = 0u;
	uint32 Maps = 0u;
	uint32 Raws = 0u;
	uint32 Tables = 0u;
	uint32 Rows = 0u;
	uint32 Commits = 0u;
	double CommitMilliseconds = 0.0;  // every Commit, publication included
	double BuildMilliseconds = 0.0;   // generating the deltas
};

// Commits the generated project into an empty store (mounts included). A store that holds records already
// gets the same records again (re-upserts).
SGeneratorReport Generate (CRegistryStore& Store, const SGeneratorSpec& Spec);

// Mount paths the benchmarks query by (k: the category-local index).
FString GetTexturePath (uint32 K);
FString GetMaterialPath (uint32 K);
FString GetModelPath (uint32 K);
}
