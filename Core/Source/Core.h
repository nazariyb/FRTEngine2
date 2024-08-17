#pragma once

#ifdef FRT_CORE_EXPORTS
#define FRT_CORE_API __declspec(dllexport)
#else
#define FRT_CORE_API __declspec(dllimport)
#endif

#define NAMESPACE_FRT_START namespace frt{
#define NAMESPACE_FRT_END }

int FRT_CORE_API test();
