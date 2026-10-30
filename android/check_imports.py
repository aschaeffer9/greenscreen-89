#!/usr/bin/env python3
"""Fail the build if the app imports a symbol the target Android version doesn't
export from the library it's linked against (the stub libraries accept anything,
so a missing symbol would only show up as an instant crash on the phone).
usage: check_imports.py API_LEVEL STUB_DIR BIONIC_LIBC_MAP LIBANDROID_MAP AAUDIO_H"""
import re, sys

api = int(sys.argv[1]); stubs = sys.argv[2]


def parse_map(path):
    ok = {}
    level, skip = 0, False
    for line in open(path):
        m = re.match(r'^(\w+)\s*\{(.*)$', line)
        if m:
            name, rest = m.group(1), m.group(2)
            skip = any(t in name for t in ('PRIVATE', 'PLATFORM', 'DEPRECATED'))
            lv = re.search(r'introduced(?:-arm64)?=(\d+)', rest)
            level = int(lv.group(1)) if lv else {'LIBC_O': 26}.get(name, 0)
            continue
        m = re.match(r'^\s+([\w.]+);\s*(?:#(.*))?$', line)
        if not m or skip:
            continue
        sym, tags = m.group(1), (m.group(2) or '')
        lv = level
        t = re.search(r'introduced(?:-arm64)?=(\d+)', tags)
        if t:
            lv = int(t.group(1))
        words = set(re.findall(r'\b(arm|arm64|x86|x86_64|riscv64)\b', tags))
        if words and 'arm64' not in words:
            continue
        if any(w in tags for w in ('apex', 'platform-only', 'systemapi', 'llndk')) and not t:
            continue
        ok[sym] = min(lv, ok.get(sym, 999))
    return ok


libc = parse_map(sys.argv[3])
libandroid = parse_map(sys.argv[4])
aaudio = {}
for m in re.finditer(r'AAUDIO_API[^;]*?\b(AAudio\w+)\s*\([^;]*?\)\s*__INTRODUCED_IN\((\d+)\)', open(sys.argv[5]).read(), re.S):
    aaudio[m.group(1)] = int(m.group(2))

bad = []
for lib, table in (('libc', libc), ('libandroid', libandroid), ('libaaudio', aaudio)):
    syms = re.findall(r'void (\w+)\(void\)\{\}', open(f'{stubs}/{lib}.c').read())
    for s in syms:
        if s not in table:
            bad.append(f'{s}: not exported by {lib}')
        elif table[s] > api:
            bad.append(f'{s}: {lib} only has it from API {table[s]}')
log = re.findall(r'void (\w+)\(void\)\{\}', open(f'{stubs}/liblog.c').read())
bad += [f'{s}: unexpected liblog import' for s in log if s not in ('__android_log_print', '__android_log_write')]
if bad:
    print('IMPORT CHECK FAILED - the app would crash on launch:')
    for b in bad:
        print('  ' + b)
    sys.exit(1)
print(f'imports OK: every symbol exists on Android API {api}')
