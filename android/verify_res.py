#!/usr/bin/env python3
"""Independent decoder for binary XML and resources.arsc, used to check the
hand-built files (and sanity-checked against the framework's own arsc)."""
import struct, sys


def read_pool(b, off):
    typ, hsz, size, cnt, scnt, flags, sstart, stystart = struct.unpack_from('<HHIIIIII', b, off)
    assert typ == 1, hex(typ)
    utf8 = flags & 0x100
    offs = struct.unpack_from(f'<{cnt}I', b, off + hsz)
    out = []
    for o in offs:
        p = off + sstart + o
        if utf8:
            n = b[p]; p += 1
            if n & 0x80: n = ((n & 0x7f) << 8) | b[p]; p += 1
            m = b[p]; p += 1
            if m & 0x80: m = ((m & 0x7f) << 8) | b[p]; p += 1
            s = b[p:p + m].decode('utf-8'); assert b[p + m] == 0
        else:
            n = struct.unpack_from('<H', b, p)[0]; p += 2
            if n & 0x8000: n = ((n & 0x7fff) << 16) | struct.unpack_from('<H', b, p)[0]; p += 2
            s = b[p:p + 2 * n].decode('utf-16-le'); assert b[p + 2 * n:p + 2 * n + 2] == b'\0\0'
        out.append(s)
    return out, size


def axml(b):
    typ, hsz, size = struct.unpack_from('<HHI', b, 0)
    assert typ == 3 and hsz == 8 and size == len(b), (typ, hsz, size, len(b))
    off = 8
    strings = resmap = None
    depth = 0
    lines = []
    nsmap = {}
    while off < len(b):
        t, h, sz = struct.unpack_from('<HHI', b, off)
        assert sz >= h and off + sz <= len(b)
        if t == 1:
            strings, _ = read_pool(b, off)
        elif t == 0x180:
            resmap = struct.unpack_from(f'<{(sz - 8) // 4}I', b, off + 8)
        elif t == 0x100:
            p, u = struct.unpack_from('<II', b, off + 16); nsmap[u] = strings[p]
        elif t == 0x102:
            ns, name, ast, asz, acnt, idi, cli, sti = struct.unpack_from('<IIHHHHHH', b, off + 16)
            assert ast == 20 and asz == 20
            attrs = []
            for i in range(acnt):
                ans, an, raw, vs, r0, dt, data = struct.unpack_from('<IIIHBBI', b, off + 16 + 20 + i * 20)
                assert vs == 8 and r0 == 0
                rid = resmap[an] if resmap and an < len(resmap) else None
                pre = (nsmap[ans] + ':') if ans != 0xFFFFFFFF else ''
                if dt == 3: val = repr(strings[data]); assert raw == data
                elif dt == 1: val = f'@0x{data:08x}'
                elif dt == 0x12: val = 'true' if data else 'false'
                elif dt == 0x11: val = hex(data)
                else: val = str(data)
                attrs.append(f'{pre}{strings[an]}{"" if rid is None else f"[{rid:#010x}]"}={val}')
                if ans != 0xFFFFFFFF:
                    assert rid is not None, 'android attr without resource id'
            lines.append('  ' * depth + f'<{strings[name]} ' + ' '.join(attrs) + '>')
            depth += 1
        elif t == 0x103:
            depth -= 1
        elif t == 0x101:
            pass
        else:
            raise Exception(hex(t))
        off += sz
    assert depth == 0
    return '\n'.join(lines)


def arsc(b, verbose=True, limit_types=None):
    t, h, size, npk = struct.unpack_from('<HHII', b, 0)
    assert t == 2 and h == 12 and size == len(b), (t, h, size, len(b))
    off = 12
    values = None
    result = {}
    cfg_sizes = set()
    while off < len(b):
        t, h, sz = struct.unpack_from('<HHI', b, off)
        if t == 1:
            values, _ = read_pool(b, off)
        elif t == 0x200:
            pid = struct.unpack_from('<I', b, off + 8)[0]
            pname = b[off + 12:off + 12 + 256].decode('utf-16-le').rstrip('\0')
            tso, lpt, kso, lpk = struct.unpack_from('<IIII', b, off + 268)
            assert h in (284, 288), h
            types, _ = read_pool(b, off + tso)
            keys, _ = read_pool(b, off + kso)
            p = off + h
            end = off + sz
            while p < end:
                ct, chh, csz = struct.unpack_from('<HHI', b, p)
                if ct == 0x0202:
                    tid, r0, r1, ec = struct.unpack_from('<BBHI', b, p + 8)
                    assert chh == 16 and csz == 16 + 4 * ec
                elif ct == 0x0201:
                    tid, fl, r, ec, es = struct.unpack_from('<BBHII', b, p + 8)
                    csize = struct.unpack_from('<I', b, p + 20)[0]
                    cfg_sizes.add(csize)
                    assert chh == 20 + csize, (chh, csize)
                    assert es == chh + 4 * ec or fl & 1, (es, chh, ec)
                    if not (fl & 1):  # dense
                        eoffs = struct.unpack_from(f'<{ec}I', b, p + chh)
                        for ei, eo in enumerate(eoffs):
                            if eo == 0xFFFFFFFF: continue
                            q = p + es + eo
                            esz, efl, key = struct.unpack_from('<HHI', b, q)
                            if not (efl & 1):
                                vsz, v0, vt, vd = struct.unpack_from('<HBBI', b, q + esz)
                                rid = (pid << 24) | (tid << 16) | ei
                                if limit_types is None or types[tid - 1] in limit_types:
                                    val = values[vd] if vt == 3 else hex(vd)
                                    result.setdefault((rid, types[tid - 1], keys[key]), []).append((csize, vt, val))
                elif ct in (0x0001, 0x0203, 0x0204, 0x0205, 0x0206):
                    pass
                else:
                    raise Exception(hex(ct))
                p += csz
            assert p == end
            if verbose:
                print(f'package {pid:#x} {pname!r} types={types} keys={len(keys)} config sizes={cfg_sizes}')
        off += sz
    return result


if __name__ == '__main__':
    kind, path = sys.argv[1], sys.argv[2]
    b = open(path, 'rb').read()
    if kind == 'axml':
        print(axml(b))
    else:
        r = arsc(b, limit_types=sys.argv[3:] or None)
        for k, v in list(r.items())[:20]:
            print(f'{k[0]:#010x} {k[1]}/{k[2]} -> {v}')
