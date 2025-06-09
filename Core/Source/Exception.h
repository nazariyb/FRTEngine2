#pragma once

#include "Core.h"
#include "Windows.h"

#include <exception>
#include <string>


namespace frt
{
class FRT_CORE_API Exception : public std::exception
{
public:
	Exception (int line, const char* file) noexcept;
	Exception (int line, const char* file, HRESULT hr) noexcept;
	Exception (const Exception&) = delete;
	virtual ~Exception () override = default;

	const char* What () const noexcept;
	virtual const char* GetType () const noexcept;

	HRESULT GetErrorCode () const noexcept;
	std::string GetErrorDescription () const noexcept;
	std::string TranslateErrorCode () noexcept;
	static std::string TranslateErrorCode (HRESULT hr) noexcept;

	int GetLine () const noexcept;
	const std::string& GetFile () const noexcept;

	std::string GetOriginString () const noexcept;

protected:
	mutable std::string whatBuffer;

	virtual const char* what () const noexcept override;

private:
	HRESULT hr;
	int line;
	std::string file;
};
}


#define EXCEPTION( hr ) Exception(__LINE__, __FILE__, hr);
#define LAST_EXCEPTION( ) Exception(__LINE__, __FILE__, GetLastError());

#define THROW_IF_FAILED( hr ) if (!FAILED(hr)) {} else { frt_assert(false); throw EXCEPTION(hr); }
