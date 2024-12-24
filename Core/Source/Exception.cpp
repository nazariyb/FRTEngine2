#include "Exception.h"
#include <sstream>


using std::ostringstream;
using std::endl;

namespace frt
{
Exception::Exception(int line, const char* file) noexcept
    : line(line), file(file), hr(-1)
{}

Exception::Exception(int line, const char* file, HRESULT hr) noexcept
    : line(line), file(file), hr(hr)
{}

const char* Exception::What() const noexcept
{
    return what();
}

const char* Exception::GetType() const noexcept
{
    return "FRT Base Exception";
}

HRESULT Exception::GetErrorCode() const noexcept
{
    return hr;
}

std::string Exception::GetErrorDescription() const noexcept
{
    return Exception::TranslateErrorCode(hr);
}

std::string Exception::TranslateErrorCode() noexcept
{
    return TranslateErrorCode(hr);
}

std::string Exception::TranslateErrorCode(HRESULT hr) noexcept
{
    char* pMsgBuf = nullptr;
    const DWORD nMsgLen = FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&pMsgBuf), 0, nullptr
    );

    if (nMsgLen == 0)
    {
        return "Unidentified error code";
    }
    std::string errorString = pMsgBuf;
    LocalFree(pMsgBuf);
    return errorString;
}

int Exception::GetLine() const noexcept
{
    return line;
}

const std::string& Exception::GetFile() const noexcept
{
    return file;
}

std::string Exception::GetOriginString() const noexcept
{
    ostringstream oss;
    oss << "[File]: " << file << endl
        << "[Line]: " << line;
    return oss.str();
}

const char* Exception::what() const noexcept
{
    std::ostringstream oss;
    oss << GetType() << std::endl
        << "[Error Code] 0x" << std::hex << std::uppercase << GetErrorCode()
        << std::dec << " (" << (unsigned long)GetErrorCode() << ")" << std::endl
        << "[Description] " << GetErrorDescription() << std::endl
        << GetOriginString();
    whatBuffer = oss.str();
    return whatBuffer.c_str();
}
}