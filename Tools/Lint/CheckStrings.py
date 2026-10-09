"""Checks the engine's text rules (README.md, "Strings and text") where the compiler cannot.

The string types delete the overloads that would turn engine text into the wrong encoding, but std,
Win32 and third-party calls have no such overloads: `YAML::LoadFile(Path.string())`,
`stbi_load(Name.CStr(), ...)` or `std::filesystem::path(Text.CStr())` compile, pass every ASCII test,
and fail only for a project under a non-ASCII path. Each gate below is a pattern that must not appear
in the engine, the editor, the launcher, the templates' game sources or Core-Test, outside the files
listed with it, each for the reason given there. Comments are not checked; string literals are.

    python -I Tools/Lint/CheckStrings.py [--info]

Prints every hit, then a count per gate; exit code 1 when there is any. --info also lists the files
that still include <string>, <string_view> or <sstream> (allowed, but worth a look). A use the
patterns cannot see: a narrow name stored first and passed on later (`const char* N = Text.CStr();`).
Standard library only.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCANNED = ['Core/Source', 'Editor/Source', 'Launcher/Source', 'Core-Test'] + sorted(
    os.path.relpath(p, ROOT).replace(os.sep, '/') for p in glob.glob(os.path.join(ROOT, 'Templates', '*', 'Source')))
EXTENSIONS = ('.h', '.hpp', '.cpp', '.inl')


def strip_comments(text):
    """The text with // and /* */ comments blanked (line count kept); string and character literals stay."""
    out = []
    i = 0
    n = len(text)
    quote = None
    while i < n:
        c = text[i]
        if quote is None:
            if text.startswith('//', i):
                j = text.find('\n', i)
                if j < 0:
                    break
                i = j
                continue
            if text.startswith('/*', i):
                j = text.find('*/', i + 2)
                comment = text[i:j + 2] if j >= 0 else text[i:]
                out.append('\n' * comment.count('\n'))
                i = j + 2 if j >= 0 else n
                continue
            if c in '"\'':
                if c == '"' and i > 0 and text[i - 1] == 'R':
                    raw = re.match(r'"([^(\s]*)\(', text[i:])
                    if raw:
                        end = text.find(')' + raw.group(1) + '"', i) + len(raw.group(1)) + 2
                        out.append(text[i:end])
                        i = end
                        continue
                # A digit separator (1'000) starts no character literal.
                if c == '\'' and 0 < i < n - 1 and text[i - 1].isalnum() and text[i + 1].isalnum() and text[i - 1] not in 'uL':
                    out.append(c)
                    i += 1
                    continue
                quote = c
            out.append(c)
            i += 1
        else:
            if c == '\\':
                out.append(text[i:i + 2])
                i += 2
                continue
            if c == quote or c == '\n':
                quote = None
            out.append(c)
            i += 1
    return ''.join(out)


# Calls that open or name a file. Text reaches them as CStr(), GetData() or data() (a view's GetData()
# is not even NUL-terminated); a std::wstring_view argument is UTF-16 by type and fine.
PATH_CALLS = (r'(path|ifstream|ofstream|fstream|fopen|exists|is_directory|create_directories|remove|remove_all|rename'
              r'|copy_file|canonical|absolute|ReadFileText|ReadFileBytes|WriteFileText)')
NARROW = r'(CStr|GetData|data)\(\)'
# Third-party calls that open a file by narrow name, whatever produced the name. Each has a UTF-16 route in
# the engine: stbi_load_from_file on _wfopen_s, YAML::Load on platform::ReadFileText, AddFontFromMemoryTTF on
# ReadFileBytes, CImGuiLayer's own ini file I/O, CAssimpWideIOSystem.
NARROW_FILE_APIS = (r'\bstbi_(load|load_16|loadf|info|is_hdr|is_16_bit)\s*\(|\bstbi_write_(png|bmp|tga|jpg|hdr)\s*\('
                    r'|\bYAML::(LoadFile|LoadAllFromFile)\s*\(|\bAddFontFromFileTTF\s*\(|\baiImportFile\w*\s*\('
                    r'|\b(IniFilename|LogFilename)\s*=(?!=|\s*nullptr\b)')

# (name, pattern, files allowed it: path fragments)
GATES = [
    # Engine text is FString / FStringView / FStringLiteral. The string layer itself, the platform layer and the
    # third-party adapters convert at the boundary; the tests listed have std text as their subject (overload
    # sets that must or must not accept it, the platform layer against the CRT).
    ('std-string', r'std::(w?string|w?string_view|u8string)\b',
     ['Containers/StringFwd.h', 'Containers/StringView.', 'Containers/String.', 'Containers/WideString.',
      'Platform/Platform.cpp', 'Platform/Platform.h', 'Assets/Yaml/YamlString.h', 'Assets/Yaml/YamlArchive.cpp',
      'Graphics/AssimpWideIO.', 'Core-Test/Containers/String', 'Core-Test/Unicode/', 'Core-Test/Platform/',
      'Core-Test/Assets/ArchiveTest.cpp']),
    # path.string() narrows through the code page: FString::FromPath(Path) instead.
    ('path-string', r'\.(u8|generic_|generic_u8)?w?string\(\)', []),
    # Wide literals only as direct arguments of a *W call that takes fixed OS protocol text (L"open"), the DXR
    # export names, _wfopen_s modes, and test inputs.
    ('L-literal', r'\bL"',
     ['PathTracerPrograms.cpp', 'Platform/Platform.cpp', 'ToolProcess.cpp', 'ProjectManager.cpp', 'TextureManager.cpp',
      'Core-Test/Containers/String', 'Core-Test/Unicode/', 'Core-Test/Platform/']),
    # UTF-16 buffers belong in frt::platform: a new one moves there instead of joining this list.
    ('wchar_t', r'\bwchar_t\b',
     ['Containers/String', 'Containers/WideString', 'Unicode/', 'Platform/Platform.cpp', 'PathTracerPrograms.cpp',
      'Core-Test/Containers/String', 'Core-Test/Unicode/', 'Core-Test/Platform/', 'Core-Test/Assets/ArchiveTest.cpp']),
    # The converters and helpers the string types replaced.
    ('old-helpers', r'\benum_::EqualsIgnoreCase|\bToLowerAscii\b|text::(ToLowerCopy|TrimCopy|StripQuotes)|PathToUtf8'
                    r'|PathFromUtf8|WidenUtf8|\bWiden\(|WideToUtf8|\bNarrow\(', []),
    # Locale-dependent or throwing parsing: TryParse / To* / ascii:: / unicode:: instead.
    ('std-parse', r'std::to_w?string|std::sto[fild]|std::ato[if]|\bato[if]\(|strto[lfu]l?\(|sscanf|stringstream'
                  r'|std::(isspace|tolower|toupper|isdigit|isalpha)',
     ['Core-Test/Containers/StringFormatTest.cpp']),   # std::ostringstream << View is its subject
    # Narrow CRT and Win32 text: Equals<ESearchCase>, platform::. Asserts.h and Precision.cpp report fatal ASCII
    # errors before anything else is safe to call; Core-Test/Platform checks the platform layer against the CRT.
    ('crt-text', r'strcmp|_stricmp|_wcsicmp|_wcsnicmp|OutputDebugStringA|GetEnvironmentVariableA|_wdupenv_s|_wputenv_s',
     ['Asserts.h', 'Precision.cpp', 'Platform/Platform.cpp', 'Core-Test/Platform/']),
    ('snprintf', r'snprintf|%ls', ['Asserts.h', 'Precision.cpp', 'Containers/String.cpp']),
    # Narrow text into a path or a file stream (decoded with the code page, not as UTF-8): Text.ToPath(). Stops at
    # a stream insertion, so a gtest message ending in CStr() is not one.
    ('narrow-path-call', PATH_CALLS + r'\s*\((?!\s*std::wstring_view\b)(?:(?!<<)[^;])*' + NARROW,
     ['Core-Test/Containers/StringTest.cpp']),   # a concept checking that a narrow path does not compile
    ('narrow-path-join', r'/\s*[\w.:>()\[\]-]*' + NARROW, []),
    ('narrow-path-decl', r'\bpath\s+\w+\s*[({=][^;]*' + NARROW, []),
    ('narrow-stream-decl', r'\bw?[io]?fstream\s+\w+\s*[({][^;]*' + NARROW, []),
    ('narrow-file-api', NARROW_FILE_APIS, []),
    # void* string getters cast back at the call site: ui::Combo takes typed items.
    ('void-getter', r'static_cast<\s*(const\s+)?(std::string|FString)\s*\*>', ['UI/ImGuiString.cpp']),
]
INFO = [('include-text', r'#\s*include\s*<(string|string_view|sstream)>')]


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    hits = {name: 0 for name, _, _ in GATES}
    info = []
    for directory in SCANNED:
        for base, _, files in os.walk(os.path.join(ROOT, directory)):
            for file in sorted(files):
                if not file.endswith(EXTENSIONS):
                    continue
                relative = os.path.relpath(os.path.join(base, file), ROOT).replace(os.sep, '/')
                with open(os.path.join(ROOT, relative), 'r', encoding='utf-8', errors='replace') as source:
                    lines = strip_comments(source.read()).split('\n')
                for name, pattern, allowed in GATES:
                    if any(fragment in relative for fragment in allowed):
                        continue
                    for number, line in enumerate(lines, 1):
                        if re.search(pattern, line):
                            hits[name] += 1
                            print(f'{name:18} {relative}:{number}: {line.strip()[:160]}')
                for name, pattern in INFO:
                    for number, line in enumerate(lines, 1):
                        if re.search(pattern, line):
                            info.append(f'{name:18} {relative}:{number}: {line.strip()}')
    for name, count in hits.items():
        print(f'gate {name:18} {count} hit(s)')
    total = sum(hits.values())
    print(f'total {total} hit(s)')
    if '--info' in sys.argv:
        print('\n'.join(info))
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
