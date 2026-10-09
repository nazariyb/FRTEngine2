#pragma once

#include "Core.h"
#include "Windows.h"

#include <exception>

#include "Containers/String.h"


namespace frt
{
class FRT_CORE_API Exception : public std::exception
{
public:
	Exception (int InLine, const char* InFile) noexcept;
	Exception (int InLine, const char* InFile, HRESULT InErrorCode) noexcept;
	Exception (const Exception&) = default;
	Exception& operator= (const Exception&) = default;
	virtual ~Exception () override = default;

	const char* What () const noexcept;
	virtual const char* GetType () const noexcept;

	HRESULT GetErrorCode () const noexcept;
	FString GetErrorDescription () const noexcept;
	FString TranslateErrorCode () noexcept;
	static FString TranslateErrorCode (HRESULT Result) noexcept;

	int GetLine () const noexcept;
	FStringView GetFile () const noexcept;

	FString GetOriginString () const noexcept;

protected:
	mutable FString WhatBuffer;

	virtual const char* what () const noexcept override;

private:
	HRESULT ErrorCode;
	int Line;
	FString File;
};
}


#define EXCEPTION( hr ) Exception(__LINE__, __FILE__, hr);
#define LAST_EXCEPTION( ) Exception(__LINE__, __FILE__, GetLastError());

#define THROW_IF_FAILED( hr ) if (!FAILED(hr)) {} else { frt_assert(false); throw EXCEPTION(hr); }
