#!/usr/bin/env python3
"""Minimal zipalign: rewrite an APK so every STORED entry's data starts on a
4-byte boundary (.so files on 4096). Entry contents are unchanged, so the v1
(JAR) signature stays valid."""
import struct, sys, zipfile, zlib

src, dst = sys.argv[1], sys.argv[2]
zin = zipfile.ZipFile(src)
entries = []
with open(dst, 'wb') as f:
    for zi in zin.infolist():
        data = zin.read(zi.filename)
        name = zi.filename.encode()
        crc = zlib.crc32(data) & 0xFFFFFFFF
        if zi.compress_type == zipfile.ZIP_STORED:
            comp, method = data, 0
        else:
            c = zlib.compressobj(9, zlib.DEFLATED, -15)
            comp, method = c.compress(data) + c.flush(), 8
        off = f.tell()
        extra = b''
        if method == 0:
            al = 4096 if zi.filename.endswith('.so') else 4
            pad = (al - (off + 30 + len(name)) % al) % al
            extra = b'\0' * pad
        dt = zi.date_time
        dostime = (dt[3] << 11) | (dt[4] << 5) | (dt[5] // 2)
        dosdate = ((dt[0] - 1980) << 9) | (dt[1] << 5) | dt[2]
        f.write(struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, 0x0800 if False else 0, method, dostime, dosdate,
                            crc, len(comp), len(data), len(name), len(extra)) + name + extra)
        f.write(comp)
        entries.append((name, method, dostime, dosdate, crc, len(comp), len(data), off))
    cd = f.tell()
    for name, method, t, d, crc, cl, ul, off in entries:
        f.write(struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, 20, 20, 0, method, t, d, crc, cl, ul,
                            len(name), 0, 0, 0, 0, 0, off) + name)
    cde = f.tell()
    f.write(struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(entries), len(entries), cde - cd, cd, 0))

# verify
z = zipfile.ZipFile(dst)
assert z.testzip() is None
raw = open(dst, 'rb').read()
for zi in z.infolist():
    o = zi.header_offset
    n, e = struct.unpack_from('<HH', raw, o + 26)
    data_off = o + 30 + n + e
    if zi.compress_type == 0:
        al = 4096 if zi.filename.endswith('.so') else 4
        assert data_off % al == 0, zi.filename
    print(f'{zi.filename:40s} method={zi.compress_type} data@{data_off}')
