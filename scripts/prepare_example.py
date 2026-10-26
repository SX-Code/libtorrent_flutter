#!/usr/bin/env python3
"""Generate a disposable Flutter example platform and install the FFI smoke test."""
import argparse
import pathlib
import plistlib
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
tests.mkdir(exist_ok=True)
shutil.copyfile(root / 'test/integration/engine_smoke.dart', tests / 'engine_test.dart')
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
    import re
    project.write_text(re.sub(r'MACOSX_DEPLOYMENT_TARGET = [^;]+;',
                             'MACOSX_DEPLOYMENT_TARGET = 11.0;', project.read_text()))
