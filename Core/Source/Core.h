#pragma once

#ifdef FRT_CORE_EXPORTS
#define FRT_CORE_API __declspec(dllexport)
#else
#define FRT_CORE_API __declspec(dllimport)
#endif

int FRT_CORE_API test();
