"""Keeps the asset system's builders pure functions (design §7.3, "The builder contract").

A builder is a pure function of its resolved settings, its recorded inputs and its declared target facets:
the derived-data cache keys its outputs by exactly those. Anything else a builder reads - a file it opens on
its own, the config, the engine's paths, the memory pool, state a global keeps from one run to the next, the
order a hash table happens to iterate in - is an input the key does not see, and a cache hit then serves an
output its inputs no longer produce, on this machine or on the next one. The compiler cannot see that. Each
gate below rejects one pattern that causes it, in every file a builder is made of: FRT.bat builder-hashes
lists them in Local/Build/PipelineSources.txt from the builder table (Core/Source/Assets/BuilderVersions.inl),
shared conversion code included, wherever it lives, and every build of Core writes the list again.

    python -I Tools/Lint/CheckPipeline.py [--list=<file>]

Prints every hit, then a count per gate; exit code 1 when there is any, when the list is missing (build the
engine, or run FRT.bat builder-hashes, first) or names a file that is gone. Comments and string literals are
not checked. ALLOWED holds the few legitimate exemptions, each one use in one file with its reason, never a
whole file. Before it looks at the sources it checks itself against SELF_TEST (code each gate must and must
not flag) and SELF_TEST_EXEMPTIONS, so a regex that stopped matching fails the lint instead of passing
everything. Standard library only.

What the gates cannot see: a mutable global reached through a function of a file outside the list, a hash
table iterated through a reference or a function's result, a file opened by a library the builder calls.
The list grows as builders do; a builder's own sources stay small and plain so that the patterns suffice.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_LIST = os.path.join(ROOT, 'Local', 'Build', 'PipelineSources.txt')

# (name, pattern, why): line patterns, matched on code with comments and literals blanked.
LINE_GATES = [
    ('platform-file', r'\bplatform::(?:Read\w*|Stat|GetFileIdentity|EnumerateDirectory|Write\w*|CAppendFile|RemoveFile|TouchFile'
     r'|MoveToTrash|ReplaceFileAtomic|GetLoadedModulePath)\b',
     'file access the key does not record - a read, a probe, a listing, a write: inputs come through the build context'),
    ('raw-file', r'\bstd::[io]?fstream\b|\b_?w?fopen\b|\bCreateFile(?:2|[AW])?\b',
     'file access past the platform layer: the C and C++ libraries, Win32'),
    ('std-filesystem', r'\bstd::filesystem\b', 'paths and file access: a key never holds a path, a builder never opens files'),
    ('config', r'\bCConfig\b', 'config values are inputs only when recorded (GetConfig), never read directly'),
    ('paths', r'\bpaths::', "the engine's and project's directories differ per machine and checkout"),
    ('memory-pool', r'\bCMemoryPool\b', 'TLSF is single-threaded; builders run on workers'),
    ('tarray', r'\bTArray\b', 'allocates from TLSF, which is single-threaded; builders run on workers'),
    ('trefshared', r'\bTRefShared\b', 'allocates from TLSF, which is single-threaded; builders run on workers'),
]
# Computed per file (see check_unordered_iteration and check_mutable_globals).
UNORDERED_GATE = ('unordered-iteration', 'iteration order of a hash table is not part of any key: sort first')
GLOBAL_GATE = ('mutable-global', 'state kept between runs is an input no key records')
GATE_NAMES = [name for name, _, _ in LINE_GATES] + [UNORDERED_GATE[0], GLOBAL_GATE[0]]

# The exemptions, one use each: (path fragment, gate, pattern, reason, while_unlisted). A line of a file
# whose path contains the fragment is exempt from a line gate when nothing of the gate's match is left
# once every exempt use is cut out of it, so a line that mixes an exempt use with another one is still
# flagged; a hit of a computed gate is exempt when the pattern matches its text. while_unlisted, when
# set, is a path fragment: the exemption lapses as soon as a listed file contains it. Never a whole file:
# a read or a probe added later to an exempt file is flagged like any other.
SHADER_COMPILER = 'Core/Source/Graphics/Render/RHI/ShaderCompiler.'
SHADER_CACHE = 'Core/Source/Graphics/Render/RHI/ShaderCache.'
ALLOWED = [
    # The DXC bridge is the shader step's file access: its include handler plays the build context's
    # ReadFile, and every file it opens is recorded (SShaderBinary::Dependencies), which the shader cache
    # keys by mount path and content hash (§14.5). Shaders stay path-addressed engine code.
    (SHADER_COMPILER, 'std-filesystem', r'\bstd::filesystem::path\b',
     'the path type (parameters, members, path arithmetic): naming a file opens nothing', None),
    (SHADER_COMPILER, 'std-filesystem',
     r'\bstd::filesystem::weakly_canonical\(\s*(?:InShaderRoot|Desc\.SourcePath|resolved)\s*,\s*ec\s*\)',
     'one name for the shader root, the main file and an include that was opened, for the cache to make '
     'mount paths of', None),
    (SHADER_COMPILER, 'platform-file', r'\bplatform::ReadFileText\(\s*Path\b',
     "LoadSourceBlob: the one read of the main file and of each include, every one recorded", None),
    # The include probes: a candidate that exists is read through LoadSourceBlob and recorded with its hash,
    # and one that does not is recorded too, as an "Absent" input (SShaderBinary::FilesMissing), so a header
    # created later at a higher-priority place changes the key instead of being missed (§14.5).
    (SHADER_COMPILER, 'std-filesystem', r'\bstd::filesystem::exists\(\s*candidate\s*,\s*ec\s*\)',
     'include probe whose outcome, found or absent, the cache records as a discovered input', None),
    (SHADER_COMPILER, 'std-filesystem', r'\bstd::filesystem::weakly_canonical\(\s*candidate\s*,\s*ec\s*\)',
     'the one name of an absent candidate, recorded as an "Absent" input', None),
    (SHADER_COMPILER, 'platform-file', r'\bplatform::ReadFileChunksStable\(\s*Path\s*,',
     'hashes what the key records: the bytes of a file DXC read, and the DXC binaries of its identity', None),
    (SHADER_COMPILER, 'platform-file', r'\bplatform::GetLoadedModulePath\(\s*"\s*"\s*\)',
     'GetDxcIdentity: which DXC modules this process loaded, to hash them into the key (the compiler identity)', None),
    (SHADER_COMPILER, 'paths', r'\bpaths::GetEngineDir\(\)',
     "where ThirdParty/Dxc/VERSION.txt is; only its content hash enters the key (the compiler identity)", None),
    # The shader cache: its file access is the build context's bookkeeping - turning paths into mount paths
    # for keys and re-hashing recorded inputs to match a manifest set - never an unrecorded read.
    (SHADER_CACHE, 'std-filesystem', r'\bstd::filesystem::path\b',
     'the path type (parameters, members, path arithmetic): naming a file opens nothing; keys get mount paths',
     None),
    (SHADER_CACHE, 'std-filesystem', r'\bstd::filesystem::weakly_canonical\(\s*Desc\.SourcePath\s*,\s*ec\s*\)',
     'one name for the main file, for ToMountPath', None),
    (SHADER_CACHE, 'platform-file', r'\bplatform::ReadFileChunksStable\(\s*Path\s*,',
     'HashCurrentFile: the race-free re-hash of a recorded input or the main file, to match manifest sets',
     None),
    (SHADER_CACHE, 'platform-file', r'\bplatform::Stat\(\s*Path\s*\)',
     "HashCurrentFile's stats around that re-hash: whether the file is the one a remembered hash was taken of",
     None),
    (SHADER_CACHE, 'platform-file', r'\bplatform::Stat\(\s*\*path\s*\)',
     'IsInputCurrent: whether a recorded "Absent" input is still absent, to match manifest sets', None),
]

EXTENSIONS = ('.h', '.hpp', '.cpp', '.inl', '.c', '.cc', '.cxx')


# ---------------------------------------------------------------------------------------------------------
# Text

def blank(text):
    """The text with comments, string and character literals and preprocessor lines replaced by spaces; line
    breaks stay, so line numbers and columns do too. A literal keeps its quotes, an empty shell."""
    out = []
    i = 0
    n = len(text)
    line_start = True
    while i < n:
        c = text[i]
        if line_start and c in ' \t':
            out.append(c)
            i += 1
            continue
        if line_start and c == '#':
            # A preprocessor line, with its continuations.
            j = i
            while j < n:
                end = text.find('\n', j)
                end = n if end < 0 else end
                if end > 0 and text[end - 1] == '\\' and end < n:
                    j = end + 1
                    continue
                j = end
                break
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
            continue
        line_start = False
        if c == '\n':
            line_start = True
            out.append(c)
            i += 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif c == '"' and i > 0 and text[i - 1] == 'R' and re.match(r'"([^(\s]*)\(', text[i:]):
            delimiter = re.match(r'"([^(\s]*)\(', text[i:]).group(1)
            close = text.find(')' + delimiter + '"', i)
            j = n if close < 0 else close + len(delimiter) + 2
            out.append('"' + re.sub(r'[^\n]', ' ', text[i + 1:j - 1]) + '"')
            i = j
        elif c in '"\'':
            # A digit separator (1'000) starts no character literal.
            if c == '\'' and 0 < i < n - 1 and text[i - 1].isalnum() and text[i + 1].isalnum() and text[i - 1] not in 'uUL8':
                out.append(c)
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j, n - 1)
            out.append(c + ' ' * (j - i - 1) + text[j])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def line_of(text, position):
    return text.count('\n', 0, position) + 1


def matching_angle(text, start):
    """The index just past the '>' that closes the '<' at Start, or -1."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '<':
            depth += 1
        elif text[i] == '>':
            depth -= 1
            if depth == 0:
                return i + 1
        elif text[i] in ';{}':
            return -1
    return -1


# ---------------------------------------------------------------------------------------------------------
# Gates

UNORDERED_TYPE = re.compile(r'\bstd::unordered_(?:map|set|multimap|multiset)\s*<')


def brace_spans(code):
    """{open index: close index} of every matched brace pair of Code."""
    spans = {}
    stack = []
    for i, c in enumerate(code):
        if c == '{':
            stack.append(i)
        elif c == '}' and stack:
            spans[stack.pop()] = i
    return spans


def visible_range(code, spans, position):
    """Where a name declared at Position can be used: to the end of the innermost function body or block
    around it, or the whole text for a declaration at namespace or class scope (a member is used by every
    member function, which may come anywhere after the class)."""
    innermost = None
    for open_index, close_index in spans.items():
        if open_index < position < close_index and (innermost is None or open_index > innermost):
            innermost = open_index
    if innermost is None:
        return 0, len(code)
    header = ' '.join(code[max(0, innermost - 300):innermost].split())
    header = re.split(r'[;{}]', header)[-1]
    if re.search(r'\b(?:class|struct|union|namespace)\b', header) and not re.search(r'\)\s*(?:const\s*)?$', header):
        return 0, len(code)
    return position, spans[innermost]


def unordered_names(code):
    """(name, first, last) of every name declared in Code with a std::unordered_* type, or an alias of one
    (variables, members, parameters), with the range of Code where it is visible."""
    spans = brace_spans(code)
    declared = []
    aliases = set()
    for match in UNORDERED_TYPE.finditer(code):
        end = matching_angle(code, match.end() - 1)
        if end < 0:
            continue
        alias = re.search(r'\busing\s+(\w+)\s*=\s*$', code[max(0, match.start() - 200):match.start()])
        if alias:
            aliases.add(alias.group(1))
            continue
        name = re.match(r'\s*(?:const\s*)?[&*]*\s*(?:const\s+)?(\w+)\s*[;={(,)\[]', code[end:])
        if name:
            declared.append((name.group(1), match.start()))
    for alias in aliases:
        for name in re.finditer(r'\b' + alias + r'\s*(?:const\s*)?[&*]*\s*(\w+)\s*[;={(,)\[]', code):
            declared.append((name.group(1), name.start()))
    result = []
    for name, position in declared:
        # A parameter: visible in the body that follows its parameter list.
        body = re.match(r'[^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[\w:<>,\s*&]+)?\{', code[position:])
        if body and position + body.end() - 1 in spans:
            result.append((name, position, spans[position + body.end() - 1]))
        else:
            result.append((name, *visible_range(code, spans, position)))
    return result


def check_unordered_iteration(code, names):
    """(position, text) of every iteration over one of Names (unordered_names) where it is visible: a
    range-for, begin(), std::begin, an algorithm over the whole container."""
    hits = []
    for name, first, last in names:
        name = re.escape(name)
        patterns = [
            r'\bfor\s*\([^;()]*(?:\([^()]*\)[^;()]*)*:\s*(?:\*?\s*\w+\s*(?:\.|->)\s*)*' + name + r'\s*\)',
            r'\b' + name + r'\s*(?:\.|->)\s*c?r?begin\s*\(',
            r'\bstd::(?:ranges::)?(?:c?r?begin|for_each|copy|transform|accumulate|any_of|all_of|none_of|find_if|count_if)'
            r'\s*\(\s*' + name + r'\b',
        ]
        for pattern in patterns:
            for match in re.finditer(pattern, code):
                if first <= match.start() <= last:
                    hits.append((match.start(), match.group(0)))
    return sorted(set(hits))


DECLARATION_SKIP = re.compile(r'^(?:using|typedef|namespace|static_assert|friend|template|class|struct|union|enum|concept'
                              r'|return|case|default|goto|break|continue|delete|throw|co_return|co_yield|public|private'
                              r'|protected|if|else|for|while|do|switch|try|catch)\b')


def declared_variable(statement):
    """Whether Statement (one declaration statement, without its ';') declares a mutable variable: the
    declared name's type is neither const nor constexpr. Functions (a '(' before any '='), type and alias
    declarations, and anything that is no declaration are not variables."""
    text = re.sub(r'\[\[.*?\]\]', ' ', statement).strip()
    text = re.sub(r'^(?:__declspec\s*\([^)]*\)\s*|FRT_\w+_API\s+)+', '', text)
    if not text or DECLARATION_SKIP.match(text):
        return False
    # Array bounds may call functions (Values[std::size(Table)]): they make no function declaration.
    head = re.sub(r'\[[^\]]*\]', '[]', re.split(r'=|\{', text, maxsplit=1)[0])
    if '(' in head or '::~' in head or 'operator' in head:
        return False
    if not re.match(r'^(?:(?:static|thread_local|inline|extern|constinit|mutable|volatile|const|constexpr)\s+)*'
                    r'[\w:<>,\s*&\[\]]+$', head):
        return False
    words = re.findall(r'\w+|\*|&', head)
    if len(words) < 2 or not re.match(r'^[A-Za-z_]\w*$', words[-1]):
        return False   # "x;" or an expression statement
    if 'constexpr' in words or 'consteval' in words:
        return False
    # The declarator's own constness: after the last '*' when it is a pointer ("const char* P" is mutable).
    last_star = max((i for i, word in enumerate(words) if word == '*'), default=-1)
    return 'const' not in words[last_star + 1:]


def check_mutable_globals(code):
    """(position, text) of every mutable variable at namespace scope, and of every static or thread_local
    mutable variable anywhere (class members and function-local statics are globals too)."""
    hits = []
    stack = ['namespace']     # the kind of each open brace: namespace, class, block, init
    statement_start = 0
    paren_depth = 0
    i = 0
    n = len(code)

    def statement_text(end):
        return code[statement_start:end]

    while i < n:
        c = code[i]
        if c == '(':
            paren_depth += 1
        elif c == ')':
            paren_depth = max(0, paren_depth - 1)
        elif c == '{' and paren_depth == 0:
            header = statement_text(i)
            flat = ' '.join(header.split())
            if stack[-1] == 'init':
                stack.append('init')
            elif re.search(r'\bnamespace\b[\w\s:]*$', flat) or re.search(r'\bextern\s*"\s*"\s*$', flat):
                stack.append('namespace')
                statement_start = i + 1
            elif re.search(r'\b(?:class|struct|union|enum)\b', flat) and '=' not in flat and not re.search(r'\)\s*(?:const\s*)?$', flat):
                stack.append('class')
                statement_start = i + 1
            elif stack[-1] in ('namespace', 'class') and ('=' in flat or re.search(r'[\w\]>]\s*$', flat)) and \
                    not re.search(r'\)\s*(?:const|noexcept|override|final|\s)*(?:->\s*[\w:<>]+\s*)?$', flat):
                stack.append('init')   # a braced initializer: the declaration goes on after it
            else:
                stack.append('block')
                statement_start = i + 1
        elif c == '}' and paren_depth == 0:
            kind = stack.pop() if len(stack) > 1 else 'namespace'
            if kind != 'init':
                statement_start = i + 1
        elif c == ';' and paren_depth == 0 and stack[-1] != 'init':
            statement = statement_text(i)
            flat = ' '.join(statement.split())
            scope = stack[-1]
            is_static = re.match(r'^(?:\[\[.*?\]\]\s*)*(?:inline\s+)?(?:static|thread_local)\b', flat)
            if (scope == 'namespace' or (scope in ('class', 'block') and is_static)) and declared_variable(flat):
                offset = len(statement) - len(statement.lstrip())
                hits.append((statement_start + offset, flat))
            statement_start = i + 1
        i += 1
    return hits


def check_text(text):
    """{gate: [(line, text)]} of one file's text, before exemptions."""
    code = blank(text)
    lines = code.split('\n')
    hits = {name: [] for name in GATE_NAMES}
    for name, pattern, _ in LINE_GATES:
        for number, line in enumerate(lines, 1):
            if re.search(pattern, line):
                hits[name].append((number, line.strip()))
    for position, snippet in check_unordered_iteration(code, unordered_names(code)):
        hits[UNORDERED_GATE[0]].append((line_of(code, position), ' '.join(snippet.split())))
    for position, snippet in check_mutable_globals(code):
        hits[GLOBAL_GATE[0]].append((line_of(code, position), snippet))
    return hits


LINE_GATE_PATTERNS = {name: pattern for name, pattern, _ in LINE_GATES}


def is_allowed(relative, gate, line, listed, allowed=None):
    """Whether the hit of Gate on Line (of the file Relative; Listed: every listed path) is exempt."""
    patterns = []
    for fragment, allowed_gate, pattern, _, unlisted in ALLOWED if allowed is None else allowed:
        lapsed = unlisted is not None and any(unlisted in other for other in listed)
        if fragment in relative and gate == allowed_gate and not lapsed:
            patterns.append(pattern)
    if not patterns:
        return False
    if gate not in LINE_GATE_PATTERNS:
        return any(re.search(pattern, line) for pattern in patterns)
    rest = line
    for pattern in patterns:
        rest = re.sub(pattern, ' ', rest)
    return not re.search(LINE_GATE_PATTERNS[gate], rest)


def check_allowed(allowed):
    """Problems with an exemption list, as text lines: an unknown gate, a pattern that is empty or does not
    compile, or no reason."""
    problems = []
    for entry in allowed:
        if len(entry) != 5:
            problems.append(f'not (fragment, gate, pattern, reason, while_unlisted): {entry!r}')
            continue
        fragment, gate, pattern, reason, _ = entry
        if gate not in GATE_NAMES:
            problems.append(f'unknown gate {gate!r} for {fragment}')
        if not pattern:
            problems.append(f'{fragment} {gate}: no pattern (an exemption names one use, never a whole file)')
        else:
            try:
                re.compile(pattern)
            except re.error as error:
                problems.append(f'{fragment} {gate}: {pattern!r} does not compile: {error}')
        if not reason:
            problems.append(f'{fragment} {gate}: no reason')
    return problems


# ---------------------------------------------------------------------------------------------------------
# Self-test: what each computed gate must flag (first) and must leave alone (second)

SELF_TEST = [
    ('platform-file', 'auto Text = platform::ReadFileText(Path);', 'auto Text = Context.ReadFile(Name); // platform::ReadFileText'),
    ('platform-file', 'if (platform::Stat(Probe)) {}', 'const platform::SFileStat Stat = Recorded.Stat;'),
    ('platform-file', 'platform::EnumerateDirectory(Dir, OnEntry);', 'platform::EStableRead Read = Result;'),
    ('platform-file', 'platform::WriteFileAtomic(Path, Bytes);', 'platform::OutputDebugText(Log);'),
    ('platform-file', 'platform::CAppendFile Log;', 'CAppendFileLike Log;'),
    ('platform-file', 'platform::RemoveFile(Path);', 'RemoveFileFrom(List);'),
    ('platform-file', 'auto Id = platform::GetFileIdentity(Path);', 'platform::SFileIdentity Id;'),
    ('platform-file', 'auto Dll = platform::GetLoadedModulePath("x.dll");', 'auto Dll = Context.ModulePath;'),
    ('raw-file', 'std::ifstream In(Path);', 'std::stringstream Text;'),
    ('raw-file', 'FILE* F = _wfopen(Name, L"rb");', 'const char* Doc = "fopen";'),
    ('raw-file', 'HANDLE H = CreateFileW(Name, 0, 0, nullptr, 0, 0, nullptr);', 'CreateFileLike(Name);'),
    ('std-filesystem', 'std::filesystem::path P;', 'const char* Doc = "std::filesystem";'),
    ('config', 'const CConfig& C = GetConfig();', 'CConfigLike X;'),
    ('paths', 'auto Dir = paths::GetEngineDir();', 'auto Dir = mypaths::Get();'),
    ('memory-pool', 'CMemoryPool& Pool = CMemoryPool::Get();', 'CMemoryPoolStats Stats;'),
    ('tarray', 'TArray<int> Values;', 'TArrayView<int> Values;'),
    ('trefshared', 'TRefShared<SThing> Thing;', 'TRefSharedLike Thing;'),
    ('unordered-iteration',
     'std::unordered_map<FString, int> Counts;\nvoid F () { for (const auto& [k, v] : Counts) {} }',
     'std::unordered_map<FString, int> Counts;\nvoid F () { auto It = Counts.find(K); std::vector<int> V; for (int x : V) {} }'),
    ('unordered-iteration',
     'struct S { std::unordered_set<uint64, SHash> Seen; };\nvoid G (S& s) { auto it = s.Seen.begin(); }',
     'struct S { std::unordered_set<uint64, SHash> Seen; };\nvoid G (S& s) { bool b = s.Seen.contains(1); }'),
    ('unordered-iteration',
     'using TMap = std::unordered_map<int, std::vector<int>>;\nTMap Index;\nvoid H () { for (auto& e : Index) {} }',
     'using TMap = std::unordered_map<int, std::vector<int>>;\nTMap Index;\nvoid H () { Index[1].push_back(2); }'),
    ('mutable-global', 'namespace frt\n{\nint Counter = 0;\n}', 'namespace frt\n{\nconstexpr int Counter = 0;\n}'),
    ('mutable-global', 'namespace\n{\nstd::vector<int> Cache;\n}', 'namespace\n{\nconst std::vector<int> Table = { 1, 2 };\n}'),
    ('mutable-global', 'const char* Name = "x";', 'const char* const Name = "x";'),
    ('mutable-global', 'int F ()\n{\n\tstatic int Calls = 0;\n\treturn ++Calls;\n}',
     'int F ()\n{\n\tstatic constexpr int Limit = 3;\n\tint local = 0;\n\treturn local + Limit;\n}'),
    ('mutable-global', 'struct S\n{\n\tstatic int Count;\n};', 'struct S\n{\n\tstatic bool Parse (int X);\n\tint Count = 0;\n};'),
    ('mutable-global', 'extern int Shared;',
     'struct SField { FString Name; };\nnamespace a::b\n{\nvoid F (int X);\nclass C;\nusing T = int;\n'
     'template <typename X> struct Y { X Value; };\nenum class E : uint8 { A, B };\nconstexpr FStringLiteral K = "k";\n'
     'bool G (FStringView A, FStringView B) noexcept\n{\n\treturn A < B;\n}\n'
     'CKeyWriter::CKeyWriter ()\n{\n\tStack.emplace_back();\n}\n'
     'static constexpr std::array<int, 2> Pair = { 1, 2 };\nvoid H () { for (int i = 0; i < 3; ++i) {} }\n}'),
]


# The exemption rules (is_allowed), against an exemption list of their own: (file, gate, line, other listed
# files, whether exempt).
SELF_TEST_ALLOWED = [
    ('Bridge.', 'std-filesystem', r'\bstd::filesystem::path\b', 'the type', None),
    ('Bridge.', 'std-filesystem', r'\bstd::filesystem::exists\(\s*probe\s*\)', 'a probe', 'Cache.'),
    ('Bridge.', 'mutable-global', r'\bCalls\b', 'a counter', None),
]
SELF_TEST_EXEMPTIONS = [
    ('Src/Bridge.cpp', 'std-filesystem', 'void F (const std::filesystem::path& P);', [], True),
    ('Src/Bridge.cpp', 'std-filesystem', 'std::filesystem::path P = std::filesystem::current_path();', [], False),
    ('Src/Bridge.cpp', 'std-filesystem', 'if (std::filesystem::exists(probe))', [], True),
    ('Src/Bridge.cpp', 'std-filesystem', 'if (std::filesystem::exists(probe))', ['Src/Cache.cpp'], False),
    ('Src/Bridge.cpp', 'std-filesystem', 'if (std::filesystem::exists(other))', [], False),
    ('Src/Bridge.cpp', 'platform-file', 'std::filesystem::path P = platform::ReadFileText(Q);', [], False),
    ('Src/Other.cpp', 'std-filesystem', 'std::filesystem::path P;', [], False),
    ('Src/Bridge.cpp', 'mutable-global', 'static int Calls = 0', [], True),
    ('Src/Bridge.cpp', 'mutable-global', 'static int Runs = 0', [], False),
]


def self_test():
    """Failures of the gates against SELF_TEST, of the exemption rules and of ALLOWED itself, as text
    lines; empty when every case behaves."""
    failures = []
    for gate, flagged, clean in SELF_TEST:
        if not check_text(flagged)[gate]:
            failures.append(f'{gate} misses: {flagged!r}')
        found = check_text(clean)[gate]
        if found:
            failures.append(f'{gate} wrongly flags {found} in: {clean!r}')
    for relative, gate, line, listed, expected in SELF_TEST_EXEMPTIONS:
        if is_allowed(relative, gate, line, [relative] + listed, SELF_TEST_ALLOWED) != expected:
            failures.append(f'exemption: {gate} on {line!r} in {relative} (listed with {listed}) should be '
                            f'{"exempt" if expected else "flagged"}')
    failures += [f'ALLOWED: {problem}' for problem in check_allowed(SELF_TEST_ALLOWED + ALLOWED)]
    return failures


# ---------------------------------------------------------------------------------------------------------

def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    list_file = DEFAULT_LIST
    for argument in sys.argv[1:]:
        if argument.startswith('--list='):
            list_file = os.path.abspath(argument[len('--list='):])
        else:
            print(f'unknown argument {argument!r}: python -I Tools/Lint/CheckPipeline.py [--list=<file>]')
            return 2

    failures = self_test()
    if failures:
        print('the lint itself is broken (SELF_TEST):')
        for failure in failures:
            print(f'  {failure}')
        return 1

    if not os.path.isfile(list_file):
        print(f'no {list_file}: build the engine, or run FRT.bat builder-hashes, first')
        return 1
    with open(list_file, 'r', encoding='utf-8') as f:
        files = [line.strip() for line in f if line.strip() and not line.startswith('#')]

    hits = {name: 0 for name in GATE_NAMES}
    exempt = 0
    missing = []
    for relative in files:
        path = os.path.join(ROOT, relative)
        if not os.path.isfile(path):
            missing.append(relative)
            continue
        if not relative.endswith(EXTENSIONS):
            continue
        with open(path, 'r', encoding='utf-8', errors='replace') as source:
            found = check_text(source.read())
        for gate in GATE_NAMES:
            for number, line in found[gate]:
                if is_allowed(relative, gate, line, files):
                    exempt += 1
                    continue
                hits[gate] += 1
                print(f'{gate:20} {relative}:{number}: {line[:160]}')
    for relative in missing:
        print(f'{"missing":20} {relative}: listed, but not there (a stale list: build the engine, or run FRT.bat builder-hashes)')
    for gate in GATE_NAMES:
        print(f'gate {gate:20} {hits[gate]} hit(s)')
    total = sum(hits.values())
    print(f'total {total} hit(s) in {len(files)} pipeline file(s), {exempt} allowed')
    return 1 if total or missing else 0


if __name__ == '__main__':
    sys.exit(main())
