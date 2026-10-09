#include "Exception.h"

#include "Platform/Platform.h"


namespace frt
{
Exception::Exception (int InLine, const char* InFile) noexcept
	: ErrorCode(-1)
	, Line(InLine)
	, File(InFile)
{}

Exception::Exception (int InLine, const char* InFile, HRESULT InErrorCode) noexcept
	: ErrorCode(InErrorCode)
	, Line(InLine)
	, File(InFile)
{}

const char* Exception::What () const noexcept
{
	return what();
}

const char* Exception::GetType () const noexcept
{
	return "FRT Base Exception";
}

HRESULT Exception::GetErrorCode () const noexcept
{
	return ErrorCode;
}

FString Exception::GetErrorDescription () const noexcept
{
	return Exception::TranslateErrorCode(ErrorCode);
}

FString Exception::TranslateErrorCode () noexcept
{
	return TranslateErrorCode(ErrorCode);
}

FString Exception::TranslateErrorCode (HRESULT Result) noexcept
{
	FString message = platform::GetSystemErrorMessage(static_cast<uint32>(Result));
	if (message.IsEmpty())
	{
		message = "Unidentified error code";
	}
	return message;
}

int Exception::GetLine () const noexcept
{
	return Line;
}

FStringView Exception::GetFile () const noexcept
{
	return File;
}

FString Exception::GetOriginString () const noexcept
{
	return FString::Format("[File]: {}\n[Line]: {}", File, Line);
}

const char* Exception::what () const noexcept
{
	// An HRESULT reads as its bits: 0x887A0005, not a negative number.
	const uint32 code = static_cast<uint32>(GetErrorCode());
	WhatBuffer = FString::Format("{}\n[Error Code] 0x{:X} ({})\n[Description] {}\n{}",
		GetType(), code, code, GetErrorDescription(), GetOriginString());
	return WhatBuffer.CStr();
}
}
