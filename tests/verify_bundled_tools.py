"""Verify tools from an extracted Windows x64 installer in an isolated project."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--install-dir', required=True, type=Path)
parser.add_argument('--compiler', required=True, type=Path)
parser.add_argument('--work-dir', required=True, type=Path)
args = parser.parse_args()
root = args.install_dir.resolve()
work = args.work_dir.resolve()
work.relative_to(Path(__file__).resolve().parents[1] / 'artifacts')
work.mkdir(parents=True, exist_ok=False)
manifest = json.loads((root / 'tools/build-tools-manifest.json').read_text(encoding='utf-8-sig'))
cmake = root / 'tools/cmake/bin/cmake.exe'
ninja = root / 'tools/ninja/ninja.exe'
for exe in (root / 'RedPandaIDE.exe', cmake, ninja):
    data = exe.read_bytes()
    assert data[:2] == b'MZ'
    offset = struct.unpack_from('<L', data, 0x3c)[0]
    assert data[offset:offset + 4] == b'PE\0\0'
    assert struct.unpack_from('<H', data, offset + 4)[0] == 0x8664, exe
for file in (root / 'tools/cmake' / manifest['cmake']['module'],
             root / 'tools/cmake' / manifest['cmake']['license'], root / 'tools/ninja/LICENSE.txt'):
    assert file.is_file() and file.stat().st_size > 0, file
environment = os.environ.copy()
environment['PATH'] = ';'.join((str(args.compiler.resolve().parent), str(cmake.parent), str(ninja.parent), environment.get('PATH', '')))
environment['CMAKE_HOME'] = str(root / 'tools/cmake')
def run(command):
    result = subprocess.run([str(value) for value in command], cwd=work, env=environment,
                            capture_output=True, encoding='utf-8', errors='replace', timeout=60,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    with (work / 'commands.log').open('a', encoding='utf-8') as log:
        log.write(repr([str(value) for value in command]) + '\n' + result.stdout + result.stderr + '\n')
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout.strip()
cmake_version = run((cmake, '--version'))
ninja_version = run((ninja, '--version'))
assert manifest['cmake']['version'] in cmake_version
assert ninja_version == manifest['ninja']['version']
(work / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.19)\nproject(bundle_probe LANGUAGES C)\nadd_executable(probe main.c)\n')
(work / 'main.c').write_text('int main(void) { return 0; }\n')
run((cmake, '-S', '.', '-B', 'build', '-G', 'Ninja', f'-DCMAKE_C_COMPILER={args.compiler.resolve().as_posix()}'))
run((cmake, '--build', 'build'))
run((work / 'build/probe.exe',))
cache = (work / 'build/CMakeCache.txt').read_text(encoding='utf-8')
assert 'CMAKE_GENERATOR:INTERNAL=Ninja' in cache
selected_ninja = next(line.split('=', 1)[1] for line in cache.splitlines()
                      if line.startswith('CMAKE_MAKE_PROGRAM:'))
assert Path(selected_ninja).samefile(ninja)  # CMake may use a Windows 8.3 alias.
report = {'cmake': manifest['cmake']['version'], 'ninja': ninja_version,
          'architecture': 'x64', 'module_and_licenses_present': True,
          'configure_build_run': True, 'install_dir': str(root)}
(work / 'verification.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(report, ensure_ascii=False, indent=2))
