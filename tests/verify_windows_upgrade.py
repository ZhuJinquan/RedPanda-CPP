"""Run the production NSIS flow against isolated per-user fixture installs.

All product names, association IDs, registry keys and config paths in fixture
scripts are replaced so an installed real IDE cannot be changed by these tests.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'artifacts' / 'upgrade-tests'
NSIS = ROOT / '.tools/nsis-3.12/makensis.exe'
KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\RedPanda-Upgrade-Test'


def isolated(script):
    return (script.replace('Red Panda C++', 'Red Panda Upgrade Test')
            .replace('小熊猫C++', '小熊猫升级测试')
            .replace('RedPanda-C++', 'RedPanda-Upgrade-Test')
            .replace('RedPanda-Cpp', 'RedPanda-Upgrade-Test')
            .replace('DevCpp.', 'RedPandaUpgradeTest.')
            .replace('$APPDATA\\RedPandaIDE', '$APPDATA\\RedPandaUpgradeTest')
            .replace('!insertmacro MULTIUSER_PAGE_INSTALLMODE', '')
            .replace('!define MULTIUSER_EXECUTIONLEVEL Highest', '!define MULTIUSER_EXECUTIONLEVEL User'))


def prepare():
    WORK.mkdir(parents=True, exist_ok=True)
    old = subprocess.check_output(
        ['git', 'show', 'cbe084d162987fb90867b12f279af94c7a1d6a38:platform/windows/installer-scripts/redpanda.nsi'], cwd=ROOT).decode('utf-8')
    new = (ROOT / 'platform/windows/installer-scripts/redpanda.nsi').read_text(encoding='utf-8')
    for flavor, script, version in [('old', old, '3.4'), ('new', new, '3.5.0-beta.2')]:
        folder = WORK / flavor
        folder.mkdir(exist_ok=True)
        fixture_script = isolated(script)
        if flavor == 'old':
            # The upstream .onInit overwrites /D with MultiUser's default.
            # Simulate selecting the directory page without automating the UI.
            position = fixture_script.index('FunctionEnd', fixture_script.index('Function .onInit'))
            fixture_script = (fixture_script[:position] + '  StrCpy $INSTDIR "${FIXTURE_INSTALL_DIR}"\n'
                              + fixture_script[position:])
        (folder / 'redpanda.nsi').write_text(fixture_script, encoding='utf-8')
        for name in ('lang.nsh', 'utils.nsh'):
            (folder / name).write_bytes((ROOT / 'platform/windows/installer-scripts' / name).read_bytes())
        for name in ('RedPandaIDE.exe', 'ConsolePauser.exe', 'astyle.exe'):
            (folder / name).write_bytes((ROOT / 'artifacts/debugwatch_fixture.exe').read_bytes())
        (folder / 'devcpp.ico').write_bytes((ROOT / 'RedPandaIDE/images/devcpp.ico').read_bytes())
        for name in ('LICENSE', 'NEWS.md', 'README.md', 'qt.conf'):
            (folder / name).write_text(flavor, encoding='utf-8')
        for name in ('mingw32', 'mingw64', 'templates'):
            (folder / name).mkdir(exist_ok=True)
            (folder / name / 'payload.txt').write_text(flavor)
        if flavor == 'new':
            for path in ('tools/cmake/bin/cmake.exe', 'tools/cmake/share/cmake-fixture/Modules/CMake.cmake',
                         'tools/cmake/doc/cmake/LICENSE.rst', 'tools/ninja/ninja.exe', 'tools/ninja/LICENSE.txt',
                         'tools/build-tools-manifest.json'):
                target = folder / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('new fixture payload')
        for arch in (('x86', 'x64') if flavor == 'old' else ('x64',)):
            command = [str(NSIS), '/V2', f'/DAPP_VERSION={version}', f'/DARCH={arch}',
                       f'/DFINALNAME={folder}/setup-{arch}.exe', '/DMINGW32_COMPILER_NAME=Test32',
                       '/DMINGW64_COMPILER_NAME=Test64', '/DHAVE_MINGW32', '/DHAVE_MINGW64',
                       '/DREQUIRED_WINDOWS_BUILD=7601', '/DREQUIRED_WINDOWS_NAME=Windows 7', 'redpanda.nsi']
            if flavor == 'old':
                command.insert(-1, f'/DFIXTURE_INSTALL_DIR={WORK / f"installed {arch} 中文"}')
            else:
                command.insert(-1, '/DHAVE_BUILD_TOOLS')
            result = subprocess.run(command, cwd=folder, capture_output=True, text=True)
            (folder / f'compile-{arch}.log').write_text(result.stdout + result.stderr, encoding='utf-8')
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
    print('Fixture installers compiled.')


def run():
    import winreg
    report = []
    config = Path(os.environ['APPDATA']) / 'RedPandaUpgradeTest'
    if config.exists():
        raise RuntimeError(f'Fixture config already exists: {config}')
    # Refuse to replace an existing test installation from another run.
    for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_READ | view):
                raise RuntimeError('A fixture uninstall key already exists; inspect before rerunning.')
        except FileNotFoundError:
            pass
    config.mkdir()
    sentinel = config / 'settings.ini'
    sentinel.write_bytes(b'compiler.path=external-toolchain\r\nlanguage=zh_CN\r\n')
    config_bytes = sentinel.read_bytes()
    for old_arch in ('x86', 'x64'):
        install = WORK / f'installed {old_arch} 中文'
        if install.exists():
            raise RuntimeError(f'Fixture install directory already exists: {install}')
        setup = WORK / 'old' / f'setup-{old_arch}.exe'
        # NSIS explicitly requires /D= and _?= to be unquoted final arguments.
        old_result = subprocess.run(f'"{setup}" /S /CurrentUser /D={install}')
        assert old_result.returncode == 0, old_result.returncode
        project = install / 'projects' / 'original.dev'
        project.parent.mkdir()
        project.write_bytes(b'[Project]\r\nName=keep me\r\n')
        local_config = install / 'config'
        local_config.mkdir()
        (local_config / 'settings.ini').write_bytes(config_bytes)
        user_tool = install / 'tools/custom/keep.txt'
        user_tool.parent.mkdir(parents=True)
        user_tool.write_text('preserve user tool')
        (install / 'mingw64/custom-old-marker.txt').write_text('old compiler addition')
        # Test legacy registrations that only have UninstallString as well.
        if old_arch == 'x86':
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0,
                                winreg.KEY_SET_VALUE | winreg.KEY_WOW64_32KEY) as key:
                winreg.DeleteValue(key, 'InstallLocation')
        target = subprocess.Popen([str(install / 'RedPandaIDE.exe')],
                                  creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            time.sleep(0.3)
            blocked = subprocess.run([str(WORK / 'new/setup-x64.exe'), '/S', '/CurrentUser'])
            assert blocked.returncode == 2, blocked.returncode
            assert (install / 'NEWS.md').read_text() == 'old'
            assert (install / 'mingw64/custom-old-marker.txt').exists()
        finally:
            target.terminate()
            target.wait(timeout=10)
        upgraded = subprocess.run([str(WORK / 'new/setup-x64.exe'), '/S', '/CurrentUser'])
        assert upgraded.returncode == 0, upgraded.returncode
        assert (install / 'NEWS.md').read_text() == 'new'
        assert (install / 'mingw64/payload.txt').read_text() == 'new'
        assert (install / 'mingw32/payload.txt').read_text() == 'new'
        assert not (install / 'mingw64/custom-old-marker.txt').exists()
        for path in ('tools/cmake/bin/cmake.exe', 'tools/cmake/share/cmake-fixture/Modules/CMake.cmake',
                     'tools/cmake/doc/cmake/LICENSE.rst', 'tools/ninja/ninja.exe', 'tools/ninja/LICENSE.txt',
                     'tools/build-tools-manifest.json'):
            assert (install / path).read_text() == 'new fixture payload'
        assert user_tool.read_text() == 'preserve user tool'
        for extension in ('dev', 'c', 'cpp', 'cxx', 'cc', 'hxx', 'h', 'hpp'):
            association = rf'Software\Classes\RedPandaUpgradeTest.{extension}\Shell\Open\Command'
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, association) as key:
                command = winreg.QueryValueEx(key, '')[0]
                assert command == f'"{install / "RedPandaIDE.exe"}" "%1"', command
        assert project.read_bytes() == b'[Project]\r\nName=keep me\r\n'
        assert (local_config / 'settings.ini').read_bytes() == config_bytes
        assert sentinel.read_bytes() == config_bytes
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            assert winreg.QueryValueEx(key, 'InstallLocation')[0] == str(install)
            assert winreg.QueryValueEx(key, 'DisplayVersion')[0] == '3.5.0-beta.2'
        repeat = subprocess.run([str(WORK / 'new/setup-x64.exe'), '/S', '/CurrentUser'])
        assert repeat.returncode == 0, repeat.returncode
        # _?= makes NSIS wait instead of spawning the temporary uninstaller.
        removed = subprocess.run(f'"{install / "uninstall.exe"}" /S /CurrentUser _?={install}')
        assert removed.returncode == 0, removed.returncode
        assert not (install / 'RedPandaIDE.exe').exists()
        assert project.exists()
        assert sentinel.read_bytes() == config_bytes
        assert not (install / 'tools/cmake').exists()
        assert not (install / 'tools/ninja').exists()
        assert not (install / 'tools/build-tools-manifest.json').exists()
        assert user_tool.read_text() == 'preserve user tool'
        report.append({'old_arch': old_arch, 'new_arch': 'x64',
                       'auto_location': str(install), 'running_ide_blocked': True,
                       'program_and_compilers_replaced': True, 'projects_and_settings_unchanged': True,
                       'repeat_upgrade': True, 'uninstall': True,
                       'all_eight_association_commands_quoted': True,
                       'bundled_tools_installed_and_uninstalled': True,
                       'unrelated_user_tools_preserved': True})
    # Only delete the one fixture config file created by this test.
    sentinel.unlink()
    config.rmdir()
    (WORK / 'verification.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


def failure():
    import winreg
    folder = WORK / 'uninstall-failure'
    folder.mkdir(exist_ok=True)
    script = folder / 'failed-uninstaller.nsi'
    script.write_text('Unicode true\nRequestExecutionLevel user\n'
                      'OutFile "uninstall.exe"\nSilentInstall silent\n'
                      'Section\nSetErrorLevel 7\nQuit\nSectionEnd\n')
    subprocess.run([str(NSIS), '/V2', str(script)], cwd=folder, check=True)
    (folder / 'RedPandaIDE.exe').write_bytes((ROOT / 'artifacts/debugwatch_fixture.exe').read_bytes())
    (folder / 'NEWS.md').write_text('old')
    view = winreg.KEY_WOW64_64KEY
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_READ | view):
            raise RuntimeError('A fixture uninstall key already exists.')
    except FileNotFoundError:
        pass
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_SET_VALUE | view) as key:
        for name, value in [('InstallLocation', str(folder)),
                            ('UninstallString', f'"{folder / "uninstall.exe"}"'),
                            ('DisplayVersion', '3.4'), ('CurrentUser', '1')]:
            winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)
    try:
        result = subprocess.run([str(WORK / 'new/setup-x64.exe'), '/S', '/CurrentUser'])
        assert result.returncode == 2, result.returncode
        assert (folder / 'NEWS.md').read_text() == 'old'
        assert not (folder / 'ConsolePauser.exe').exists()
        result = {'old_uninstaller_exit': 7, 'installer_exit': result.returncode,
                  'installation_stopped_before_replacing_files': True}
        (WORK / 'uninstall-failure.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result))
    finally:
        winreg.DeleteKeyEx(winreg.HKEY_CURRENT_USER, KEY, view)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=('prepare', 'run', 'failure'))
    parser.add_argument('--work-dir', type=Path, default=WORK)
    args = parser.parse_args()
    WORK = args.work_dir.resolve()
    WORK.relative_to(ROOT / 'artifacts')
    {'prepare': prepare, 'run': run, 'failure': failure}[args.mode]()
