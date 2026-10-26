#!/usr/bin/env python3
"""Check readelf -lW output for Android 16 KB ELF LOAD segment alignment."""
import pathlib
import sys

loads = [line.split() for line in pathlib.Path(sys.argv[1]).read_text().splitlines()
         if line.lstrip().startswith('LOAD ')]
if not loads or any(int(fields[-1], 16) < 16384 for fields in loads):
    raise SystemExit('ELF LOAD segments are not all aligned to at least 16 KB')
print(f'Checked {len(loads)} LOAD segments: alignment >= 16 KB')
