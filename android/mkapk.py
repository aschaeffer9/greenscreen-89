#!/usr/bin/env python3
"""Build an APK without the Android SDK:
binary AndroidManifest.xml + resources.arsc (one icon) + native lib.
Signing (v1/JAR) is done afterwards with the JDK's jarsigner."""
import struct, sys, zipfile, io

ANDROID_NS = 'http://schemas.android.com/apk/res/android'

# verified against android-28 android.jar (android.R$attr / R$style)
ATTR_IDS = {
    'theme': 0x01010000, 'label': 0x01010001, 'icon': 0x01010002, 'name': 0x01010003,
    'hasCode': 0x0101000c, 'exported': 0x01010010, 'screenOrientation': 0x0101001e,
    'configChanges': 0x0101001f, 'value': 0x01010024, 'minSdkVersion': 0x0101020c,
    'versionCode': 0x0101021b, 'versionName': 0x0101021c, 'targetSdkVersion': 0x01010270,
    'allowBackup': 0x01010280, 'isGame': 0x010103f4, 'resizeableActivity': 0x010104f6,
    'extractNativeLibs': 0x010104ea, 'mimeType': 0x01010026, 'scheme': 0x01010027,
}
THEME_BLACK_NOTITLEBAR_FULLSCREEN = 0x0103000a

T_REF, T_STRING, T_INT_DEC, T_INT_HEX, T_BOOL = 0x01, 0x03, 0x10, 0x11, 0x12


def align4(b):
    return b + b'\0' * ((4 - len(b) % 4) % 4)


def string_pool(strings, utf8=False):
    offs, data = [], b''
    for s in strings:
        offs.append(len(data))
        if utf8:
            e = s.encode('utf-8')
            assert len(s) < 128 and len(e) < 128
            data += bytes([len(s), len(e)]) + e + b'\0'
        else:
            e = s.encode('utf-16-le')
            assert len(s) < 0x8000
            data += struct.pack('<H', len(s)) + e + b'\0\0'
    data = align4(data)
    hdr = 28
    strings_start = hdr + 4 * len(strings)
    size = strings_start + len(data)
    out = struct.pack('<HHIIIIII', 0x0001, hdr, size, len(strings), 0, 0x100 if utf8 else 0,
                      strings_start, 0)
    out += b''.join(struct.pack('<I', o) for o in offs) + data
    assert len(out) == size
    return out


# ---------------------------------------------------------------- AXML
class Elem:
    def __init__(self, tag, attrs, children=()):
        self.tag, self.attrs, self.children = tag, attrs, list(children)


def build_axml(root):
    # attrs: list of (ns_is_android, name, type, value)
    attr_names = []
    def walk(e):
        for a in e.attrs:
            if a[0] and a[1] not in attr_names:
                attr_names.append(a[1])
        for c in e.children:
            walk(c)
    walk(root)
    attr_names.sort(key=lambda n: ATTR_IDS[n])
    strings = list(attr_names)
    def sidx(s):
        if s not in strings:
            strings.append(s)
        return strings.index(s)
    sidx('android'); sidx(ANDROID_NS)
    def collect(e):
        sidx(e.tag)
        for ns, name, t, v in e.attrs:
            sidx(name)
            if t == T_STRING:
                sidx(v)
        for c in e.children:
            collect(c)
    collect(root)

    chunks = []
    resmap = struct.pack('<HHI', 0x0180, 8, 8 + 4 * len(attr_names)) + b''.join(
        struct.pack('<I', ATTR_IDS[n]) for n in attr_names)
    line = [1]
    def node_hdr(typ, size):
        line[0] += 1
        return struct.pack('<HHIII', typ, 16, size, line[0], 0xFFFFFFFF)
    chunks.append(node_hdr(0x0100, 24) + struct.pack('<II', sidx('android'), sidx(ANDROID_NS)))
    def emit(e):
        attrs = sorted(e.attrs, key=lambda a: (1, ATTR_IDS[a[1]]) if a[0] else (0, 0))
        body = struct.pack('<IIHHHHHH', 0xFFFFFFFF, sidx(e.tag), 20, 20, len(attrs), 0, 0, 0)
        for ns, name, t, v in attrs:
            nsi = sidx(ANDROID_NS) if ns else 0xFFFFFFFF
            if t == T_STRING:
                raw, data = sidx(v), sidx(v)
            else:
                raw, data = 0xFFFFFFFF, v & 0xFFFFFFFF
            body += struct.pack('<IIIHBBI', nsi, sidx(name), raw, 8, 0, t, data)
        chunks.append(node_hdr(0x0102, 16 + len(body)) + body)
        for c in e.children:
            emit(c)
        chunks.append(node_hdr(0x0103, 24) + struct.pack('<II', 0xFFFFFFFF, sidx(e.tag)))
    emit(root)
    chunks.append(node_hdr(0x0101, 24) + struct.pack('<II', sidx('android'), sidx(ANDROID_NS)))
    body = string_pool(strings) + resmap + b''.join(chunks)
    return struct.pack('<HHI', 0x0003, 8, 8 + len(body)) + body


# ---------------------------------------------------------------- ARSC
def build_arsc(package_name, icon_path):
    """one package (0x7f) with one type 'drawable' and one entry 'icon' -> icon_path.
    resource id = 0x7f010000"""
    values = string_pool([icon_path], utf8=True)
    type_strings = string_pool(['drawable'])
    key_strings = string_pool(['icon'])
    typespec = struct.pack('<HHIBBHI', 0x0202, 16, 16 + 4, 1, 0, 0, 1) + struct.pack('<I', 0)
    config = struct.pack('<I', 64) + b'\0' * 60
    hdr_size = 20 + len(config)
    entries_start = hdr_size + 4
    entry = struct.pack('<HHI', 8, 0, 0) + struct.pack('<HBBI', 8, 0, T_STRING, 0)
    tsize = entries_start + len(entry)
    typechunk = struct.pack('<HHIBBHII', 0x0201, hdr_size, tsize, 1, 0, 0, 1, entries_start) + config + \
        struct.pack('<I', 0) + entry
    assert len(typechunk) == tsize
    name = package_name.encode('utf-16-le')
    name = name + b'\0' * (256 - len(name))
    pkg_hdr = 288
    type_off = pkg_hdr
    key_off = type_off + len(type_strings)
    pkg_body = type_strings + key_strings + typespec + typechunk
    pkg = struct.pack('<HHII', 0x0200, pkg_hdr, pkg_hdr + len(pkg_body), 0x7f) + name + \
        struct.pack('<IIIII', type_off, 1, key_off, 1, 0) + pkg_body
    body = values + pkg
    return struct.pack('<HHII', 0x0002, 12, 12 + len(body), 1) + body


# its own app ID, so it installs alongside earlier personal builds
import os
PACKAGE = os.environ.get('PACKAGE', 'com.ari.greenscreen89')


def main():
    lib, icon_png, out = sys.argv[1], sys.argv[2], sys.argv[3]
    A = lambda n, t, v: (True, n, t, v)
    manifest = Elem('manifest', [
        (False, 'package', T_STRING, PACKAGE),
        A('versionCode', T_INT_DEC, 13), A('versionName', T_STRING, '3.2'),
    ], [
        Elem('uses-sdk', [A('minSdkVersion', T_INT_DEC, 28), A('targetSdkVersion', T_INT_DEC, 28)]),
        Elem('uses-permission', [A('name', T_STRING, 'android.permission.INTERNET')]),
        # only used to look for the player's own ROM in Downloads on first run
        Elem('uses-permission', [A('name', T_STRING, 'android.permission.READ_EXTERNAL_STORAGE')]),
        Elem('application', [
            A('label', T_STRING, "Greenscreen '89"), A('icon', T_REF, 0x7f010000),
            A('hasCode', T_BOOL, 0), A('allowBackup', T_BOOL, 0), A('isGame', T_BOOL, 0xFFFFFFFF),
            A('extractNativeLibs', T_BOOL, 0xFFFFFFFF),
        ], [
            Elem('activity', [
                A('name', T_STRING, 'android.app.NativeActivity'),
                A('label', T_STRING, "Greenscreen '89"),
                A('theme', T_REF, THEME_BLACK_NOTITLEBAR_FULLSCREEN),
                A('screenOrientation', T_INT_DEC, 2),           # "user"
                A('configChanges', T_INT_HEX, 0x40007FFF),
                A('exported', T_BOOL, 0xFFFFFFFF),
                A('resizeableActivity', T_BOOL, 0xFFFFFFFF),
            ], [
                Elem('meta-data', [A('name', T_STRING, 'android.app.lib_name'), A('value', T_STRING, 'greenscreen')]),
                Elem('intent-filter', [], [
                    Elem('action', [A('name', T_STRING, 'android.intent.action.MAIN')]),
                    Elem('category', [A('name', T_STRING, 'android.intent.category.LAUNCHER')]),
                ]),
                # "Open with Greenscreen '89" for the player's .gb / .zip ROM file
                Elem('intent-filter', [], [
                    Elem('action', [A('name', T_STRING, 'android.intent.action.VIEW')]),
                    Elem('category', [A('name', T_STRING, 'android.intent.category.DEFAULT')]),
                    Elem('data', [A('scheme', T_STRING, 'content')]),
                    Elem('data', [A('scheme', T_STRING, 'file')]),
                ] + [Elem('data', [A('mimeType', T_STRING, m)]) for m in
                     ('application/octet-stream', 'application/zip', 'application/x-zip-compressed', 'application/x-gameboy-rom')]),
                # Share sheet
                Elem('intent-filter', [], [
                    Elem('action', [A('name', T_STRING, 'android.intent.action.SEND')]),
                    Elem('category', [A('name', T_STRING, 'android.intent.category.DEFAULT')]),
                    Elem('data', [A('mimeType', T_STRING, 'application/*')]),
                ]),
            ]),
        ]),
    ])
    axml = build_axml(manifest)
    arsc = build_arsc(PACKAGE, 'res/drawable/icon.png')
    with zipfile.ZipFile(out, 'w') as z:
        def add(name, data, compress):
            zi = zipfile.ZipInfo(name, date_time=(2026, 10, 29, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, data)
        add('AndroidManifest.xml', axml, True)
        add('resources.arsc', arsc, False)
        add('res/drawable/icon.png', open(icon_png, 'rb').read(), False)
        add('lib/arm64-v8a/libgreenscreen.so', open(lib, 'rb').read(), True)
    open(out + '.manifest.bin', 'wb').write(axml)
    open(out + '.arsc.bin', 'wb').write(arsc)


if __name__ == '__main__':
    main()
