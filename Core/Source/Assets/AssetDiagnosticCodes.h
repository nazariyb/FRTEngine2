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
}
