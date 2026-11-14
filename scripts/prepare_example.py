#!/usr/bin/env python3
"""Generate a disposable Flutter example platform and install the FFI smoke test."""
import argparse
import pathlib
import plistlib
import re
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('platform', choices=['ios', 'macos', 'android', 'linux', 'windows'])
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
example = root / 'example'
flutter = shutil.which('flutter')
if flutter is None:
    raise SystemExit('Flutter must be on PATH')
subprocess.run([flutter, 'create', '--platforms=' + args.platform,
                '--project-name', 'libtorrent_flutter_example', '.'], cwd=example,
               check=True, shell=False)
tests = example / 'integration_test'
if tests.exists():
    for stale in tests.glob('*.dart'):
        stale.unlink()
else:
    tests.mkdir()
for source in sorted((root / 'test/integration').glob('*.dart')):
    target = 'engine_test.dart' if source.name == 'engine_smoke.dart' else source.name
    shutil.copyfile(source, tests / target)
relative_import = re.compile(r"""^\s*import\s+['"]([^'"]+)['"]\s*;""", re.MULTILINE)
for test_file in tests.glob('*.dart'):
    for imported in relative_import.findall(test_file.read_text()):
        if imported.startswith(('dart:', 'package:')):
            continue
        if not (test_file.parent / imported).is_file():
            raise SystemExit(
                f'{test_file.name} imports missing integration source: {imported}'
            )
if args.platform == 'macos':
    for name in ['DebugProfile', 'Release']:
        path = example / 'macos/Runner' / (name + '.entitlements')
        with path.open('rb') as file:
            data = plistlib.load(file)
        data['com.apple.security.network.client'] = True
        data['com.apple.security.network.server'] = True
        with path.open('wb') as file:
            plistlib.dump(data, file)
    project = example / 'macos/Runner.xcodeproj/project.pbxproj'
    project.write_text(re.sub(r'MACOSX_DEPLOYMENT_TARGET = [^;]+;',
                             'MACOSX_DEPLOYMENT_TARGET = 11.0;', project.read_text()))
