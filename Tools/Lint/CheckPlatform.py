"""Keeps Win32, D3D12 and DirectStorage out of the asset system (design §8.15, §15 I27).

FRT is Windows/D3D12 today and meant to run elsewhere later, so every OS facility the asset system
uses is a frt::platform declaration in Core/Source/Platform/*.h with its Windows implementation in
Core/Source/Platform/Windows/. That holds only if asset code never reaches past the declarations: a
`#include <Windows.h>` or a stray `CreateFileW` compiles today (the precompiled header has Windows.h
anyway) and breaks the first port. Each gate below is a pattern that must not appear in the scanned
sources. Core/Source/Platform/Windows/ is the only home for them, and the public platform headers
are scanned too, so no Win32 type leaks into a platform signature.

    python -I Tools/Lint/CheckPlatform.py

Prints every hit, then a count per gate; exit code 1 when there is any. Comments are not checked;
string literals are. Standard library only. Later milestones add their directories to SCANNED (the
registry, the DDC, the loader, AssetTool).
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# Comment stripping is shared with the string lint. -I keeps this directory off sys.path: add it, and
# write no __pycache__ into the source tree.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from CheckStrings import strip_comments  # noqa: E402

# Directories scanned recursively, then single files (globs) scanned as they are: the asset system,
# and the M0 infrastructure it calls (path globs, diagnostics, the log, functions, CPU scopes, the
# memory pool).
SCANNED_DIRS = ['Core/Source/Assets', 'Core/Source/Hash', 'Core/Source/Diagnostics', 'Core/Source/Log',
                'Core/Source/Functional']
SCANNED_FILES = ['Core/Source/Platform/*.h', 'Core/Source/Containers/PathGlob.*', 'Core/Source/Profiler/CpuProfiler.*',
                 'Core/Source/Memory/MemoryPool.*']
EXTENSIONS = ('.h', '.hpp', '.cpp', '.inl')

# OS, graphics and storage headers. Matched case-insensitively: Windows.h and windows.h are one file.
HEADERS = (r'windows|winbase|winnt|winuser|wingdi|winreg|winerror|winternl|winioctl|minwindef|minwinbase|fileapi'
           r'|handleapi|processthreadsapi|synchapi|memoryapi|sysinfoapi|libloaderapi|ioapiset|errhandlingapi'
           r'|shellapi|shlobj|shlobj_core|shobjidl|shobjidl_core|shlwapi|objbase|combaseapi|unknwn|comdef|comip|atlbase'
           r'|wrl|wrl/client|wrl/wrappers/corewrappers|bcrypt|ntstatus|cfapi|ioringapi|winsock2?|ws2tcpip|dbghelp|psapi|tlhelp32'
           r'|d3d12\w*|d3dcompiler|d3dx12\w*|dxgi\w*|dxcapi|dstorage\w*|pix3|wincodec|propvarutil|knownfolders')

# (name, pattern, flags)
GATES = [
    ('os-header', r'#\s*include\s*[<"](' + HEADERS + r')(\.h)?[>"]', re.IGNORECASE),
    # The Windows implementation's own headers include <Windows.h>: the same hole one step removed.
    ('platform-impl', r'#\s*include\s*[<"][^>"]*Platform/Windows/', re.IGNORECASE),
    # Win32 calls that have a frt::platform equivalent, or that asset code has no business making.
    ('win32-call', r'\b(CreateFile(2|[AW])?|_w(fopen|open)|ReadFile(Ex)?|WriteFile(Ex)?|CloseHandle|DeviceIoControl|GetFileAttributes(Ex)?[AW]?'
                   r'|SetFileAttributes[AW]?|GetFileInformationByHandle(Ex)?|GetFileInformationByName|SetFileInformationByHandle'
                   r'|GetFileSizeEx?|NtQueryInformationFile|NtCreateFile'
                   r'|FindFirstFile(Ex)?[AW]?|FindNextFile[AW]?|FindClose|MoveFile(Ex)?[AW]?|ReplaceFile[AW]?|CopyFile(Ex|2)?[AW]?'
                   r'|DeleteFile[AW]?|RemoveDirectory[AW]?|CreateDirectory[AW]?|FlushFileBuffers|LockFileEx?|GetVolumeInformation\w*'
                   r'|GetDriveType[AW]?|GetFinalPathNameByHandle[AW]?|ReadDirectoryChanges(Ex)?W|CreateIoCompletionPort'
                   r'|GetQueuedCompletionStatus(Ex)?|VirtualAlloc(Ex)?|VirtualFree(Ex)?|VirtualProtect(Ex)?|VirtualQuery(Ex)?'
                   r'|HeapAlloc|HeapFree|HeapCreate|GetProcessHeap|LocalAlloc|GlobalAlloc|CreateThread|_beginthreadex'
                   r'|SetThreadPriority|SetThreadDescription|SetThreadAffinityMask|WaitForSingleObject(Ex)?|WaitForMultipleObjects(Ex)?'
                   r'|CreateEvent(Ex)?[AW]?|SetEvent|ResetEvent|CreateMutex(Ex)?[AW]?|CreateSemaphore(Ex)?[AW]?|Sleep(Ex)?'
                   r'|QueryPerformanceCounter|QueryPerformanceFrequency|GetTickCount(64)?|GetSystemTimeAsFileTime|GetLastError'
                   r'|SetLastError|FormatMessage[AW]?|LoadLibrary(Ex)?[AW]?|GetProcAddress|FreeLibrary|GetModuleHandle(Ex)?[AW]?'
                   r'|CreateProcess[AW]?|TerminateProcess|CreateJobObject[AW]?|CreateNamedPipe[AW]?|ConnectNamedPipe'
                   r'|CoInitialize(Ex)?|CoUninitialize|CoCreateInstance|SHFileOperation[AW]?|SHCreateItemFromParsingName'
                   r'|OutputDebugString[AW]?|MessageBox[AW]?|GetEnvironmentVariable[AW]?|BCryptGenRandom|RegGetValue[AW]?'
                   r'|RegOpenKey(Ex)?[AW]?|CfGetSyncRootInfoBy\w+)\s*\(', 0),
    # Win32 and COM types and constants in code: a platform type in a signature or a handle kept in a struct.
    ('win32-type', r'\b(HANDLE|HMODULE|HINSTANCE|HWND|HRESULT|DWORD|BOOL|WORD|LPCWSTR|LPWSTR|LPCSTR|LPSTR|LPVOID'
                   r'|LARGE_INTEGER|ULARGE_INTEGER|FILETIME|OVERLAPPED|SECURITY_ATTRIBUTES|INVALID_HANDLE_VALUE'
                   r'|MAX_PATH|WIN32_FIND_DATA[AW]?|BY_HANDLE_FILE_INFORMATION|FILE_ID_\w+|IFileOperation\w*|IShellItem'
                   r'|ComPtr|IUnknown|REFIID|CRITICAL_SECTION|SRWLOCK|CONDITION_VARIABLE)\b', 0),
    # The graphics and storage APIs belong to the RHI and the I/O backend (§8.15 "What is not abstracted").
    ('gpu-api', r'\b(ID3D12\w+|IDXGI\w+|IDStorage\w+|D3D12_\w+|DXGI_\w+|DSTORAGE_\w+|D3D12CreateDevice|DStorageGetFactory)\b', 0),
]


def scanned_files():
    """Repository-relative paths of every file the gates apply to, sorted."""
    files = set()
    for directory in SCANNED_DIRS:
        for base, _, names in os.walk(os.path.join(ROOT, directory)):
            for name in names:
                if name.endswith(EXTENSIONS):
                    files.add(os.path.relpath(os.path.join(base, name), ROOT).replace(os.sep, '/'))
    for pattern in SCANNED_FILES:
        for path in glob.glob(os.path.join(ROOT, pattern)):
            if path.endswith(EXTENSIONS):
                files.add(os.path.relpath(path, ROOT).replace(os.sep, '/'))
    return sorted(files)


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    hits = {name: 0 for name, _, _ in GATES}
    files = scanned_files()
    for relative in files:
        with open(os.path.join(ROOT, relative), 'r', encoding='utf-8', errors='replace') as source:
            lines = strip_comments(source.read()).split('\n')
        for name, pattern, flags in GATES:
            for number, line in enumerate(lines, 1):
                if re.search(pattern, line, flags):
                    hits[name] += 1
                    print(f'{name:12} {relative}:{number}: {line.strip()[:160]}')
    for name, count in hits.items():
        print(f'gate {name:12} {count} hit(s)')
    total = sum(hits.values())
    print(f'total {total} hit(s) in {len(files)} file(s)')
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
