"""FRT.bat verify: the engine's merge gate, run on a developer's machine.

There is no CI, and byte-exact D3D12 captures need a GPU, which hosted runners lack, so the gate is this
script, behind FRT.bat verify (Premake/tools.lua). A milestone branch merges only after it passes. In order:

  1. builds: the engine's solution and the templates' generated again (FRT.bat generate: Premake finds
     source files by glob, so a solution older than the sources would build without the new ones), then
     the engine in Debug and Development (FRT.bat build), and the templates' game modules in both;
  2. Core-Test in Debug and Development;
  3. the lints: Tools/Lint/CheckStrings.py, Tools/Lint/CheckPipeline.py (the builder contract, over the
     files the build listed in Local/Build/PipelineSources.txt), Tools/Lint/CheckPlatform.py when it exists,
     line endings: every file .gitattributes makes LF is LF in the index and in the working tree, every
     -text file is checked out as committed (FRT.bat check-line-endings: the rules FRT.bat setup applies, in
     Premake/tools.lua only), and the builder source guard: no builder's sources changed without a new
     version GUID (FRT.bat check-builders, BUILD003);
  4. AssetTool through FRT.bat assets: newguid --json exits with 0, and its standard output is JSON objects,
     one per line, and nothing else; -Project= names the project, and arguments reach AssetTool unchanged;
     every asset registry verb with --json over Templates/Demo, read-only (scan --ci, status, query, refs,
     referencers, explain, list-rules, dump registry); the registry's golden dumps: dump registry --text --ci
     of the engine (/Engine) and of each template (/Project) in place, on a tree git says is unchanged there,
     equal to Core-Test/Assets/Golden/Registry-*.txt and clean of PATH001-003, RULE001-002 and errors (Core-Test
     compares the same files over temp copies); and every verb of FRT.bat assets help --json and every code of
     Core/Source/Assets/AssetDiagnosticCodes.h is named in Docs/Assets.md;
  5. the editor's smoke test (-Editor:SmokeTest=1) over Templates/Demo and Templates/Blank, in place, in
     Debug and Development, which also checks the asset registry's initial scan and the editor's Assets panel
     (its "[smoke] ok registry:" line; in Demo the panel lists textures); then its steps that edit files, on a
     scratch copy of Templates/Demo made with
     FRT.bat new and a copy of the shaders (Gate.smoke_scratch): a material rewritten must reload and a
     shader include edited must rebuild exactly the programs that include it, through the file watcher;
  6. automation captures of Templates/Demo through the launcher, in Debug and in Development: one raster,
     compared byte for byte with its baseline, and one path traced, compared by relative RMSE - path
     tracing still varies a little from run to run, as its radiance cache inserts with GPU atomics, so a
     path-traced capture beyond the tolerance is taken again (PATH_TRACED_ATTEMPTS). Every process of the
     run shares one derived-data cache, emptied at the start: the first editor start compiles the shaders,
     the second start of each configuration must compile none (its "[shader] cache:" line says so), and
     the captures, taken from the warm cache, must still match (step 5's scratch copy is not counted: debug
     information names the shader root, a copy there, so it compiles its own);
  7. the Release package baseline: FRT.bat package of both templates into a short scratch directory, and
     the same two captures of each packaged game, against the Release baselines;
  8. git status --porcelain is empty at the end: nothing it ran changed a tracked file.
Steps that later milestones bring print "skipped (M<n>)". It ends with a summary table and the timings a
commit message reports (the hosts' "[assets] initial scan:" lines among them: the scan and how long
BuildWorld waited for it), and exits with 1 when any step failed. --nightly also builds Release and runs its
Core-Test.exe over the registry, query and snapshot benchmark suites, the DISABLED_ 100k/1M/100k + 500k ones
included, with FRT_PERF_STRICT=1, which holds design 5.11's figures exactly (Development's Core-Test step judges
the 10k suites by its regression guards).

    python -I Tools/Verify/Verify.py [--quick | --nightly] [--record-baselines] [--baselines=<dir>]
                                     [--pt-tolerance=<rmse>]
    python -I Tools/Verify/Verify.py --update-goldens

  --quick             build Development, run its Core-Test, the lints and AssetTool: the pre-commit subset
  --nightly           everything, and the long checks (placeholders until they exist)
  --record-baselines  record the captures as the baselines instead of comparing with them; a baseline is
                      kept only when two recordings agree, byte for byte for raster, and a configuration's
                      baselines replace its old ones only when every one of its captures recorded
  --baselines=<dir>   where the baselines are (default <engine>/Local/Verify/Baselines): Debug/ and
                      Development/ for the in-place captures, Release/ for the packaged games'
  --pt-tolerance=<x>  the relative RMSE a path-traced capture may differ by (default 1e-4)
  --update-goldens    write the registry's golden dumps again (Core-Test/Assets/Golden) from FRT.bat assets
                      dump registry, show what changed, and run nothing else: for a commit that changes content

Baselines belong to a machine: a capture depends on the GPU, its driver and the display's scaling, which
sets the window's size. Baseline.ini beside them says what took them, and a comparison with baselines
from another GPU or another recipe fails as such. What a run writes - logs, captures, the smoke tests'
output, and a user-data directory (FRT_USER_DATA_DIR) of its own, new and empty, for every process it
starts, so that no step sees what an earlier one left there, except the derived-data cache they share on
purpose (FRT_DDC_DIR, Last/DDC) - goes to <engine>/Local/Verify/Last, emptied first; packages go to
%TEMP%/frtv-<hash of the engine path>, the scratch copy to %TEMP%/frts-<the same hash> (short paths: MSBuild
fails on paths beyond MAX_PATH), deleted once its step passed.

The captures pin what would otherwise depend on the machine's state or on wall-clock time: the render
mode, a windowed window, and manual exposure. Auto exposure adapts by measured frame times, and the
raster preview's TAA history keeps a trace of every frame's exposure, so with auto exposure a raster
capture repeats only while every run has the same frame times - not on a busy machine. So no capture
covers auto exposure yet, which every run's summary says (see CAPTURE_ARGUMENTS).

Standard library only.
"""
import argparse
import array
import datetime
import difflib
import hashlib
import json
import math
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BINARIES = ROOT / 'Binaries' / 'Win64'
FRT = ROOT / 'FRT.bat'
# What FRT.bat runs, for a check that must start Premake without a batch file in between.
PREMAKE = ROOT / 'ThirdParty' / 'premake-5.0.0-beta2-windows' / 'premake5.exe'
TEMPLATES = ('Demo', 'Blank')
CAPTURED_IN_PLACE = ('Demo',)
# The scratch copy's template, its name (FRT.bat new) and the configuration its smoke steps run in: one is
# enough for steps that test the file watcher, and Development compiles the copy's shaders fastest.
SCRATCH_TEMPLATE = 'Demo'
SCRATCH_NAME = 'Scratch'
SCRATCH_CONFIG = 'Development'
CAPTURE_CONFIGS = ('Debug', 'Development')
# The registry's golden dumps (design 5.3): (golden name, template or None for the engine, dump arguments). Each
# dumps only its own mounts, so an engine content change touches only Registry-Engine.txt.
GOLDEN_DIR = ROOT / 'Core-Test' / 'Assets' / 'Golden'
GOLDENS = (
    ('Engine', None, ('--engine', '--mount=Engine')),
    ('Demo', 'Demo', ('--mount=Project',)),
    ('Blank', 'Blank', ('--mount=Project',)),
)
# Templates whose in-place smoke test must see textures in the editor's Assets panel (type:Texture).
SMOKE_TEXTURES = ('Demo',)
# Core-Test's tests the gate relies on: they must exist and not be disabled (each Core-Test step runs them).
REGISTRY_GATE_TESTS = (
    'RegistryGoldenTest.*', 'AssetQueryParseTest.RandomTokenStreamsNeverCrashAndEveryErrorHasAPosition',
    'RegistryBenchmarkTest.TheStoreMeetsItsBudgetsAt10kAssets', 'QueryBenchmarkTest.TheQueryEngineMeetsItsBudgetsAt10kAssets',
    'SnapshotTest.SnapshotAndJournalMeetTheirBudgetsAt10kAssets',
    'AssetRegistryBudget.TwentyThousandFilesWarmStartWithinTheirBudgetAlsoAfterAMountRuleEdit',
)
# --nightly: the registry's benchmark suites in Release, the DISABLED_ ones included.
NIGHTLY_REGISTRY_FILTER = ':'.join((
    'RegistryBenchmarkTest.*', 'QueryBenchmarkTest.*', 'SnapshotTest.*AndJournal*', 'AssetRegistryBudget.*',
    'RegistryRulesEditBenchmark.*', 'DataTreeYamlTest.*Rate*', 'MountScannerTest.*Budget*', 'AnalyzeBenchmarkTest.*',
))
NIGHTLY_TIMEOUT = 7200
SMOKE_CONFIGS = ('Debug', 'Development')
CAPTURE_FRAME = 400
# ||capture - baseline|| / ||baseline|| over every channel of every pixel of the linear HDR capture. Two
# path-traced runs of identical binaries differ by up to about 1e-4 (commit fee4062), more while other
# work shares the GPU, which reorders the radiance cache's atomics further: a path-traced capture beyond
# the tolerance is taken again, up to PATH_TRACED_ATTEMPTS times. A real change moves the image by far
# more than the noise, so it fails every attempt; the summary shows the attempts a pass needed.
PATH_TRACED_TOLERANCE = 1e-4
PATH_TRACED_ATTEMPTS = 5

# Every capture: frame-indexed, manual exposure, windowed.
#
# TODO(renderer): manual exposure leaves auto exposure (ExposureAdapt.hlsl, and its part of
# PostProcess.cpp) without a capture, so a regression there passes the gate. It adapts by
# g_Frame.DeltaTime, which nothing sets, so the shader falls back to the CPU's wall-clock frame time
# (PostProcess.cpp: FallbackDeltaTime) and a capture with auto exposure differs from run to run. Once a
# renderer change - announced, and judged on its own - sets FrameData.DeltaTime from the automation's
# fixed step, add a raster capture kind with auto exposure to CAPTURE_KINDS (or take the pin off the
# raster kind), record the baselines again, and drop the 'capture with auto exposure' line in Gate.run.
CAPTURE_ARGUMENTS = (
    f'-Automation:CaptureFrame={CAPTURE_FRAME}',
    f'-Automation:ExitFrame={CAPTURE_FRAME}',
    '-Render:AutoExposure=false',
    '-Render:ExposureEV=3',
    '-Display:FullscreenMode=Windowed',
)
# (name, arguments, byte-exact)
CAPTURE_KINDS = (
    ('Raster', ('-Render:RenderMode=Raster',), True),
    ('PathTraced', ('-Render:RenderMode=Raytracing',), False),
)

# cl.exe crashes now and then on some machines with these, and a retry builds (internal compiler errors,
# corrupt PCH reads); any other error fails the build at once.
FLAKY_COMPILER = re.compile(rb'\b(C1060|C1001|C2018|C2026|C1907|C2001)\b')
BUILD_ATTEMPTS = 4

# How long a process may run before it counts as hung, and is killed with everything it started. A build
# or a package (a Release build of a game, then a copy) gets room for a build from scratch; one that runs
# past it is a hung compiler, MSBuild node or Premake, and fails without the retries a flaky compiler gets.
BUILD_TIMEOUT = 3600
PACKAGE_TIMEOUT = 3600
CAPTURE_TIMEOUT = 900
SMOKE_TIMEOUT = 900
TEST_TIMEOUT = 1800
LINT_TIMEOUT = 600
GENERATE_TIMEOUT = 600


def log_name(text):
    return re.sub(r'[^A-Za-z0-9]+', '-', text).strip('-')


class StepFailed(Exception):
    """A step's failure, with the line the summary shows."""


# ---------------------------------------------------------------------------------------------------------
# Processes

def kill_tree(process):
    # A build runs cmd, Premake, MSBuild and compilers: the whole tree goes.
    subprocess.run(['taskkill', '/T', '/F', '/PID', str(process.pid)], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def run(command, log, cwd=None, env=None, timeout=None, on_line=None, stdout=None):
    """Runs Command (a list) with its output appended to Log. Returns (exit code, seconds); the code is None
    when it ran past Timeout and was killed. OnLine(line bytes, seconds since start) sees each line, a
    little after it was written. With Stdout (a path), standard output goes to that file instead, emptied
    first, and only standard error to Log: for a command whose standard output is data.

    The output goes straight into the file, never through a pipe: a build leaves processes behind
    (mspdbsrv, MSBuild nodes) that inherit the output handle, and a pipe would stay open as long as they
    live. A file is read back while the process runs instead."""
    command = [str(part) for part in command]
    start = time.monotonic()
    with open(log, 'ab') as out, open(stdout or os.devnull, 'wb') as data:
        out.write(f'$ {subprocess.list2cmdline(command)}{f" > {stdout}" if stdout else ""}\n'.encode('utf-8'))
        out.flush()
        offset = out.tell()
        process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=data if stdout else out,
                                   stderr=out if stdout else subprocess.STDOUT)
        timed_out = False
        pending = b''
        with open(log, 'rb') as reader:
            reader.seek(offset)
            while True:
                code = process.poll()
                if on_line:
                    pending += reader.read()
                    *lines, pending = pending.split(b'\n')
                    for line in lines:
                        on_line(line + b'\n', time.monotonic() - start)
                if code is not None:
                    break
                if timeout and time.monotonic() - start > timeout:
                    timed_out = True
                    kill_tree(process)
                    code = process.wait()
                    break
                time.sleep(0.05)
            if on_line and pending:
                on_line(pending + reader.read(), time.monotonic() - start)
        seconds = time.monotonic() - start
        out.seek(0, os.SEEK_END)
        out.write(f'# exit code {code} after {seconds:.1f} s{" (timed out)" if timed_out else ""}\n\n'
                  .encode('utf-8'))
    return (None if timed_out else code), seconds


def output_of(command, cwd=ROOT):
    """A quick command's standard output (bytes), or None when it fails."""
    try:
        result = subprocess.run([str(part) for part in command], cwd=cwd, stdin=subprocess.DEVNULL,
                                capture_output=True)
    except OSError:
        return None
    return result.stdout if result.returncode == 0 else None


def tail(log, since=0, lines=12, pattern=None):
    """The last Lines lines of Log from byte Since on, or the first lines matching Pattern when it is given."""
    with open(log, 'rb') as f:
        f.seek(since)
        text = [line for line in f.read().decode('utf-8', errors='replace').splitlines()
                if line.strip() and not line.startswith(('$ ', '# exit code '))]
    if pattern:
        matching = [line.strip() for line in text if re.search(pattern, line)]
        if matching:
            return matching[:lines]
    return [line.rstrip() for line in text[-lines:]]


def git(*arguments):
    return output_of(['git', '-C', ROOT, *arguments])


def remove_tree(path):
    """Deletes the directory Path with everything in it, read-only files included (git makes its objects
    read-only, and a scratch copy is a git repository). Nothing when it does not exist."""
    def make_writable_and_retry(function, failed, *_):
        try:
            os.chmod(failed, stat.S_IWRITE)
            function(failed)
        except OSError:
            pass

    if not Path(path).exists():
        return
    if sys.version_info >= (3, 12):
        shutil.rmtree(path, onexc=make_writable_and_retry)
    else:
        shutil.rmtree(path, onerror=make_writable_and_retry)


# ---------------------------------------------------------------------------------------------------------
# Images: PFM (the linear HDR capture) parsed here; PNG compared as bytes only

def read_pfm(path):
    """(width, height, channels, floats) of a PFM file: "PF" (RGB) or "Pf" (grey), a negative scale for
    little-endian data, rows bottom to top."""
    data = Path(path).read_bytes()
    tokens = []
    position = 0
    while len(tokens) < 4:
        while position < len(data) and data[position:position + 1].isspace():
            position += 1
        start = position
        while position < len(data) and not data[position:position + 1].isspace():
            position += 1
        if start == position:
            raise ValueError(f'{path}: not a PFM file')
        tokens.append(data[start:position].decode('ascii', errors='replace'))
    position += 1   # the single whitespace character after the scale
    kind, width, height, scale = tokens[0], int(tokens[1]), int(tokens[2]), float(tokens[3])
    if kind not in ('PF', 'Pf'):
        raise ValueError(f'{path}: not a PFM file')
    channels = 3 if kind == 'PF' else 1
    count = width * height * channels
    if len(data) - position < count * 4:
        raise ValueError(f'{path}: truncated')
    floats = array.array('f')
    floats.frombytes(data[position:position + count * 4])
    if (scale < 0) != (sys.byteorder == 'little'):
        floats.byteswap()
    return width, height, channels, floats


def compare_pfm(capture, baseline):
    """(relative RMSE, max abs difference, size text) of two PFM files; raises ValueError when they cannot
    be compared."""
    w1, h1, c1, a = read_pfm(capture)
    w2, h2, c2, b = read_pfm(baseline)
    if (w1, h1, c1) != (w2, h2, c2):
        raise ValueError(f'the capture is {w1}x{h1}, the baseline {w2}x{h2} (display scaling or window size '
                         f'changed? then record the baselines again)')
    error = 0.0
    reference = 0.0
    largest = 0.0
    for x, y in zip(a, b):
        d = x - y
        error += d * d
        reference += y * y
        if abs(d) > largest:
            largest = abs(d)
    if not (math.isfinite(error) and math.isfinite(reference)):
        raise ValueError('non-finite values in the capture or the baseline')
    relative = math.sqrt(error / reference) if reference > 0.0 else (0.0 if error == 0.0 else math.inf)
    return relative, largest, f'{w1}x{h1}'


def same_bytes(a, b):
    return Path(a).read_bytes() == Path(b).read_bytes()


# ---------------------------------------------------------------------------------------------------------
# The gate

class Gate:
    def __init__(self, options):
        self.options = options
        self.mode = 'quick' if options.quick else ('nightly' if options.nightly else 'full')
        self.record = options.record_baselines
        self.tolerance = options.pt_tolerance
        self.baselines = Path(options.baselines) if options.baselines else ROOT / 'Local' / 'Verify' / 'Baselines'
        self.run_dir = ROOT / 'Local' / 'Verify' / 'Last'
        self.logs = self.run_dir / 'Logs'
        engine_hash = hashlib.sha1(os.path.normcase(str(ROOT)).encode('utf-8')).hexdigest()[:8]
        self.package_root = Path(tempfile.gettempdir()) / ('frtv-' + engine_hash)
        self.scratch_root = Path(tempfile.gettempdir()) / ('frts-' + engine_hash)
        self.results = []    # (name, status, seconds, detail)
        self.timings = []    # (what, text)
        self.broken = set()  # what failed and so cannot be used: ('engine', config), (template, config)
        self.gpu = None
        self.step_number = 0
        # (what, files, records, scan ms, BuildWorld wait ms) of every host start that logged its initial scan.
        self.initial_scans = []

        env = dict(os.environ)
        # Never the user's own; every process gets a directory of its own below this one (env_for).
        env['FRT_USER_DATA_DIR'] = str(self.run_dir / 'UserData')
        # One derived-data cache for every process of the run, never the machine's (and not below a
        # process's own user-data directory): emptied with run_dir at the start, so the first editor start
        # compiles every shader and the later ones read them back - which check_shader_cache checks, and
        # under which the captures must still match their baselines byte for byte.
        self.ddc_dir = self.run_dir / 'DDC'
        env['FRT_DDC_DIR'] = str(self.ddc_dir)
        # The [shader] cache: line of every editor or game start that printed one, per configuration, in
        # the order they ran: (what, hits, compiled, milliseconds).
        self.shader_starts = {}
        # The templates find the engine they are in; an override would send them to another one.
        env.pop('FRT_ENGINE_DIR', None)
        # GoogleTest reads its flags from the environment too: a GTEST_FILTER, GTEST_REPEAT or
        # GTEST_SHARD_* left in the shell would run a subset of Core-Test, and the gate would pass on it.
        for key in [key for key in env if key.upper().startswith('GTEST_')]:
            env.pop(key)
        self.env = env

    def env_for(self, log):
        """The environment of the process whose output goes to Log: FRT_USER_DATA_DIR is a directory of its
        own, new and empty, named after the log. What one process writes there - the editor's state, a
        game's settings and captures, the engine registry - would otherwise reach every later one, and a
        step's result would depend on which steps ran before it, and differ when it runs alone or after a
        step that did not run."""
        user_data = self.run_dir / 'UserData' / Path(log).stem
        shutil.rmtree(user_data, ignore_errors=True)
        user_data.mkdir(parents=True, exist_ok=True)
        return dict(self.env, FRT_USER_DATA_DIR=str(user_data))

    # ---- bookkeeping ----

    def log_path(self, name):
        self.step_number += 1
        return self.logs / f'{self.step_number:02d}-{log_name(name)}.log'

    def report(self, name, status, seconds, detail=''):
        self.results.append((name, status, seconds, detail))
        time_text = f'{seconds:7.1f} s' if seconds is not None else ' ' * 9
        print(f'[verify] {name:<40} {status:<16} {time_text}  {detail}', flush=True)

    def step(self, name, function, *arguments, needs=()):
        """Runs Function(log, *Arguments), which returns a detail text or raises StepFailed."""
        missing = [what for what in needs if what in self.broken]
        if missing:
            self.report(name, 'not run', None, 'needs ' + ', '.join(' '.join(what) for what in missing))
            return False
        log = self.log_path(name)
        start = time.monotonic()
        try:
            detail = function(log, *arguments)
            self.report(name, 'ok', time.monotonic() - start, detail or '')
            return True
        except StepFailed as failure:
            self.report(name, 'FAILED', time.monotonic() - start, str(failure))
            if log.exists():
                print(f'           log: {log}')
            return False
        except Exception as error:   # a bug here, or the machine: the step fails, the gate goes on
            self.report(name, 'FAILED', time.monotonic() - start, f'{type(error).__name__}: {error}')
            return False

    def skip(self, name, why):
        self.report(name, why, None)

    # ---- builds ----

    def generate(self, log, *arguments):
        """FRT.bat generate, with the generation options this checkout remembers. FRT.bat build generates
        only a missing solution, and Premake adds source files by glob when it generates, so without this a
        branch whose solution predates its new sources (a new test file, a new project) would build and
        test without them, and pass."""
        code, _ = run([FRT, 'generate', *arguments], log, cwd=ROOT, env=self.env_for(log), timeout=GENERATE_TIMEOUT)
        if code != 0:
            errors = tail(log, lines=3, pattern=r'error|failed|cannot')
            raise StepFailed(('timed out; ' if code is None else '') +
                             ('; '.join(errors) if errors else f'exit code {code}'))

    def generate_engine(self):
        if not self.step('generate engine', self.generate):
            self.broken.add(('generate', 'engine'))

    def generate_template(self, name):
        project = ROOT / 'Templates' / name / f'{name}.frtproject'
        if not self.step(f'generate {name}', self.generate, f'--project={project}'):
            self.broken.add(('generate', name))

    def build(self, log, *arguments, env=None):
        """FRT.bat build with Arguments, retried when the compiler crashed (FLAKY_COMPILER). Env: the
        environment of every attempt; by default each gets a new user-data directory (env_for)."""
        retries = []
        for attempt in range(BUILD_ATTEMPTS):
            since = log.stat().st_size if log.exists() else 0
            code, _ = run([FRT, 'build', *arguments], log, cwd=ROOT, env=env or self.env_for(log), timeout=BUILD_TIMEOUT)
            if code == 0:
                return f'after {len(retries)} retr{"y" if len(retries) == 1 else "ies"} ({", ".join(retries)})' \
                    if retries else ''
            if code is None:
                raise StepFailed(f'timed out after {BUILD_TIMEOUT / 60:g} min and was killed (a hung compiler, '
                                 f'MSBuild node or Premake?)' + (f'; retried before: {", ".join(retries)}'
                                                                if retries else ''))
            with open(log, 'rb') as f:
                f.seek(since)
                flaky = FLAKY_COMPILER.search(f.read())
            if not flaky or attempt + 1 == BUILD_ATTEMPTS:
                break
            retries.append(flaky.group(1).decode('ascii'))
        errors = tail(log, since, lines=4, pattern=r'\berror\b|\bfailed\b')
        raise StepFailed('; '.join(errors) if errors else f'exit code {code}')

    def build_engine(self, config):
        if not self.step(f'build engine ({config})', self.build, f'--config={config}', needs=[('generate', 'engine')]):
            self.broken.add(('engine', config))

    def build_template(self, name, config):
        project = ROOT / 'Templates' / name / f'{name}.frtproject'
        if not self.step(f'build {name} ({config})', self.build, f'--project={project}', f'--config={config}',
                         needs=[('generate', name), ('engine', config)]):
            self.broken.add((name, config))

    # ---- Core-Test and the lints ----

    def core_test(self, log, config):
        exe = BINARIES / f'Core-Test-{config}-Default.exe'
        if not exe.exists():
            raise StepFailed(f'no {exe.name}')
        code, seconds = run([exe, '--gtest_brief=1'], log, cwd=ROOT, env=self.env_for(log), timeout=TEST_TIMEOUT)
        text = Path(log).read_bytes().decode('utf-8', errors='replace')
        passed = re.findall(r'\[  PASSED  \] (\d+) test', text)
        failed = re.findall(r'\[  FAILED  \] (\d+) test', text)
        skipped = re.findall(r'\[  SKIPPED \] (\d+) test', text)
        disabled = re.findall(r'YOU HAVE (\d+) DISABLED TEST', text)
        counts = f'{passed[-1] if passed else 0} passed'
        if skipped:
            counts += f', {skipped[-1]} skipped'
        if disabled:
            counts += f', {disabled[-1]} disabled'
        self.timings.append((f'Core-Test {config}', f'{seconds:.1f} s'))
        if code != 0:
            names = re.findall(r'\[  FAILED  \] (\S+)', text)
            raise StepFailed(('timed out; ' if code is None else '') + counts +
                             (f', {failed[-1]} failed: ' + ', '.join(names[:5]) if failed else f', exit code {code}'))
        missing = self.missing_gate_tests(exe, log)
        if missing:
            raise StepFailed(counts + "; the gate's tests are missing or disabled: " + ', '.join(missing))
        return counts

    def missing_gate_tests(self, exe, log):
        """The REGISTRY_GATE_TESTS patterns that name no enabled test of Exe: the golden dumps, the query fuzz test
        and the 10k registry budgets must run in every Core-Test step, so a rename or a DISABLED_ fails the step."""
        listing = Path(log).with_suffix('.tests')
        missing = []
        for pattern in REGISTRY_GATE_TESTS:
            code, _ = run([exe, '--gtest_list_tests', f'--gtest_filter={pattern}'], listing, cwd=ROOT, env=self.env_for(log),
                          timeout=LINT_TIMEOUT)
            names = [line for line in listing.read_bytes().decode('utf-8', errors='replace').splitlines()
                     if line.startswith('  ') and not line.strip().startswith('DISABLED_')]
            if code != 0 or not names:
                missing.append(pattern)
            listing.unlink(missing_ok=True)
        return missing

    def lint(self, log, script):
        code, _ = run([sys.executable, '-I', ROOT / script], log, cwd=ROOT, env=self.env_for(log), timeout=LINT_TIMEOUT)
        if code != 0:
            raise StepFailed(('timed out; ' if code is None else '') + '; '.join(tail(log, lines=3)))
        last = tail(log, lines=1)
        return last[0].strip() if last else ''

    def line_endings(self, log):
        """FRT.bat check-line-endings: the rules live in Premake/tools.lua alone, beside setup, which fixes
        what they find. Setup must work before Python is installed, so the check is not written here."""
        code, _ = run([FRT, 'check-line-endings'], log, cwd=ROOT, env=self.env_for(log), timeout=LINT_TIMEOUT)
        text = Path(log).read_bytes().decode('utf-8', errors='replace')
        if code != 0:
            raise StepFailed(('timed out; ' if code is None else '') + '; '.join(tail(log, lines=4)))
        summary = re.findall(r'Line endings: (.*?)\.?\s*$', text, re.MULTILINE)
        return summary[-1] if summary else ''

    def check_builders(self, log):
        """FRT.bat check-builders (BUILD003): every builder whose sources changed since the hash committed with
        its version has a new version GUID and the new hash recorded. Lua in Premake/tools.lua, beside the
        builder-hashes step every build runs, so the hash is computed in one place."""
        code, _ = run([FRT, 'check-builders'], log, cwd=ROOT, env=self.env_for(log), timeout=LINT_TIMEOUT)
        text = Path(log).read_bytes().decode('utf-8', errors='replace')
        if code != 0:
            problems = re.findall(r'^(BUILD003 \S+|\S+ \(Core/Source/Assets/BuilderVersions\.inl:\d+\))', text, re.MULTILINE)
            raise StepFailed(('timed out; ' if code is None else '') +
                             ('; '.join(problems[:3]) if problems else '; '.join(tail(log, lines=3))))
        summary = re.findall(r'Builders: (.*?)\.?\s*$', text, re.MULTILINE)
        return summary[-1] if summary else ''

    # ---- AssetTool ----

    def assets_json(self, log, command, expected_code, timeout=BUILD_TIMEOUT):
        """Runs Command, an FRT.bat assets command with --json, the way a script runs it: standard output to a
        file of its own, standard error to Log. Raises StepFailed unless it exits with Expected Code and every
        line of its standard output is one JSON object - nothing else, not a line from Premake or from a build
        of AssetTool (those belong on standard error). Returns the objects."""
        output = Path(log).with_suffix('.stdout')
        code, _ = run(command, log, cwd=ROOT, env=self.env_for(log), timeout=timeout, stdout=output)
        text = output.read_bytes().decode('utf-8', errors='replace')
        if code != expected_code:
            raise StepFailed(('timed out; ' if code is None else f'exit code {code}, not {expected_code}; ') +
                             '; '.join(tail(log, lines=3) + text.splitlines()[:2]))
        objects = []
        for line in text.splitlines():
            try:
                value = json.loads(line)
            except ValueError as error:
                raise StepFailed(f'standard output has a line that is no JSON: {line[:80]!r} ({error})')
            if not isinstance(value, dict):
                raise StepFailed(f'standard output has a line that is no JSON object: {line[:80]!r}')
            objects.append(value)
        return objects

    def assets_newguid(self, log):
        """FRT.bat assets newguid 3 --json: exit code 0, and standard output is three lines, each a JSON object
        with the stable fields "guid" (32 lowercase hex digits, all different) and "cpp"."""
        objects = self.assets_json(log, [FRT, 'assets', 'newguid', '3', '--json'], 0)
        if len(objects) != 3:
            raise StepFailed(f'{len(objects)} lines of standard output instead of 3')
        for value in objects:
            if not isinstance(value.get('guid'), str) or not re.fullmatch(r'[0-9a-f]{32}', value['guid']) or \
                    not isinstance(value.get('cpp'), str):
                raise StepFailed(f'no "guid" and "cpp" in {json.dumps(value)[:80]}')
        if len({value['guid'] for value in objects}) != 3:
            raise StepFailed('the same GUID twice')
        return '3 GUIDs as JSON lines'

    # The text of an argument that cmd.exe would change on its way from Premake to AssetTool unless it is
    # escaped for it: variables (%OS% is set everywhere), carets, &, quotes - one after backslashes - a
    # pipe, redirections, parentheses, !NAME! and non-ASCII text.
    CMD_SPECIAL_ARGUMENT = '%OS% 50%% ^caret& "quoted" back\\"slash (a|b) <c> !OS! ї 资产'

    def assets_arguments(self, log):
        """How FRT.bat assets hands its arguments to AssetTool:
          - -Project=<file or directory>, AssetTool's own spelling of --project, names the project (here
            Templates/Blank, which lock locks and lets go of at once, as its input is empty);
          - an argument reaches AssetTool with its text unchanged, through the cmd.exe that Premake starts
            it with: help '<CMD_SPECIAL_ARGUMENT>' --json exits with 2 and names that text in its error.
            Premake runs directly, with no FRT.bat: a batch file expands %NAME% itself, before Premake gets
            to the text, as it does for every command."""
        locked = self.assets_json(log, [FRT, 'assets', '-Project=Templates/Blank', 'lock', '--json'], 0)
        lock_file = str(ROOT / 'Templates' / 'Blank' / 'Local' / 'Assets' / 'Owner.lock')
        if len(locked) != 2 or locked[0].get('locked') != '/Project' or \
                os.path.normcase(locked[0].get('lockFile', '')) != os.path.normcase(lock_file):
            raise StepFailed(f'-Project= did not lock Templates/Blank: {json.dumps(locked)[:120]}')

        argument = self.CMD_SPECIAL_ARGUMENT
        refused = self.assets_json(log, [PREMAKE, f'--file={ROOT / "premake5.lua"}', 'assets', 'help', argument, '--json'], 2)
        expected = f"help: no verb '{argument}' (FRT.bat assets help lists them)"
        if len(refused) != 1 or refused[0].get('error') != expected or refused[0].get('exitCode') != 2:
            raise StepFailed(f'the argument changed on its way: {json.dumps(refused, ensure_ascii=False)[:160]}')
        return '-Project= and an argument cmd.exe would change'

    # ---- the asset registry ----

    def assets_registry_verbs(self, log):
        """Every asset registry verb of FRT.bat assets with --json over Templates/Demo, in place and read-only (--ci:
        only scan could write, and with --ci it writes nothing): each exits with 0 and prints JSON lines with its
        stable fields. The files it asks about come from a query, so nothing here names a file of the template."""
        def verb(*arguments):
            return self.assets_json(log, [FRT, 'assets', '-Project=Templates/Demo', *arguments, '--json', '--ci'], 0)

        scan = verb('scan')
        if len(scan) != 1 or scan[0].get('verb') != 'scan' or scan[0].get('writer') is not False or not scan[0].get('records'):
            raise StepFailed(f'scan --ci: {json.dumps(scan)[:160]}')
        status = verb('status')
        if not any(o.get('kind') == 'summary' for o in status) or not any(o.get('kind') == 'ddc' for o in status):
            raise StepFailed(f'status: no "summary" or "ddc" line in {json.dumps(status)[:160]}')
        textures = verb('query', 'type:Texture', '--columns=$Path,Width')
        if len(textures) < 2 or not textures[-1].get('matched') or 'path' not in textures[0]:
            raise StepFailed(f'query type:Texture: {json.dumps(textures)[:160]}')
        texture = textures[0]['path']
        models = verb('query', 'type:Model', '--columns=$Path,$Dependencies')
        model = next((o['path'] for o in models if o.get('dependencies', 0) > 0), None)
        if model is None:
            raise StepFailed(f'query type:Model: no model with dependencies in {json.dumps(models)[:160]}')
        refs = verb('refs', model)
        if not refs or any(set(o) != {'from', 'target', 'resolved', 'flags', 'field', 'state'} for o in refs):
            raise StepFailed(f'refs {model}: {json.dumps(refs)[:160]}')
        referencers = verb('referencers', texture, '--transitive')
        if any(set(o) != {'path', 'type'} for o in referencers):
            raise StepFailed(f'referencers {texture} --transitive: {json.dumps(referencers)[:160]}')
        explain = verb('explain', texture)
        if len(explain) != 1 or explain[0].get('importer') != 'Texture' or not explain[0].get('chain'):
            raise StepFailed(f'explain {texture}: {json.dumps(explain)[:160]}')
        rules = verb('list-rules')
        if not rules or rules[0] != {'format': 'frt.rules.cache', 'version': 1}:
            raise StepFailed(f'list-rules: {json.dumps(rules)[:160]}')
        dump = verb('dump', 'registry', '--mount=Project')
        records = sum(1 for o in dump if o.get('kind') == 'record')
        if not records or dump[0] != {'kind': 'mount', 'mount': '/Project'}:
            raise StepFailed(f'dump registry --mount=Project: {json.dumps(dump[:2])[:160]}')
        return (f'8 verbs: {scan[0]["records"]} records, {textures[-1]["matched"]} textures, {len(refs)} edge(s) of a model, '
                f'{records} records dumped')

    def golden_dump(self, log, name, template, arguments):
        """FRT.bat assets dump registry --text --ci for one golden dump (GOLDENS), in place, with LF line endings."""
        output = Path(log).with_suffix(f'.{name}.txt')
        command = [FRT, 'assets', *([f'-Project=Templates/{template}'] if template else []), 'dump', 'registry', '--text', '--ci', *arguments]
        code, _ = run(command, log, cwd=ROOT, env=self.env_for(log), timeout=BUILD_TIMEOUT, stdout=output)
        if code != 0:
            raise StepFailed(f'{name}: ' + ('timed out' if code is None else f'exit code {code}') + '; ' + '; '.join(tail(log, lines=2)))
        return output.read_bytes().decode('utf-8', errors='replace').replace('\r\n', '\n')

    @staticmethod
    def golden_sources(template):
        """What a golden dump reads: the engine's content and rule file, and a template's."""
        return ['Core/Content', 'Core/Config'] + ([f'Templates/{template}/Content', f'Templates/{template}/Config'] if template else [])

    # A dump line that breaks design 1.1 G11 (the engine and the templates are clean of portable-path and rule
    # problems) or that reports an error.
    G11_PROBLEM = re.compile(r'^  problem (?:(?:PATH00[123]|RULE00[12]) |\S+ error )', re.MULTILINE)

    def registry_goldens(self, log):
        """The registry's golden dumps (GOLDENS): dump registry --text --ci in place, on a tree whose dumped content
        git says is unchanged (the runtime can write Content/), equals Core-Test/Assets/Golden/Registry-<name>.txt
        and passes G11. The difference goes into the log; --update-goldens writes the files again."""
        compared = []
        for name, template, arguments in GOLDENS:
            changed = git('status', '--porcelain', '--', *self.golden_sources(template))
            if changed is None:
                raise StepFailed('not a git checkout: the dumped content cannot be checked unchanged')
            if changed.strip():
                raise StepFailed(f'{name}: its content has uncommitted changes, so the dump says nothing about the commit: '
                                 + '; '.join(changed.decode('utf-8', errors='replace').splitlines()[:3]))
            dumped = self.golden_dump(log, name, template, arguments)
            golden_path = GOLDEN_DIR / f'Registry-{name}.txt'
            if not golden_path.exists():
                raise StepFailed(f'no {golden_path.relative_to(ROOT)}: FRT.bat verify --update-goldens writes it')
            golden = golden_path.read_bytes().decode('utf-8', errors='replace').replace('\r\n', '\n')
            if dumped != golden:
                diff = list(difflib.unified_diff(golden.splitlines(), dumped.splitlines(), f'golden/{golden_path.name}',
                                                 f'dumped/{golden_path.name}', lineterm='', n=1))
                with open(log, 'a', encoding='utf-8') as f:
                    f.write('\n'.join(diff[:400]) + '\n')
                first = next((line for line in diff[2:] if line[:1] in '-+'), '')
                raise StepFailed(f'{name}: the dump differs from {golden_path.name} (first: {first[:100]!r}; the diff is in the log); '
                                 f'content changed on purpose: FRT.bat verify --update-goldens')
            broken = self.G11_PROBLEM.findall(dumped)
            if broken:
                raise StepFailed(f'{name}: {len(broken)} problem(s) G11 forbids, first: ' +
                                 next(line for line in dumped.splitlines() if self.G11_PROBLEM.match(line + '\n')).strip()[:120])
            compared.append(name)
        return 'equal to the golden files of ' + ', '.join(compared)

    def update_goldens(self):
        """--update-goldens: each golden dump written again from FRT.bat assets dump registry, what changed shown,
        nothing else run (so no git status step counts the rewritten files)."""
        shutil.rmtree(self.run_dir, ignore_errors=True)
        self.logs.mkdir(parents=True, exist_ok=True)
        (self.run_dir / 'UserData').mkdir(parents=True, exist_ok=True)
        GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
        failed = False
        for name, template, arguments in GOLDENS:
            log = self.log_path(f'golden {name}')
            golden_path = GOLDEN_DIR / f'Registry-{name}.txt'
            changed = git('status', '--porcelain', '--', *self.golden_sources(template))
            if changed and changed.strip():
                print(f'[verify] {name}: its content has uncommitted changes; the golden file will hold them')
            try:
                dumped = self.golden_dump(log, name, template, arguments)
            except StepFailed as failure:
                print(f'[verify] {name}: FAILED: {failure}')
                failed = True
                continue
            old = golden_path.read_bytes().decode('utf-8', errors='replace').replace('\r\n', '\n') if golden_path.exists() else ''
            if old == dumped:
                print(f'[verify] {golden_path.relative_to(ROOT)}: unchanged')
                continue
            golden_path.write_bytes(dumped.encode('utf-8'))
            diff = list(difflib.unified_diff(old.splitlines(), dumped.splitlines(), f'a/{golden_path.name}', f'b/{golden_path.name}',
                                             lineterm='', n=1))
            print(f'[verify] {golden_path.relative_to(ROOT)}: written ({len(dumped.splitlines())} lines)')
            for line in diff[:80]:
                print('  ' + line)
            if len(diff) > 80:
                print(f'  ... {len(diff) - 80} more diff lines (git diff shows them)')
        return 1 if failed else 0

    def docs_coverage(self, log):
        """Design 16.1 item 10: every verb FRT.bat assets help --json lists and every diagnostic code of
        Core/Source/Assets/AssetDiagnosticCodes.h is named in Docs/Assets.md (a verb in backquotes)."""
        verbs = [o['verb'] for o in self.assets_json(log, [FRT, 'assets', 'help', '--json'], 0) if 'verb' in o]
        codes = re.findall(r'DiagnosticCode\("([A-Z]+[0-9]+)"\)',
                           (ROOT / 'Core' / 'Source' / 'Assets' / 'AssetDiagnosticCodes.h').read_text(encoding='utf-8'))
        docs = (ROOT / 'Docs' / 'Assets.md').read_text(encoding='utf-8')
        missing = [f'the verb {v}' for v in verbs if not re.search('`' + re.escape(v) + r'(?![\w-])', docs)]
        missing += [f'the code {c}' for c in codes if not re.search(r'\b' + c + r'\b', docs)]
        if not verbs or not codes:
            raise StepFailed(f'{len(verbs)} verbs and {len(codes)} codes found: nothing to check')
        if missing:
            raise StepFailed('Docs/Assets.md does not name ' + ', '.join(missing))
        return f'{len(verbs)} verbs and {len(codes)} codes named'

    def nightly_registry(self, log):
        """--nightly: Release's Core-Test.exe over the registry, query and snapshot benchmark suites, the DISABLED_
        100k/1M/100k + 500k ones included, with FRT_PERF_STRICT=1 (design 5.11's figures, or the proposed
        revisions, held exactly). Run with nothing else on the machine: the figures are for a quiet one."""
        exe = BINARIES / 'Core-Test.exe'
        if not exe.exists():
            raise StepFailed(f'no {exe.name}')
        env = dict(self.env_for(log), FRT_PERF_STRICT='1')
        code, seconds = run([exe, '--gtest_brief=1', '--gtest_also_run_disabled_tests', f'--gtest_filter={NIGHTLY_REGISTRY_FILTER}'],
                            log, cwd=ROOT, env=env, timeout=NIGHTLY_TIMEOUT)
        text = Path(log).read_bytes().decode('utf-8', errors='replace')
        passed = re.findall(r'\[  PASSED  \] (\d+) test', text)
        missed = len(re.findall(r'\bMISSED\b', text))
        self.timings.append(('registry benchmarks, Release with the DISABLED_ suites', f'{seconds:.1f} s'))
        if code != 0:
            names = re.findall(r'\[  FAILED  \] (\S+)', text)
            raise StepFailed(('timed out; ' if code is None else '') + f'{passed[-1] if passed else 0} passed, failed: ' + ', '.join(names[:5]))
        return f'{passed[-1] if passed else 0} passed; {missed} row(s) print MISSED against 5.11 (see the log)'

    # The hosts' start-up line (CAssetRegistry::WaitBeforeBuildWorld).
    INITIAL_SCAN_LINE = re.compile(r'\[assets\] initial scan: (\d+) files, (\d+) records, ([\d.]+) ms; BuildWorld waited ([\d.]+) ms')

    def read_initial_scan(self, what, log):
        """Records the "[assets] initial scan:" line a host start printed to Log for the summary; None without one
        (Release, or [Assets] Registry=0)."""
        match = self.INITIAL_SCAN_LINE.search(Path(log).read_bytes().decode('utf-8', errors='replace'))
        if match:
            self.initial_scans.append((what, int(match.group(1)), int(match.group(2)), float(match.group(3)), float(match.group(4))))
        return match

    # ---- the shader cache ----

    SHADER_CACHE_LINE = re.compile(r'^\[shader\] cache: (\d+) hits, (\d+) compiled \((\d+) ms\)', re.MULTILINE)

    def check_shader_cache(self, config, what, log):
        """Reads the "[shader] cache: <hits> hits, <compiled> compiled (<ms> ms)" line that a start of Config
        printed to Log after its first frame, and checks the warm start: every process of the run shares one
        DDC (FRT_DDC_DIR, emptied at the start), so the first start of a configuration may compile, and its
        second must compile nothing. Release opens no DDC (its shaders compile at start-up until the cook
        ships DXIL), so its starts are not checked. Returns a detail text, or raises StepFailed."""
        if config == 'Release':
            return ''
        text = Path(log).read_bytes().decode('utf-8', errors='replace')
        match = self.SHADER_CACHE_LINE.search(text)
        if not match:
            raise StepFailed(f'no "[shader] cache:" line: the shader cache did not report (log: {log})')
        hits, compiled, milliseconds = (int(group) for group in match.groups())
        starts = self.shader_starts.setdefault(config, [])
        starts.append((what, hits, compiled, milliseconds))
        if len(starts) <= 2:
            which = 'first' if len(starts) == 1 else 'second'
            self.timings.append((f'shaders at the {which} start ({config}, {what})',
                                 f'{hits} hits, {compiled} compiled, {milliseconds} ms'))
        if len(starts) == 2 and (compiled != 0 or hits == 0):
            raise StepFailed(f'the second start of {config} compiled {compiled} shader(s) ({hits} hits) from a warm '
                             f'DDC ({self.ddc_dir}; the first start, {starts[0][0]}, compiled {starts[0][2]})')
        return f'shaders: {hits} hits, {compiled} compiled'

    # ---- the editor's smoke test ----

    def smoke(self, log, name, config):
        exe = BINARIES / f'Editor-{config}-Default.exe'
        project_dir = ROOT / 'Templates' / name
        output = self.run_dir / 'Smoke' / f'{name}-{config}'
        first_step = []

        def on_line(line, seconds):
            if not first_step and line.startswith(b'[smoke] step:'):
                first_step.append(seconds)

        code, seconds = run([exe, f'-Project={project_dir / (name + ".frtproject")}', '-Editor:SmokeTest=1',
                             f'-Editor:SmokeTestDir={output}', '-Assets:ReadOnly=1'],
                            log, cwd=project_dir, env=self.env_for(log), timeout=SMOKE_TIMEOUT, on_line=on_line)
        if first_step:
            self.timings.append((f'editor start to first frame, {name} ({config})', f'{first_step[0]:.1f} s'))
        self.timings.append((f'editor smoke test, {name} ({config})', f'{seconds:.1f} s'))
        text = Path(log).read_bytes().decode('utf-8', errors='replace')
        checks = len(re.findall(r'^\[smoke\]\s+ok\s', text, re.MULTILINE))
        skips = len(re.findall(r'^\[smoke\]\s+skip\s', text, re.MULTILINE))
        if code != 0:
            failures = re.findall(r'^\[smoke\]\s+FAIL\s+(.*)$', text, re.MULTILINE)
            raise StepFailed(('timed out' if code is None else f'exit code {code}') +
                             (': ' + '; '.join(f.strip() for f in failures[:3]) if failures else ''))
        shaders = self.check_shader_cache(config, f'smoke {name}', log)
        registry = self.check_smoke_registry(name, config, text, log)
        return f'{checks} checks passed' + (f', {skips} skipped' if skips else '') + f'; {shaders}; {registry}; output in {output}'

    SMOKE_REGISTRY_LINE = re.compile(r'^\[smoke\]\s+ok\s+registry: (\d+) records, initial scan (\d+) ms', re.MULTILINE)
    SMOKE_PANEL_LINE = re.compile(r'^\[smoke\]\s+ok\s+the Assets panel shows (\d+) of (\d+) record\(s\) of type:Texture', re.MULTILINE)

    def check_smoke_registry(self, name, config, text, log):
        """The smoke test's registry check: the initial scan completed (its "[smoke] ok registry:" line) and the
        editor's Assets panel shows what the registry holds; in SMOKE_TEXTURES templates it lists textures. Records
        the host's "[assets] initial scan:" line for the summary. Returns a detail text, or raises StepFailed."""
        self.read_initial_scan(f'editor, {name} ({config})', log)
        registry = self.SMOKE_REGISTRY_LINE.search(text)
        if not registry:
            raise StepFailed('no "[smoke] ok registry:" line: the asset registry check did not pass')
        panel = self.SMOKE_PANEL_LINE.search(text)
        if not panel:
            raise StepFailed('no "[smoke] ok the Assets panel shows" line: the Assets panel check did not pass')
        if name in SMOKE_TEXTURES and int(panel.group(1)) == 0:
            raise StepFailed(f'the Assets panel of {name} lists no texture (type:Texture)')
        return f'registry: {registry.group(1)} records, {panel.group(2)} textures, initial scan {registry.group(2)} ms'

    # Lines of the smoke test's file edits that must have passed: without them the step tested nothing.
    SCRATCH_EDIT_CHECKS = (
        ('the material reload', re.compile(r'^\[smoke\]\s+ok\s+.* reloaded: version', re.MULTILINE)),
        ('the shader rebuild', re.compile(r'^\[smoke\]\s+ok\s+exactly the \d+ program\(s\) that read ', re.MULTILINE)),
    )

    def smoke_scratch(self, log, config):
        """The smoke steps that edit files, on a scratch copy (design 13.5, step 3), never on the engine
        checkout: FRT.bat new copies Templates/Demo into a short directory of its own in %TEMP% (a git
        repository, as every new project is), FRT.bat build builds its game module in Config, the shaders
        are copied beside it, and the editor's smoke test runs over the copy with -Editor:SmokeTestEdits=1
        and -Render:ShaderRoot=<the copy of the shaders>. Besides its usual steps it rewrites one of the
        copy's materials and appends a comment to one shader include, and checks through the file watcher
        (which a scripted run keeps off until those steps turn it on) that the material reloads and that
        exactly the programs including that file are rebuilt. The three commands share one user-data
        directory, where FRT.bat new registers this engine for the copy to find it. The copy is deleted
        when the step passes, and kept for a look when it fails."""
        root = self.scratch_root
        remove_tree(root)
        root.mkdir(parents=True, exist_ok=True)
        env = self.env_for(log)
        project_dir = root / SCRATCH_NAME
        project = project_dir / f'{SCRATCH_NAME}.frtproject'

        code, seconds = run([FRT, 'new', f'--name={SCRATCH_NAME}', f'--template={SCRATCH_TEMPLATE}', f'--location={root}'],
                            log, cwd=ROOT, env=env, timeout=GENERATE_TIMEOUT)
        self.timings.append((f'scratch copy of {SCRATCH_TEMPLATE} (FRT.bat new)', f'{seconds:.1f} s'))
        if code != 0 or not project.exists():
            errors = tail(log, lines=3, pattern=r'error|failed|cannot|new:')
            raise StepFailed('FRT.bat new: ' + ('timed out' if code is None else
                                                '; '.join(errors) if errors else f'exit code {code}'))
        retries = self.build(log, f'--project={project}', f'--config={config}', env=env)

        shaders = root / 'Shaders'
        shutil.copytree(ROOT / 'Core' / 'Content' / 'Shaders', shaders, ignore=shutil.ignore_patterns('Bin'))

        exe = BINARIES / f'Editor-{config}-Default.exe'
        output = self.run_dir / 'Smoke' / f'{SCRATCH_NAME}-{config}'
        code, seconds = run([exe, f'-Project={project}', '-Editor:SmokeTest=1', f'-Editor:SmokeTestDir={output}',
                             '-Editor:SmokeTestEdits=1', f'-Render:ShaderRoot={shaders}'],
                            log, cwd=project_dir, env=env, timeout=SMOKE_TIMEOUT)
        self.timings.append((f'editor smoke test with file edits, scratch copy ({config})', f'{seconds:.1f} s'))
        text = Path(log).read_bytes().decode('utf-8', errors='replace')
        checks = len(re.findall(r'^\[smoke\]\s+ok\s', text, re.MULTILINE))
        skips = len(re.findall(r'^\[smoke\]\s+skip\s', text, re.MULTILINE))
        if code != 0:
            failures = re.findall(r'^\[smoke\]\s+FAIL\s+(.*)$', text, re.MULTILINE)
            raise StepFailed(('timed out' if code is None else f'exit code {code}') +
                             (': ' + '; '.join(f.strip() for f in failures[:3]) if failures else '') +
                             f' (the copy is kept: {root})')
        missing = [what for what, pattern in self.SCRATCH_EDIT_CHECKS if not pattern.search(text)]
        if missing:
            raise StepFailed(f'no check of {" or ".join(missing)} passed (the copy is kept: {root})')
        remove_tree(root)
        return (f'{checks} checks passed' + (f', {skips} skipped' if skips else '') +
                ', the material reloaded and the shader programs rebuilt' + (f'; build {retries}' if retries else '') +
                f'; output in {output}')

    # ---- captures ----

    def take_capture(self, config, exe, arguments, cwd, base, kind_arguments, label):
        """Runs one scripted capture of Config into Base(.png, .pfm) and reads its shader cache line
        (check_shader_cache); returns the GPU's name."""
        base.parent.mkdir(parents=True, exist_ok=True)
        for extension in ('.png', '.pfm'):
            base.with_suffix(extension).unlink(missing_ok=True)
        gpu = []

        def on_line(line, _):
            match = re.match(rb'\[renderer\] ([^|\r\n]+)', line)
            if match and not gpu:
                gpu.append(match.group(1).decode('utf-8', errors='replace').strip())

        log = self.logs / f'capture-{log_name(label)}.log'
        code, seconds = run([exe, *arguments, *CAPTURE_ARGUMENTS, *kind_arguments, f'-Automation:CapturePath={base}'],
                            log, cwd=cwd, env=self.env_for(log), timeout=CAPTURE_TIMEOUT, on_line=on_line)
        self.timings.append((f'capture {label}', f'{seconds:.1f} s'))
        if code != 0:
            raise StepFailed(('timed out' if code is None else f'exit code {code}') + ': ' +
                             '; '.join(tail(log, lines=2, pattern=r'error|fail|\[automation\]')) + f' (log: {log})')
        for extension in ('.png', '.pfm'):
            if not base.with_suffix(extension).exists():
                raise StepFailed(f'no {base.name}{extension} written (log: {log})')
        self.check_shader_cache(config, f'capture {label}', log)
        self.read_initial_scan(f'capture {label}', log)
        return gpu[0] if gpu else 'unknown GPU'

    def capture_targets(self, config):
        """(template, exe, arguments, working directory) of each game captured in Config."""
        if config == 'Release':
            for name in TEMPLATES:
                package = self.package_root / name
                exes = sorted(package.glob('*.exe'))
                yield name, (exes[0] if exes else package / f'{name}.exe'), [], package
        else:
            for name in CAPTURED_IN_PLACE:
                project_dir = ROOT / 'Templates' / name
                yield (name, BINARIES / f'Launcher-{config}-Default.exe',
                       [f'-Project={project_dir / (name + ".frtproject")}'], project_dir)

    def manifest(self, config):
        path = self.baselines / config / 'Baseline.ini'
        values = {}
        if path.exists():
            for line in path.read_text(encoding='utf-8').splitlines():
                key, equals, value = line.partition('=')
                if equals and not line.startswith(';'):
                    values[key.strip()] = value.strip()
        return values

    def write_manifest(self, directory):
        commit = (git('rev-parse', 'HEAD') or b'unknown').decode('utf-8').strip()
        dirty = ' (with uncommitted changes)' if git('status', '--porcelain', '--untracked-files=no') else ''
        lines = [
            '; The baseline captures of FRT.bat verify, recorded by FRT.bat verify --record-baselines. They are',
            '; this machine\'s: a capture depends on the GPU, its driver and the display\'s scaling.',
            '[Baseline]',
            f'Recorded={datetime.datetime.now().isoformat(timespec="seconds")}',
            f'Commit={commit}{dirty}',
            f'Gpu={self.gpu}',
        ]
        for kind, kind_arguments, _ in CAPTURE_KINDS:
            lines.append(f'Recipe.{kind}={" ".join(CAPTURE_ARGUMENTS + kind_arguments)}')
        (directory / 'Baseline.ini').write_text('\n'.join(lines) + '\n', encoding='utf-8')

    def staging(self, config):
        """Where --record-baselines records Config's baselines: beside the ones they replace, which stay as
        they are until every capture of Config recorded (store_baselines)."""
        return self.baselines / f'{config}.new'

    def store_baselines(self, config, complete):
        """Swaps the baselines recorded for Config in, Baseline.ini included, when every capture of Config
        recorded (Complete), and leaves the old ones as they are otherwise. A set of new and old captures
        under a new Baseline.ini - a raster capture recorded, the path-traced one failing - would hold
        images of two commits, perhaps of two renderers, and later runs would compare against that mix
        with nothing to tell, as the recipe would be the same."""
        staging, target = self.staging(config), self.baselines / config
        name = f'baselines ({config})'
        old = f'the old ones in {target} are kept' if target.exists() else f'{target} has none'
        if not complete or not self.gpu:
            shutil.rmtree(staging, ignore_errors=True)
            self.report(name, 'FAILED', None, f'none written, as not every capture of {config} recorded; {old}')
            return
        self.write_manifest(staging)
        previous = self.baselines / f'{config}.old'
        try:
            shutil.rmtree(previous, ignore_errors=True)
            if target.exists():
                target.rename(previous)
            try:
                staging.rename(target)
            except OSError:
                if previous.exists() and not target.exists():
                    previous.rename(target)
                raise
        except OSError as error:
            self.report(name, 'FAILED', None, f'cannot replace {target} ({error}); {old}, the new ones are in '
                                              f'{staging}')
            return
        shutil.rmtree(previous, ignore_errors=True)
        count = len(list(target.glob('*.pfm')))
        self.report(name, 'ok', None, f'{count} capture{"" if count == 1 else "s"} and Baseline.ini in {target}')

    def agree(self, a, b, exact):
        """(whether two captures agree, a detail text)."""
        relative, largest, size = compare_pfm(a.with_suffix('.pfm'), b.with_suffix('.pfm'))
        if exact:
            different = [extension for extension in ('.png', '.pfm')
                         if not same_bytes(a.with_suffix(extension), b.with_suffix(extension))]
            if not different:
                return True, f'{size}, identical'
            verb = 'differs' if len(different) == 1 else 'differ'
            return False, f'{size}, the {" and ".join(different)} {verb} (linear: rel-RMSE {relative:.2e}, ' \
                          f'max abs {largest:.3g})'
        return relative <= self.tolerance, f'{size}, rel-RMSE {relative:.2e} (max {self.tolerance:.0e}), ' \
                                           f'max abs {largest:.3g}'

    def capture_step(self, log, config, name, exe, arguments, cwd, kind, kind_arguments, exact):
        del log   # each capture has its own log
        if not Path(exe).exists():
            raise StepFailed(f'no {exe}')
        captures = self.run_dir / 'Captures' / config
        baseline = (self.staging(config) if self.record else self.baselines / config) / f'{name}-{kind}'
        notes = []
        user_settings = Path(cwd) / 'Local' / 'UserSettings.ini'
        if config != 'Release' and user_settings.exists():
            notes.append(f'note: {user_settings} overrides the project\'s config')
        # Raster must repeat exactly: one disagreement fails. Path tracing gets more attempts.
        attempts = 1 if exact else PATH_TRACED_ATTEMPTS

        def take(index):
            base = captures / f'{name}-{kind}-{index}'
            label = f'{name} {kind} ({config})' + (f' #{index}' if self.record or index > 1 else '')
            gpu = self.take_capture(config, exe, arguments, cwd, base, kind_arguments, label)
            if self.gpu and gpu != self.gpu:
                raise StepFailed(f'{gpu} rendered this, {self.gpu} the other captures')
            self.gpu = gpu
            return base

        if self.record:
            # Two recordings that agree, the first of them kept.
            taken = [take(1)]
            tried = []
            for index in range(2, attempts + 2):
                taken.append(take(index))
                for earlier in taken[:-1]:
                    ok, detail = self.agree(taken[-1], earlier, exact)
                    if ok:
                        baseline.parent.mkdir(parents=True, exist_ok=True)
                        for extension in ('.png', '.pfm'):
                            shutil.copyfile(earlier.with_suffix(extension), baseline.with_suffix(extension))
                        runs = f'runs {earlier.name[-1]} and {index} of {index}' if index > 2 else 'two runs'
                        return '; '.join([f'recorded, {runs} agree: {detail}'] + notes)
                    tried.append(detail)
            raise StepFailed(f'{len(taken)} recordings disagree, no baseline written: ' + '; '.join(tried[:3]))

        manifest = self.manifest(config)
        if not baseline.with_suffix('.pfm').exists() or not baseline.with_suffix('.png').exists():
            raise StepFailed(f'no baseline in {baseline.parent} (FRT.bat verify --record-baselines)')
        recipe = ' '.join(CAPTURE_ARGUMENTS + kind_arguments)
        if manifest.get(f'Recipe.{kind}', recipe) != recipe:
            raise StepFailed('the baselines were taken with other arguments: record them again')
        tried = []
        for index in range(1, attempts + 1):
            capture = take(index)
            if manifest.get('Gpu') and manifest['Gpu'] != self.gpu:
                raise StepFailed(f'the baselines are from {manifest["Gpu"]}, this is {self.gpu}: record them here')
            ok, detail = self.agree(capture, baseline, exact)
            if ok:
                if tried:
                    detail += f' (attempt {index}; before: ' + '; '.join(t.split(', ', 1)[1] for t in tried) + ')'
                return '; '.join([detail] + notes)
            tried.append(detail)
        raise StepFailed('; '.join([f'differs from the baseline: ' + ' | '.join(tried)] + notes))

    def captures(self, config):
        if self.record:
            shutil.rmtree(self.staging(config), ignore_errors=True)
        complete = True
        for name, exe, arguments, cwd in self.capture_targets(config):
            needs = [(name, config)] if config == 'Release' else [('engine', config), (name, config)]
            for kind, kind_arguments, exact in CAPTURE_KINDS:
                label = f'capture {name} {"raster" if exact else "path traced"} ({config})'
                complete &= self.step(label, self.capture_step, config, name, exe, arguments, cwd, kind,
                                      kind_arguments, exact, needs=needs)
        if self.record:
            self.store_baselines(config, complete)

    # ---- packages ----

    def package(self, log, name):
        project = ROOT / 'Templates' / name / f'{name}.frtproject'
        output = self.package_root / name
        code, seconds = run([FRT, 'package', f'--project={project}', f'--output={output}'], log, cwd=ROOT,
                            env=self.env_for(log), timeout=PACKAGE_TIMEOUT)
        self.timings.append((f'package {name} (Release, build included)', f'{seconds:.1f} s'))
        if code is None:
            raise StepFailed(f'timed out after {PACKAGE_TIMEOUT / 60:g} min and was killed (a hung compiler, '
                             f'MSBuild node or Premake?)')
        if code != 0:
            errors = tail(log, lines=4, pattern=r'\berror\b|failed|cannot')
            raise StepFailed('; '.join(errors) if errors else f'exit code {code}')
        if not list(output.glob('*.exe')):
            raise StepFailed(f'no executable in {output}')
        return str(output)

    # ---- git ----

    def git_status(self, log, before):
        status = git('status', '--porcelain')
        if status is None:
            return 'not a git checkout: nothing to check'
        Path(log).write_bytes(b'before:\n' + (before or b'') + b'\nafter:\n' + status)
        lines = status.decode('utf-8', errors='replace').splitlines()
        if self.mode == 'quick':
            if status != before:
                raise StepFailed('the run changed tracked files: ' + '; '.join(lines[:3]))
            return 'unchanged by the run' + (f' ({len(lines)} uncommitted change(s) from before)' if lines else '')
        if lines:
            changed = '' if status == before else 'the run changed tracked files; '
            raise StepFailed(f'{changed}{len(lines)} entr{"y" if len(lines) == 1 else "ies"}: ' + '; '.join(lines[:3]))
        return 'clean'

    # ---- the whole run ----

    def run(self):
        shutil.rmtree(self.run_dir, ignore_errors=True)
        self.logs.mkdir(parents=True, exist_ok=True)
        (self.run_dir / 'UserData').mkdir(parents=True, exist_ok=True)
        status_before = git('status', '--porcelain')
        started = time.monotonic()
        what = {'quick': 'quick (Development, Core-Test, lints, AssetTool)', 'full': 'full', 'nightly': 'nightly'}[self.mode]
        print(f'[verify] {ROOT}: {what}{", recording baselines" if self.record else ""}')
        print(f'[verify] logs and captures: {self.run_dir}')
        print(f'[verify] baselines: {self.baselines}', flush=True)

        configs = ('Development',) if self.mode == 'quick' else CAPTURE_CONFIGS
        self.generate_engine()
        if self.mode != 'quick':
            for name in TEMPLATES:
                self.generate_template(name)
        for config in configs:
            self.build_engine(config)
        if self.mode != 'quick':
            for config in configs:
                for name in TEMPLATES:
                    self.build_template(name, config)
        for config in configs:
            self.step(f'Core-Test ({config})', self.core_test, config, needs=[('engine', config)])

        self.step('lint strings', self.lint, 'Tools/Lint/CheckStrings.py')
        # Over the list the build writes (FRT.bat builder-hashes, Core's prebuild step).
        self.step('lint pipeline', self.lint, 'Tools/Lint/CheckPipeline.py')
        if (ROOT / 'Tools' / 'Lint' / 'CheckPlatform.py').exists():
            self.step('lint platform', self.lint, 'Tools/Lint/CheckPlatform.py')
        else:
            self.skip('lint platform', 'skipped (no Tools/Lint/CheckPlatform.py)')
        if git('rev-parse', '--git-dir') is None:
            self.skip('line endings', 'skipped (not a git checkout)')
        else:
            self.step('line endings', self.line_endings)
        self.step('builder source guard (BUILD003)', self.check_builders)
        self.step('assets newguid --json', self.assets_newguid, needs=[('engine', 'Development')])
        self.step('assets arguments', self.assets_arguments, needs=[('engine', 'Development')])
        self.step('assets registry verbs --json', self.assets_registry_verbs, needs=[('engine', 'Development')])
        # Before the steps that run the editor and games in place: their content as committed.
        self.step('registry golden dumps', self.registry_goldens, needs=[('engine', 'Development')])
        self.step('docs name every verb and code', self.docs_coverage, needs=[('engine', 'Development')])

        if self.mode != 'quick':
            self.skip('assets validate --ci', 'skipped (M4)')
            for config in SMOKE_CONFIGS:
                for name in TEMPLATES:
                    self.step(f'smoke {name} in place ({config})', self.smoke, name, config,
                              needs=[('engine', config), (name, config)])
            self.step(f'smoke on a scratch copy ({SCRATCH_CONFIG})', self.smoke_scratch, SCRATCH_CONFIG,
                      needs=[('engine', SCRATCH_CONFIG)])
            for config in CAPTURE_CONFIGS:
                self.captures(config)
            # Not a milestone's: a renderer fix first (CAPTURE_ARGUMENTS says which).
            self.skip('capture with auto exposure', 'skipped (needs FrameData.DeltaTime set)')
            self.skip('captures from an empty and a warm DDC', 'skipped (M6)')
            self.skip('captures with SyncLoadPolicy=Fail', 'skipped (M10)')
            for name in TEMPLATES:
                if not self.step(f'package {name} (Release)', self.package, name, needs=[('generate', name)]):
                    self.broken.add((name, 'Release'))
            self.captures('Release')
            self.skip('packaged smoke run', 'skipped (M6)')
            self.skip('assets docs --check', 'skipped (later: assets docs)')
        if self.mode == 'nightly':
            self.skip('assets build --determinism-check', 'skipped (M1)')
            self.skip('captures with -Ddc:Verify=100', 'skipped (M1)')
            self.build_engine('Release')
            self.step('registry benchmarks, DISABLED_ suites (Release, FRT_PERF_STRICT=1)', self.nightly_registry,
                      needs=[('engine', 'Release')])
            self.skip('assets cook --verify', 'skipped (M6)')
            self.skip('cold-cache start after a reboot', 'skipped (manual)')

        self.step('git status', self.git_status, status_before)
        return self.summary(time.monotonic() - started)

    def summary(self, seconds):
        failed = [r for r in self.results if r[1] in ('FAILED', 'not run')]
        print()
        print(f'{"Step":<42} {"Result":<16} {"Time":>9}  Detail')
        print('-' * 120)
        for name, status, step_seconds, detail in self.results:
            time_text = f'{step_seconds:7.1f} s' if step_seconds is not None else ''
            print(f'{name:<42} {status:<16} {time_text:>9}  {detail}')
        print('-' * 120)
        if self.timings:
            print('Timings (for the commit message):')
            for what, text in self.timings:
                print(f'  {what}: {text}')
            for what, files, records, scan_ms, waited_ms in self.initial_scans:
                print(f'  initial scan, {what}: {files} files, {records} records, {scan_ms:.1f} ms; BuildWorld waited {waited_ms:.1f} ms')
            if not self.initial_scans:
                print('  BuildWorld: no host logged "[assets] initial scan:"')
        verdict = 'FAILED' if failed else 'passed'
        counts = f'{len(self.results) - len(failed)} of {len(self.results)} steps ok or skipped'
        print(f'verify {verdict}: {counts}, {seconds / 60.0:.1f} min')
        return 1 if failed else 0


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description='The engine\'s merge gate (FRT.bat verify).')
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--quick', action='store_true', help='build Development, its Core-Test and the lints')
    group.add_argument('--nightly', action='store_true', help='everything, and the long checks')
    group.add_argument('--update-goldens', action='store_true',
                       help='write the registry\'s golden dumps again from FRT.bat assets, show the diff, run nothing else')
    parser.add_argument('--record-baselines', action='store_true', help='record the captures as the baselines')
    parser.add_argument('--baselines', metavar='DIR', help='the baselines (default Local/Verify/Baselines)')
    parser.add_argument('--pt-tolerance', metavar='RMSE', type=float, default=PATH_TRACED_TOLERANCE,
                        help=f'the relative RMSE a path-traced capture may differ by ({PATH_TRACED_TOLERANCE:g})')
    options = parser.parse_args()
    if options.update_goldens:
        return Gate(options).update_goldens()
    return Gate(options).run()


if __name__ == '__main__':
    sys.exit(main())
