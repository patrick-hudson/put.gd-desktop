"""Bounded dependency diagnosis only. Never launches or controls putgd."""
from pathlib import Path
import hashlib, json, os, resource, stat, subprocess, tempfile
HERE = Path(__file__).resolve().parent
MAX_BYTES = 65536
TIMEOUT_SECONDS = 15

def private_directory(path):
    value = path.lstat()
    if not stat.S_ISDIR(value.st_mode) or value.st_uid != os.getuid() or stat.S_IMODE(value.st_mode) != 0o700:
        raise RuntimeError('private_owned_directory_required')


def child_limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_BYTES, MAX_BYTES))


def main():
    private_directory(HERE)
    binary = HERE / 'build' / 'svg_decode_probe'
    info = binary.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or not os.access(binary, os.X_OK):
        raise RuntimeError('owned_regular_probe_required')
    build = binary.parent.lstat()
    if not stat.S_ISDIR(build.st_mode) or build.st_uid != os.getuid():
        raise RuntimeError('owned_build_directory_required')
    output = HERE / 'result.json'
    if output.exists() or output.is_symlink():
        raise RuntimeError('fresh_result_required')
    runtime = HERE / 'runtime'
    runtime.mkdir(mode=0o700)
    private_directory(runtime)
    initial_binary_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
    rows = []
    for mode in ('inventory', 'core', 'gui'):
        environment = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8',
                       'QT_QPA_PLATFORM': 'offscreen', 'XDG_RUNTIME_DIR': str(runtime)}
        with tempfile.TemporaryFile(dir=HERE) as stdout, tempfile.TemporaryFile(dir=HERE) as stderr:
            child = subprocess.Popen([str(binary), mode], cwd=HERE, env=environment,
                                     stdout=stdout, stderr=stderr, preexec_fn=child_limits)
            timed_out = False
            try:
                child.wait(timeout=TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                timed_out = True
                child.terminate()
                try:
                    child.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=1)
            stdout.seek(0); stderr.seek(0)
            out = stdout.read(MAX_BYTES + 1); err = stderr.read(MAX_BYTES + 1)
            if len(out) > MAX_BYTES or len(err) > MAX_BYTES:
                raise RuntimeError('child_output_limit_failed')
            parsed = []
            for line in out.decode('utf-8', errors='replace').splitlines():
                try: parsed.append(json.loads(line))
                except json.JSONDecodeError: parsed.append({'unparsedStdout': line})
            if hashlib.sha256(binary.read_bytes()).hexdigest() != initial_binary_sha:
                raise RuntimeError('probe_binary_changed')
            rows.append({'mode': mode, 'returnCode': child.returncode,
                         'signal': -child.returncode if child.returncode < 0 else None,
                         'timedOut': timed_out, 'stdoutBytes': len(out), 'stderrBytes': len(err),
                         'stdout': parsed, 'stderr': err.decode('utf-8', errors='replace')})
    value = {'schemaVersion': 1, 'dependencyDiagnosisOnly': True,
             'nativeParityProof': False, 'memoryAcceptance': False,
             'binarySha256': initial_binary_sha,
             'timeoutSeconds': TIMEOUT_SECONDS, 'outputLimitBytesPerStream': MAX_BYTES,
             'coreDumpLimitBytes': 0, 'guiPlatform': 'offscreen', 'rows': rows}
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(value, stream, indent=2); stream.write('\n')
    print(json.dumps({'result': str(output), 'sha256': hashlib.sha256(output.read_bytes()).hexdigest()}))

if __name__ == '__main__':
    main()
