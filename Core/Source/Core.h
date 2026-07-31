#pragma once

#ifdef FRT_CORE_EXPORTS
#define FRT_CORE_API __declspec(dllexport)
#else
#define FRT_CORE_API __declspec(dllimport)
#endif

#define NAMESPACE_FRT_START namespace frt{
#define NAMESPACE_FRT_END }

// The following #defines disable a bunch of unused windows stuff. If you 
// get weird errors when trying to do some windows stuff, try removing some
// (or all) of these defines (it will increase build time though).
#ifndef FULL_WINTARD
#define WIN32_LEAN_AND_MEAN
#define NOGDICAPMASKS
#define NOSYSMETRICS
#define NOMENUS
#define NOICONS
#define NOSYSCOMMANDS
#define NORASTEROPS
#define OEMRESOURCE
#define NOATOM
#define NOCLIPBOARD
#define NOCOLOR
#define NOCTLMGR
#define NODRAWTEXT
#define NOKERNEL
#define NONLS
#define NOMEMMGR
#define NOMETAFILE
#define NOOPENFILE
#define NOSCROLL
#define NOSERVICE
#define NOSOUND
#define NOTEXTMETRIC
#define NOWH
#define NOCOMM
#define NOKANJI
#define NOHELP
#define NOPROFILER
#define NODEFERWINDOWPOS
#define NOMCX
#define NORPC
#define NOPROXYSTUB
#define NOIMAGE
#define NOTAPE
#define NOMINMAX
#endif

#define NOMINMAX

#include "Asserts.h"

#define FRT_STRUCT_PADDING(Bytes) uint8 __Padding[Bytes]

// Emits a compiler warning - not an error - at every call site of the declaration it
// is applied to. C++ has no static_warning, so [[deprecated]] is the portable stand-in
// on MSVC (C4996); GCC and Clang have an attribute meant for exactly this.
//
// Suppress a deliberate call site with a localized #pragma warning(disable: 4996)
// rather than by removing the attribute.
#if defined(__GNUC__) || defined(__clang__)
	#define FRT_WARN_ON_USE(Message) __attribute__((warning(Message)))
#else
	#define FRT_WARN_ON_USE(Message) [[deprecated(Message)]]
#endif
