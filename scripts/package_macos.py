#!/usr/bin/env python3
"""Build a local macOS arm64 review bundle. Private config is opt-in, never logged."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MACH_MAGIC = {b'\xfe\xed\xfa\xce', b'\xce\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xcf\xfa\xed\xfe',
              b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca', b'\xca\xfe\xba\xbf'}


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


def copy(source, target, ignore=None):
    if source.is_dir():
        shutil.copytree(source, target, symlinks=True, ignore=ignore, dirs_exist_ok=True)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def excluded(directory, names):
    return [name for name in names if name in {'__pycache__', 'node_modules', '.git', '.pytest_cache', '.DS_Store'}
            or name.startswith('.env') or name.endswith(('.pyc', '.dSYM'))]


def macho(path):
    if path.is_symlink() or not path.is_file():
        return False
    with path.open('rb') as stream:
        return stream.read(4) in MACH_MAGIC


def dependencies(path):
    result = subprocess.run(['otool', '-L', str(path)], check=True, capture_output=True, text=True)
    return [line.strip().split(' (compatibility', 1)[0] for line in result.stdout.splitlines()[1:]
            if line.startswith('\t')]


def bundle_mysql(source, destination):
    copy(source / 'bin/mysqld', destination / 'bin/mysqld')
    copy(source / 'share', destination / 'share')
    copy(source / 'lib/plugin', destination / 'lib/plugin')
    copy(source / 'LICENSE', destination / 'LICENSE')
    dep_dir = destination / 'lib/dependencies'
    dep_dir.mkdir(parents=True)
    pending = [path for path in destination.rglob('*') if macho(path)]
    original_paths = {path: (source / path.relative_to(destination)).resolve() for path in pending}
    checked, names = set(), {}
    while pending:
        binary = pending.pop()
        if binary in checked:
            continue
        checked.add(binary)
        original = original_paths[binary]
        commands = subprocess.check_output(['otool', '-l', str(original)], text=True).splitlines()
        rpaths = [commands[index + 2].strip().split(' ', 1)[1].rsplit(' (offset', 1)[0]
                  for index, line in enumerate(commands) if line.strip() == 'cmd LC_RPATH']
        for old in dependencies(original):
            if old.startswith(('/System/Library/', '/usr/lib/')):
                continue
            candidates = []
            if old.startswith('/'):
                candidates = [Path(old)]
            elif old.startswith('@loader_path/'):
                candidates = [original.parent / old.removeprefix('@loader_path/')]
            elif old.startswith('@rpath/'):
                for rpath in [*rpaths, str(original.parent), str(source / 'lib')]:
                    rpath = rpath.replace('@loader_path', str(original.parent)).replace('@executable_path', str(source / 'bin'))
                    candidates.append(Path(rpath) / old.removeprefix('@rpath/'))
            actual = next((candidate.resolve() for candidate in candidates if candidate.is_file()), None)
            if actual is None:
                raise RuntimeError('Cannot resolve a MySQL native dependency')
            if actual == original:
                continue
            target = dep_dir / actual.name
            if actual.name in names and names[actual.name] != actual:
                raise RuntimeError('Conflicting dylib names')
            names[actual.name] = actual
            if not target.exists():
                copy(actual, target)
                original_paths[target] = actual
                pending.append(target)
            relative = os.path.relpath(target, binary.parent)
            run('install_name_tool', '-change', old, '@loader_path/' + relative, binary)
        if binary.suffix in {'.dylib', '.so'}:
            run('install_name_tool', '-id', '@rpath/' + binary.name, binary)
    vendor_roots = set()
    for original in original_paths.values():
        parts = original.parts
        if 'Cellar' in parts:
            vendor_roots.add(Path(*parts[:parts.index('Cellar') + 3]))
    for vendor in vendor_roots:
        for notice in vendor.rglob('*'):
            if notice.is_file() and notice.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE')):
                copy(notice, destination / 'licenses' / vendor.parent.name / vendor.name / notice.relative_to(vendor))
    return len(checked)


def python_runtime(base, venv, destination):
    (destination / 'bin').mkdir(parents=True)
    copy(base / 'bin/python3.12', destination / 'bin/python3.12')
    (destination / 'bin/python3').symlink_to('python3.12')
    (destination / 'bin/python').symlink_to('python3.12')
    copy(base / 'lib/python3.12', destination / 'lib/python3.12',
         ignore=lambda d, names: excluded(d, names) + [n for n in names if n in {'site-packages', 'test', 'tests', 'tkinter', 'idlelib', 'ensurepip'}])
    package_root = venv / 'lib/python3.12/site-packages'
    copy(package_root, destination / 'lib/python3.12/site-packages',
         ignore=lambda d, names: excluded(d, names) + [n for n in names if n in {'cmake', 'ninja', 'ruff', 'pip'}
               or n.startswith(('cmake-', 'ninja-', 'ruff-', 'pip-'))])
    for name in ('LICENSE', 'LICENSE.txt', 'COPYING'):
        if (base / name).is_file():
            copy(base / name, destination / name)


def project_payload(project):
    (project / 'data_analysis').mkdir(parents=True)
    for child in (ROOT / 'data_analysis').iterdir():
        if child.name in {'datasets', 'outputs', 'frontend', 'tests', '__pycache__'} or child.name.startswith('.'):
            continue
        copy(child, project / 'data_analysis' / child.name, ignore=excluded)
    for name in ('analytics_full_180d_v1', 'advanced_analytics_v2', 'charging_full_180d_v2'):
        copy(ROOT / 'data_analysis/datasets' / name, project / 'data_analysis/datasets' / name, ignore=excluded)
    for name in ('chargepilot', 'ml_load', 'ml_availability_delivery', 'ml_insights_delivery'):
        copy(ROOT / 'data_analysis/outputs' / name, project / 'data_analysis/outputs' / name,
             ignore=lambda d, names: excluded(d, names) + [n for n in names if n == 'joined_usable.pkl'])
    copy(ROOT / 'data_analysis/frontend/dist', project / 'data_analysis/frontend/dist')
    copy(ROOT / 'scripts/managed_web_service.py', project / 'scripts/managed_web_service.py')
    copy(ROOT / 'packaging/run_review.py', project / 'packaging/run_review.py')
    for name in ('LICENSE', 'NOTICE'):
        if (ROOT / name).exists():
            copy(ROOT / name, project / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--qt', type=Path, required=True)
    parser.add_argument('--python-base', type=Path, required=True)
    parser.add_argument('--venv', type=Path, required=True)
    parser.add_argument('--mysql', type=Path, required=True)
    parser.add_argument('--mysql-seed', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--include-private-config', action='store_true')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('Build this bundle on macOS with the matching arm64 toolchain.')
    output = args.output.resolve()
    if output.exists():
        parser.error('Output already exists; choose a fresh staging directory.')
    if not (args.mysql_seed / 'data/auto.cnf').is_file():
        parser.error('A closed, prepared demonstration MySQL seed is required.')
    status_file = args.mysql_seed / 'status.json'
    if not status_file.is_file() or json.loads(status_file.read_text(encoding='utf8')).get('status') != 'READY_STOPPED':
        parser.error('The demonstration seed must explicitly report READY_STOPPED.')
    app = output / '充能智析.app'
    copy(args.build_dir / 'server/charging-server.app', app)
    contents = app / 'Contents'
    resources, binaries = contents / 'Resources', contents / 'MacOS'
    resources.mkdir(exist_ok=True)
    copy(args.build_dir / 'client/charging-client', binaries / 'charging-client')
    print('Deploying Qt and QML dependencies…', flush=True)
    run(args.qt / 'bin/macdeployqt', app, '-verbose=1', '-no-codesign',
        '-executable=' + str(binaries / 'charging-client'), '-qmldir=' + str(ROOT / 'client/qml'))
    # macdeployqt includes unrelated SQL drivers whose vendor runtimes are not
    # used here. Qt business storage uses only SQLite; Python owns MySQL.
    for driver in (contents / 'PlugIns/sqldrivers').glob('*'):
        if driver.name != 'libqsqlite.dylib':
            driver.unlink()
    print('Copying CPU Python environment and model artifacts…', flush=True)
    python_runtime(args.python_base, args.venv, resources / 'python')
    project = resources / 'project'
    project_payload(project)
    licenses = resources / 'ThirdPartyLicenses'
    qt_licenses = args.qt.parent.parent / 'Licenses'
    if qt_licenses.is_dir():
        copy(qt_licenses, licenses / 'Qt')
    chromium = args.qt.parent / 'Src/qtwebengine/LICENSE.Chromium'
    if chromium.is_file():
        copy(chromium, licenses / 'QtWebEngine-LICENSE.Chromium')
    qt_spdx = args.qt.parent / 'Src/qtwebengine/LICENSES'
    if qt_spdx.is_dir():
        copy(qt_spdx, licenses / 'Qt-SPDX')
    chromium_source = args.qt.parent / 'Src/qtwebengine/src/3rdparty/chromium'
    if chromium_source.is_dir():
        for notice in chromium_source.rglob('*'):
            if notice.is_file() and notice.name.upper().startswith(('LICENSE', 'COPYING', 'NOTICE')):
                copy(notice, licenses / 'Chromium' / notice.relative_to(chromium_source))
    licenses.mkdir(parents=True, exist_ok=True)
    (licenses / 'README.txt').write_text(
        'Third-party components retain their own licenses. Qt Charts is GPLv3/commercial; '
        'do not describe all Qt modules as LGPL. Qt SPDX texts and Chromium notices are '
        'included here. MySQL and native dependency notices are in ../mysql/LICENSE and '
        '../mysql/licenses. Python and wheel notices remain in ../python/lib/python3.12 '
        'and its site-packages *.dist-info directories. This is an ad-hoc signed local '
        'review build, not an Apple notarized release.\n', encoding='utf8')
    (project / 'config').mkdir(parents=True, exist_ok=True)
    source_config = ROOT / ('config/services.local.json' if args.include_private_config else 'config/services.example.json')
    if not source_config.is_file():
        raise RuntimeError('Requested services configuration is unavailable')
    copy(source_config, project / 'config/services.local.json')
    (project / 'config/services.local.json').chmod(0o600)
    print('Bundling MySQL with relocated native libraries…', flush=True)
    mysql_binaries = bundle_mysql(args.mysql.resolve(), resources / 'mysql')
    copy(args.mysql_seed / 'data', resources / 'mysql-seed/data')
    copy(args.mysql_seed / 'environment.json', resources / 'mysql-seed/environment.json')
    (resources / 'mysql-seed/environment.json').chmod(0o600)
    (binaries / 'scripts').symlink_to('../Resources/project/scripts')
    (binaries / 'charging-server').rename(binaries / 'charging-server-real')
    launcher = binaries / 'charging-server'
    run('clang', '-arch', 'arm64', '-mmacosx-version-min=15.0', '-O2', '-Wall', '-Wextra', '-Werror',
        ROOT / 'packaging/macos_launcher.c', '-o', launcher)
    plist_path = contents / 'Info.plist'
    with plist_path.open('rb') as stream:
        plist = plistlib.load(stream)
    plist.update(CFBundleName='充能智析', CFBundleDisplayName='充能智析 电动汽车充电平台与智能运营系统',
                 CFBundleIdentifier='org.chargingplatform.competitionreview',
                 CFBundleShortVersionString='1.0.0', CFBundleVersion='20261019', LSMinimumSystemVersion='15.0')
    with plist_path.open('wb') as stream:
        plistlib.dump(plist, stream)
    command = output / '启动评审.command'
    command.write_text('#!/bin/sh\nset -eu\nHERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
                       'exec "$HERE/充能智析.app/Contents/MacOS/charging-server" "$@"\n', encoding='utf8')
    command.chmod(0o755)
    print('Checking architecture, removing developer search paths and signing…', flush=True)
    native_count = 0
    for binary in contents.rglob('*'):
        if not macho(binary):
            continue
        native_count += 1
        architectures = subprocess.check_output(['lipo', '-archs', str(binary)], text=True).split()
        if 'arm64' not in architectures:
            raise RuntimeError('A non-arm64 runtime was included')
        if len(architectures) > 1:
            temporary = binary.with_name(binary.name + '.thin')
            run('lipo', binary, '-thin', 'arm64', '-output', temporary)
            shutil.copymode(binary, temporary)
            temporary.replace(binary)
        load_commands = subprocess.check_output(['otool', '-l', str(binary)], text=True).splitlines()
        for index, line in enumerate(load_commands):
            if line.strip() == 'cmd LC_RPATH':
                old = load_commands[index + 2].strip().split(' ', 1)[1].rsplit(' (offset', 1)[0]
                if old.startswith(('/Users/', '/opt/homebrew/')):
                    run('install_name_tool', '-delete_rpath', old, binary)
        # Some wheel-vendored dylibs retain a build-machine LC_ID_DYLIB even
        # though their consumers use @loader_path. Normalize that metadata too.
        if binary.suffix == '.dylib':
            run('install_name_tool', '-id', '@rpath/' + binary.name, binary)
        for dependency in dependencies(binary):
            if dependency.startswith('/') and not dependency.startswith(('/System/Library/', '/usr/lib/')):
                raise RuntimeError('Unbundled developer dependency remains')
        if binary.name == 'QtWebEngineProcess':
            # Match the entitlements shipped on Qt's Chromium helper after
            # lipo/install_name_tool have invalidated its original signature.
            run('codesign', '--force', '--sign', '-', '--entitlements',
                ROOT / 'packaging/qtwebengine-entitlements.plist', binary)
        else:
            run('codesign', '--force', '--sign', '-', '--preserve-metadata=entitlements', binary)
    for path in contents.rglob('*'):
        if path.is_symlink() and not path.resolve().is_relative_to(contents):
            raise RuntimeError('A bundle symlink escapes its directory')
    run('codesign', '--force', '--deep', '--sign', '-', '--preserve-metadata=entitlements', app)
    run('codesign', '--verify', '--deep', '--strict', app)
    manifest = {'platform': 'macOS 15+ Apple Silicon', 'qt': '6.12.0', 'python': '3.12',
                'mysql': '8.4', 'nativeFiles': native_count, 'mysqlNativeFiles': mysql_binaries,
                'privateServicesConfigIncluded': args.include_private_config,
                'configurationCopiedByteForByte': source_config.read_bytes() == (project / 'config/services.local.json').read_bytes(),
                'demoDataOnly': True, 'sourceCheckoutRequired': False, 'developerToolsRequired': False}
    (output / 'package-manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print('Local review bundle prepared. Move it to another directory and run --verify before delivery.', flush=True)


if __name__ == '__main__':
    main()
