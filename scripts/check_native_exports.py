#!/usr/bin/env python3
"""Verify every Dart FFI lookup against a newly built native library."""
import argparse
import ctypes
import pathlib
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('library', type=pathlib.Path)
parser.add_argument('--nm', help='Use nm for cross-compiled/static libraries')
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
symbols = sorted(set(re.findall(r"'((?:lt_)[a-z_]+)'", (root / 'lib/src/ffi_bindings.dart').read_text())))
if not symbols:
    raise SystemExit('No FFI symbols found')
if args.nm:
    result = subprocess.run([args.nm, '-g', str(args.library)], capture_output=True, text=True, check=True)
    # Undefined references (U) do not count as exports. Darwin prefixes C symbols with _.
    defined = set(re.findall(r'\b[TtWw]\s+_?(lt_[a-z_]+)\s*$', result.stdout, re.MULTILINE))
    missing = set(symbols) - defined
else:
    library = ctypes.CDLL(str(args.library.resolve()))
    missing = {name for name in symbols if not hasattr(library, name)}
if missing:
    raise SystemExit('Missing FFI exports: ' + ', '.join(sorted(missing)))
print(f'Verified {len(symbols)} FFI exports in {args.library}')
