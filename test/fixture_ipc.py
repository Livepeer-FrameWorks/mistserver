"""Release only the named IPC objects an isolated test fixture owns."""
import argparse
import ctypes
import errno
import os
from pathlib import Path
import re
import subprocess
import sys


def fallback_root(env=None):
    env = os.environ if env is None else env
    return Path(next((env[key] for key in ('TMP', 'TEMP', 'TMPDIR') if env.get(key)), '/tmp')) / 'mist'


def page_exists(name, env=None):
    if (fallback_root(env) / name.lstrip('/')).exists():
        return True
    if sys.platform == 'darwin':
        return False
    libc = ctypes.CDLL(None, use_errno=True)
    libc.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint]
    descriptor = libc.shm_open(name.encode(), os.O_RDONLY, 0)
    if descriptor >= 0:
        os.close(descriptor)
        return True
    if ctypes.get_errno() != errno.ENOENT:
        raise OSError(ctypes.get_errno(), name)
    return False


def release_page(name, env=None):
    if not re.fullmatch(r'/Mst[^/\x00]+', name):
        raise ValueError('expected one literal Mist IPC object name')
    libc = ctypes.CDLL(None, use_errno=True)
    unlink = libc.sem_unlink if name.startswith('/MstSem') else libc.shm_unlink
    unlink.argtypes = [ctypes.c_char_p]
    actual = name[:30] if sys.platform == 'darwin' and name.startswith('/MstSem') else name
    if sys.platform != 'darwin' or name.startswith('/MstSem'):
        if unlink(actual.encode()) != 0 and ctypes.get_errno() != errno.ENOENT:
            raise OSError(ctypes.get_errno(), name)
    (fallback_root(env) / name.lstrip('/')).unlink(missing_ok=True)


def run_owned(command, *, timeout=None, **kwargs):
    # Unlike a buffer-managed processor, a directly spawned child has this
    # fixture as the owner of its terminal state page.
    input_data = kwargs.pop('input', None)
    if input_data is not None:
        kwargs['stdin'] = subprocess.PIPE
    with subprocess.Popen(command, **kwargs) as child:
        try:
            stdout, stderr = child.communicate(input_data, timeout=timeout)
        except BaseException:
            child.kill()
            child.wait()
            raise
        finally:
            release_page('/MstProcState_' + str(child.pid), kwargs.get('env'))
        return subprocess.CompletedProcess(command, child.returncode, stdout, stderr)


def cleanup_logs(directory, streams=(), sessions=(), env=None):
    directory = Path(directory)
    text = '\n'.join(p.read_text(errors='replace') for p in directory.glob('*.log'))
    pids = set(re.findall(r'\|(?:Mist[^|]*)\|(\d+)\|', text))
    pids.update(re.findall(r'(?:Started process |\(PID )(\d+)', text))
    sessions = set(sessions) | set(re.findall(r'(?:Booting new session|Started new session) ([IOUV][a-zA-Z0-9]+)', text))
    names = set('/MstProcState_' + pid for pid in pids)
    for session in sessions:
        names.update(('/MstSession' + session, '/MstSemSess' + session))
    for stream in streams:
        if not re.fullmatch(r'[a-zA-Z0-9_+.-]+', stream):
            raise ValueError('expected one literal fixture stream name')
        names.update('/Mst' + prefix + stream for prefix in ('Meta', 'IPID', 'PPID', 'POut', 'PGrf', 'STATE', 'User'))
        names.update('/MstSem' + prefix + stream for prefix in ('Inpt', 'TRKS', 'User'))
    for folder in (Path('/dev/shm'), fallback_root(env)):
        if not folder.is_dir():
            continue
        for p in folder.iterdir():
            if any(p.name.startswith('MstAttRes_' + pid + '_') for pid in pids) or any(
                    p.name.startswith(prefix + stream + '@') for stream in streams for prefix in ('MstData', 'MstTrak')):
                names.add('/' + p.name)
    for name in names:
        release_page(name, env)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--logs')
    parser.add_argument('--stream', action='append', default=[])
    parser.add_argument('--session', action='append', default=[])
    parser.add_argument('--timeout', type=float, default=5)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.logs:
        cleanup_logs(args.logs, args.stream, args.session)
    elif args.command:
        command = args.command[1:] if args.command[0] == '--' else args.command
        try:
            result = run_owned(command, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            sys.exit(124)
        sys.exit(result.returncode if result.returncode >= 0 else 128 - result.returncode)
    else:
        parser.error('expected --logs or a child command')
