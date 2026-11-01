#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Diagnostics.h"


/**
 * The asset system's diagnostic codes (design §13.4), as packed constants for SDiagnostic::Code. One place
 * spells each code, so a typo is a compile error instead of a code nobody groups by. Codes arrive with the
 * milestone that first reports them; M3 reports these:
 *	PATH001  two files or folders of one mount equal under the portable fold (a warning until M4)
 *	PATH002  a name illegal on a supported platform, or not valid UTF-8 (a warning until M4)
 *	PATH003  a mount-relative path longer than 180 UTF-8 bytes (warning)
 *	RULE001  a rule file error, with file and line (error)
 *	RULE002  an unclaimed file not listed in Scan.Quiet (warning)
 *	REF006   a reference resolved through a path whose case differs from the file's (info)
 *	REF007   a legacy path reference in data (info)
 * M4 names these; M4's pieces start to report them (severities as in §13.4):
 *	META001  an included file without a meta after the creation delay
 *	META002  an orphan meta: its source is gone
 *	META003  a duplicate GUID
 *	META004  a broken meta: a parse error, conflict markers, two Guid: lines
 *	META005  a meta that is not canonical (it would be rewritten)
 *	META006  a meta whose case differs from its source's
 *	META007  an excluded file that still has a meta
 *	META008  a meta missing and not restorable from a verified copy
 *	META009  an orphan meta that matches a new file by content, outside the move window (a relink suggestion)
 *	VCS001   CRLF in a hashed FRT text file
 *	VCS002   the VCS refused a write; the command wrote nothing
 *	VCS003   an asset and its meta tracked differently ("generated locally: commit it")
 *	VCS004   a Git LFS pointer file where a source is expected
 *	MIG001   project content format behind the engine: Schemas/Migrations.yml misses a step
 *	MIG002   a leftover legacy pattern
 */
namespace frt::assets
{
inline constexpr uint32 Path001 = DiagnosticCode("PATH001");
inline constexpr uint32 Path002 = DiagnosticCode("PATH002");
inline constexpr uint32 Path003 = DiagnosticCode("PATH003");
inline constexpr uint32 Rule001 = DiagnosticCode("RULE001");
inline constexpr uint32 Rule002 = DiagnosticCode("RULE002");
inline constexpr uint32 Ref006 = DiagnosticCode("REF006");
inline constexpr uint32 Ref007 = DiagnosticCode("REF007");
inline constexpr uint32 Meta001 = DiagnosticCode("META001");
inline constexpr uint32 Meta002 = DiagnosticCode("META002");
inline constexpr uint32 Meta003 = DiagnosticCode("META003");
inline constexpr uint32 Meta004 = DiagnosticCode("META004");
inline constexpr uint32 Meta005 = DiagnosticCode("META005");
inline constexpr uint32 Meta006 = DiagnosticCode("META006");
inline constexpr uint32 Meta007 = DiagnosticCode("META007");
inline constexpr uint32 Meta008 = DiagnosticCode("META008");
inline constexpr uint32 Meta009 = DiagnosticCode("META009");
inline constexpr uint32 Vcs001 = DiagnosticCode("VCS001");
inline constexpr uint32 Vcs002 = DiagnosticCode("VCS002");
inline constexpr uint32 Vcs003 = DiagnosticCode("VCS003");
inline constexpr uint32 Vcs004 = DiagnosticCode("VCS004");
inline constexpr uint32 Mig001 = DiagnosticCode("MIG001");
inline constexpr uint32 Mig002 = DiagnosticCode("MIG002");
}
