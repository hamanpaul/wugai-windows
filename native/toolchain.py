"""Run MinGW with its own runtime DLLs first and without Windows error dialogs.

Usage: python native/toolchain.py --architecture x86 -- -std=c++17 source.cpp -o out.exe
       python native/toolchain.py --architecture x64 --check
"""
import argparse
from contextlib import contextmanager
import ctypes
import os
from pathlib import Path
import struct
import subprocess
import sys

COMPILERS = {
    'x64': Path('C:/msys64/mingw64/bin/g++.exe'),
    'x86': Path('C:/msys64/mingw32/bin/g++.exe'),
}


def toolchain_environment(compiler):
    """Use a per-process PATH; never change the user's saved environment."""
    result = dict(os.environ)
    directory = str(Path(compiler).resolve().parent)
    result['PATH'] = directory + os.pathsep + result.get('PATH', '')
    return result


@contextmanager
def compiler_error_mode():
    """Keep loader faults out of the user's desktop, including child processes."""
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.SetErrorMode.argtypes = [ctypes.c_uint]
    kernel.SetErrorMode.restype = ctypes.c_uint
    previous = kernel.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    try:
        yield
    finally:
        kernel.SetErrorMode(previous)


def pe_machine(path):
    data = Path(path).read_bytes()
    if data[:2] != b'MZ' or len(data) < 64:
        raise ValueError(f'Not a PE executable: {path}')
    offset = struct.unpack_from('<I', data, 60)[0]
    if data[offset:offset+4] != b'PE\0\0':
        raise ValueError(f'Invalid PE signature: {path}')
    return struct.unpack_from('<H', data, offset+4)[0]


def run_checked(command, env):
    result = subprocess.run([str(x) for x in command], env=env, capture_output=True,
                            timeout=30)
    if result.returncode:
        code = result.returncode & 0xffffffff
        detail = (result.stdout + result.stderr).decode('utf-8', errors='replace').strip()
        raise RuntimeError(f'{command[0]} failed: 0x{code:08x} {detail}')
    return (result.stdout + result.stderr).decode('utf-8', errors='replace').strip()


def preflight(architecture, compiler, env):
    expected_machine = 0x8664 if architecture == 'x64' else 0x14c
    if pe_machine(compiler) != expected_machine:
        raise ValueError(f'Compiler PE architecture does not match {architecture}')
    target = run_checked([compiler, '-dumpmachine'], env)
    expected_target = 'x86_64-w64-mingw32' if architecture == 'x64' else 'i686-w64-mingw32'
    if target != expected_target:
        raise ValueError(f'Compiler target mismatch: {target}')
    child = Path(run_checked([compiler, '-print-prog-name=cc1plus'], env))
    if not child.is_absolute():
        child = compiler.parent / child
    child = child.resolve(strict=True)
    if pe_machine(child) != expected_machine:
        raise ValueError(f'cc1plus PE architecture does not match {architecture}')
    # Executing the actual child verifies its runtime dependencies, rather
    # than assuming a successful g++ --version also proves cc1plus can start.
    run_checked([child, '--version'], env)
    version = run_checked([compiler, '--version'], env)
    if not version:
        raise RuntimeError('g++ returned no version information')
    return {'target': target, 'cc1plus': str(child), 'version': version.splitlines()[0]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--architecture', choices=COMPILERS, required=True)
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    options = parser.parse_args()
    compiler = (options.compiler or COMPILERS[options.architecture]).resolve(strict=True)
    env = toolchain_environment(compiler)
    # Children inherit the process error mode. Missing DLLs and loader faults
    # must return exit codes, never leave modal dialogs on the user's desktop.
    with compiler_error_mode():
        details = preflight(options.architecture, compiler, env)
        if options.check:
            print(f"{options.architecture}: {details['target']}; {details['version']}; {details['cc1plus']}")
            return 0
        arguments = options.arguments
        if arguments[:1] == ['--']:
            arguments = arguments[1:]
        if not arguments:
            parser.error('supply compiler arguments after --, or use --check')
        result = subprocess.run([str(compiler), *arguments], env=env)
        if result.returncode:
            print(f'Compiler exited 0x{result.returncode & 0xffffffff:08x}', file=sys.stderr)
        return result.returncode if 0 <= result.returncode <= 255 else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f'Toolchain preflight failed: {error}', file=sys.stderr)
        sys.exit(1)
