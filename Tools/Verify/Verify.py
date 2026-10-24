"""FRT.bat verify: the engine's merge gate, run on a developer's machine.

There is no CI, and byte-exact D3D12 captures need a GPU, which hosted runners lack, so the gate is this
script, behind FRT.bat verify (Premake/tools.lua). A milestone branch merges only after it passes. In order:

  1. builds: the engine's solution and the templates' generated again (FRT.bat generate: Premake finds
     source files by glob, so a solution older than the sources would build without the new ones), then
     the engine in Debug and Development (FRT.bat build), and the templates' game modules in both;
  2. Core-Test in Debug and Development;
  3. the lints: Tools/Lint/CheckStrings.py, Tools/Lint/CheckPlatform.py when it exists, and line endings:
     every file .gitattributes makes LF is LF in the index and in the working tree, every -text file is
     checked out as committed (FRT.bat check-line-endings: the rules FRT.bat setup applies, in
     Premake/tools.lua only);
  4. AssetTool through FRT.bat assets: newguid --json exits with 0 and prints JSON objects, one per line;
  5. the editor's smoke test (-Editor:SmokeTest=1) over Templates/Demo and Templates/Blank, in place, in
     Debug and Development;
  6. automation captures of Templates/Demo through the launcher, in Debug and in Development: one raster,
     compared byte for byte with its baseline, and one path traced, compared by relative RMSE - path
     tracing still varies a little from run to run, as its radiance cache inserts with GPU atomics, so a
     path-traced capture beyond the tolerance is taken again (PATH_TRACED_ATTEMPTS);
  7. the Release package baseline: FRT.bat package of both templates into a short scratch directory, and
     the same two captures of each packaged game, against the Release baselines;
  8. git status --porcelain is empty at the end: nothing it ran changed a tracked file.
Steps that later milestones bring print "skipped (M<n>)". It ends with a summary table and the timings a
commit message reports, and exits with 1 when any step failed.

    python -I Tools/Verify/Verify.py [--quick | --nightly] [--record-baselines] [--baselines=<dir>]
                                     [--pt-tolerance=<rmse>]

  --quick             build Development, run its Core-Test, the lints and AssetTool: the pre-commit subset
  --nightly           everything, and the long checks (placeholders until they exist)
  --record-baselines  record the captures as the baselines instead of comparing with them; a baseline is
                      kept only when two recordings agree, byte for byte for raster, and a configuration's
                      baselines replace its old ones only when every one of its captures recorded
  --baselines=<dir>   where the baselines are (default <engine>/Local/Verify/Baselines): Debug/ and
                      Development/ for the in-place captures, Release/ for the packaged games'
  --pt-tolerance=<x>  the relative RMSE a path-traced capture may differ by (default 1e-4)

Baselines belong to a machine: a capture depends on the GPU, its driver and the display's scaling, which
sets the window's size. Baseline.ini beside them says what took them, and a comparison with baselines
from another GPU or another recipe fails as such. What a run writes - logs, captures, the smoke tests'
output, and a user-data directory (FRT_USER_DATA_DIR) of its own, new and empty, for every process it
starts, so that no step sees what an earlier one left there - goes to <engine>/Local/Verify/Last, emptied
first; packages go to %TEMP%/frtv-<hash of the engine path>.

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
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BINARIES = ROOT / 'Binaries' / 'Win64'
FRT = ROOT / 'FRT.bat'
TEMPLATES = ('Demo', 'Blank')
CAPTURED_IN_PLACE = ('Demo',)
CAPTURE_CONFIGS = ('Debug', 'Development')
SMOKE_CONFIGS = ('Debug', 'Development')
CAPTURE_FRAME = 400
# ||capture - baseline|| / ||baseline|| over every channel of every pixel of the linear HDR capture. Two
# path-traced runs of identical binaries differ by up to about 1e-4 (commit fee4062), more while other
# work shares the GPU, which reorders the radiance cache's atomics further: a path-traced capture beyond
# the tolerance is taken again, up to PATH_TRACED_ATTEMPTS times. A real change moves the image by far
# more than the noise, so it fails every attempt; the summary shows the attempts a pass needed.
PATH_TRACED_TOLERANCE = 1e-4
PATH_TRACED_ATTEMPTS = 3

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


def run(command, log, cwd=None, env=None, timeout=None, on_line=None):
    """Runs Command (a list) with its output appended to Log. Returns (exit code, seconds); the code is None
    when it ran past Timeout and was killed. OnLine(line bytes, seconds since start) sees each line, a
    little after it was written.

    The output goes straight into the file, never through a pipe: a build leaves processes behind
    (mspdbsrv, MSBuild nodes) that inherit the output handle, and a pipe would stay open as long as they
    live. A file is read back while the process runs instead."""
    command = [str(part) for part in command]
    start = time.monotonic()
    with open(log, 'ab') as out:
        out.write(f'$ {subprocess.list2cmdline(command)}\n'.encode('utf-8'))
        out.flush()
        offset = out.tell()
        process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=out,
                                   stderr=subprocess.STDOUT)
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
        self.package_root = Path(tempfile.gettempdir()) / (
            'frtv-' + hashlib.sha1(os.path.normcase(str(ROOT)).encode('utf-8')).hexdigest()[:8])
        self.results = []    # (name, status, seconds, detail)
        self.timings = []    # (what, text)
        self.broken = set()  # what failed and so cannot be used: ('engine', config), (template, config)
        self.gpu = None
        self.step_number = 0

        env = dict(os.environ)
        # Never the user's own; every process gets a directory of its own below this one (env_for).
        env['FRT_USER_DATA_DIR'] = str(self.run_dir / 'UserData')
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

    def build(self, log, *arguments):
        retries = []
        for attempt in range(BUILD_ATTEMPTS):
            since = log.stat().st_size if log.exists() else 0
            code, _ = run([FRT, 'build', *arguments], log, cwd=ROOT, env=self.env_for(log), timeout=BUILD_TIMEOUT)
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
        return counts

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

    # ---- AssetTool ----

    def assets_newguid(self, log):
        """FRT.bat assets newguid --json, the way a script runs AssetTool: exit code 0, and every line of
        standard output one JSON object with the stable fields "guid" (32 lowercase hex digits) and "cpp".
        The log has standard error too (FRT.bat assets would print a build there), whose lines are no
        objects: only lines that start with '{' count, and each of those must parse."""
        code, _ = run([FRT, 'assets', 'newguid', '3', '--json'], log, cwd=ROOT, env=self.env_for(log),
                      timeout=BUILD_TIMEOUT)
        if code != 0:
            raise StepFailed(('timed out; ' if code is None else f'exit code {code}; ') + '; '.join(tail(log, lines=3)))
        guids = []
        text = Path(log).read_bytes().decode('utf-8', errors='replace')
        for line in text.splitlines():
            if not line.startswith('{'):
                continue
            try:
                value = json.loads(line)
            except ValueError as error:
                raise StepFailed(f'not a JSON object: {line[:80]} ({error})')
            if not isinstance(value.get('guid'), str) or not re.fullmatch(r'[0-9a-f]{32}', value['guid']) or \
                    not isinstance(value.get('cpp'), str):
                raise StepFailed(f'no "guid" and "cpp" in {line[:80]}')
            guids.append(value['guid'])
        if len(guids) != 3 or len(set(guids)) != 3:
            raise StepFailed(f'{len(set(guids))} distinct GUIDs instead of 3')
        return '3 GUIDs as JSON lines'

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
        return f'{checks} checks passed' + (f', {skips} skipped' if skips else '') + f'; output in {output}'

    # ---- captures ----

    def take_capture(self, exe, arguments, cwd, base, kind_arguments, label):
        """Runs one scripted capture into Base(.png, .pfm); returns the GPU's name."""
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
            gpu = self.take_capture(exe, arguments, cwd, base, kind_arguments, label)
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
        if (ROOT / 'Tools' / 'Lint' / 'CheckPlatform.py').exists():
            self.step('lint platform', self.lint, 'Tools/Lint/CheckPlatform.py')
        else:
            self.skip('lint platform', 'skipped (no Tools/Lint/CheckPlatform.py)')
        if git('rev-parse', '--git-dir') is None:
            self.skip('line endings', 'skipped (not a git checkout)')
        else:
            self.step('line endings', self.line_endings)
        self.step('assets newguid --json', self.assets_newguid, needs=[('engine', 'Development')])

        if self.mode != 'quick':
            self.skip('assets validate --ci', 'skipped (M4)')
            for config in SMOKE_CONFIGS:
                for name in TEMPLATES:
                    self.step(f'smoke {name} in place ({config})', self.smoke, name, config,
                              needs=[('engine', config), (name, config)])
            self.skip('smoke on a scratch copy', 'skipped (M2)')
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
            self.skip('builder source guard (BUILD003)', 'skipped (M1)')
            self.skip('assets docs --check', 'skipped (M1)')
        if self.mode == 'nightly':
            self.skip('assets build --determinism-check', 'skipped (M1)')
            self.skip('captures with -Ddc:Verify=100', 'skipped (M1)')
            self.skip('100k/1M synthetic registry suite', 'skipped (M3)')
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
            print('  BuildWorld: not logged yet')
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
    parser.add_argument('--record-baselines', action='store_true', help='record the captures as the baselines')
    parser.add_argument('--baselines', metavar='DIR', help='the baselines (default Local/Verify/Baselines)')
    parser.add_argument('--pt-tolerance', metavar='RMSE', type=float, default=PATH_TRACED_TOLERANCE,
                        help=f'the relative RMSE a path-traced capture may differ by ({PATH_TRACED_TOLERANCE:g})')
    return Gate(parser.parse_args()).run()


if __name__ == '__main__':
    sys.exit(main())
