"""Patches the game's binary AndroidManifest.xml:

- adds <meta-data android:name="android.max_aspect" android:value="3.0"/> to
  <application>: apps targeting API < 26 are letterboxed to a 1.86 aspect
  ratio unless they declare it, and modern phones are 20:9 or wider;
- extends android:configChanges of the main activity to every change, so
  Android never recreates it (see ALL_CONFIG_CHANGES);
- raises targetSdkVersion to 24 so that Android 15+ installs it normally;
- adds <profileable android:shell="true"/> to <application>, so that `simpleperf record --app` can profile the
  game from adb (performance work; it gives no other access to the app).

usage: patch_manifest.py <in AndroidManifest.xml> <out AndroidManifest.xml> [ratio]
"""
import struct
import sys

RES_STRING_POOL = 0x0001
RES_XML_RESOURCE_MAP = 0x0180
RES_XML_START_ELEMENT = 0x0102
RES_XML_END_ELEMENT = 0x0103
UTF8_FLAG = 0x100
ANDROID_NS = "http://schemas.android.com/apk/res/android"
ATTR_NAME = 0x01010003
ATTR_VALUE = 0x01010024
ATTR_CONFIG_CHANGES = 0x0101001F
ATTR_TARGET_SDK = 0x01010270
ATTR_SHELL = 0x01010594
# Android 15+ refuses to install apps targeting API < 24 (except through
# `adb install --bypass-low-target-sdk-block`); the game targets 23.
MIN_TARGET_SDK = 24
MAIN_ACTIVITY = "com.ubisoft.assassin.pirates.SparkActivity"
# Every android:configChanges flag (mcc ... grammaticalGender, fontWeightAdjustment,
# fontScale). Recreating the activity deadlocks the game engine, while it copes
# with resizes and configuration callbacks (folding, display size, dark mode...).
ALL_CONFIG_CHANGES = 0x0000FFFF | 0x10000000 | 0x40000000
TYPE_STRING = 0x03
TYPE_FLOAT = 0x04
TYPE_INT_BOOLEAN = 0x12


def read_pool(data, off):
    _, hsize, size = struct.unpack_from("<HHI", data, off)
    count, styles, flags, s_start, st_start = struct.unpack_from("<5I", data, off + 8)
    if styles:
        raise SystemExit("string pools with styles are not supported")
    offsets = struct.unpack_from(f"<{count}I", data, off + hsize)
    utf8 = bool(flags & UTF8_FLAG)
    strings = []
    for o in offsets:
        p = off + s_start + o
        if utf8:
            n = data[p]
            p += 2 if n & 0x80 else 1
            n8 = data[p]
            if n8 & 0x80:
                n8 = ((n8 & 0x7F) << 8) | data[p + 1]
                p += 2
            else:
                p += 1
            strings.append(data[p:p + n8].decode("utf-8"))
        else:
            n = struct.unpack_from("<H", data, p)[0]
            p += 2
            if n & 0x8000:
                n = ((n & 0x7FFF) << 16) | struct.unpack_from("<H", data, p)[0]
                p += 2
            strings.append(data[p:p + 2 * n].decode("utf-16-le"))
    return strings, flags, size


def encode_pool(strings, flags):
    utf8 = bool(flags & UTF8_FLAG)
    blobs = []
    for s in strings:
        if utf8:
            b = s.encode("utf-8")
            n16 = len(s.encode("utf-16-le")) // 2

            def ln(n):
                return bytes([n]) if n < 0x80 else bytes([0x80 | (n >> 8), n & 0xFF])
            blobs.append(ln(n16) + ln(len(b)) + b + b"\x00")
        else:
            n = len(s.encode("utf-16-le")) // 2
            head = struct.pack("<H", n) if n < 0x8000 else struct.pack("<HH", 0x8000 | (n >> 16), n & 0xFFFF)
            blobs.append(head + s.encode("utf-16-le") + b"\x00\x00")
    offsets, pos = [], 0
    for b in blobs:
        offsets.append(pos)
        pos += len(b)
    body = b"".join(blobs)
    body += b"\x00" * (-len(body) % 4)
    header_size = 0x1C
    s_start = header_size + 4 * len(strings)
    size = s_start + len(body)
    return (struct.pack("<HHI", RES_STRING_POOL, header_size, size)
            + struct.pack("<5I", len(strings), 0, flags & ~1, s_start, 0)  # clear SORTED
            + struct.pack(f"<{len(strings)}I", *offsets) + body)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    ratio = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
    data = open(src, "rb").read()
    xml_type, xml_hsize, _ = struct.unpack_from("<HHI", data, 0)
    pool_off = xml_hsize
    if struct.unpack_from("<H", data, pool_off)[0] != RES_STRING_POOL:
        raise SystemExit("unexpected manifest layout")
    strings, flags, pool_size = read_pool(data, pool_off)
    rest = data[pool_off + pool_size:]

    resmap_ids = []
    if struct.unpack_from("<H", rest, 0)[0] == RES_XML_RESOURCE_MAP:
        _, _, rsize = struct.unpack_from("<HHI", rest, 0)
        resmap_ids = list(struct.unpack_from(f"<{(rsize - 8) // 4}I", rest, 8))

    def attr_name_index(res_id):
        return resmap_ids.index(res_id)

    if "android.max_aspect" in strings:
        print("manifest already declares android.max_aspect")
        open(dst, "wb").write(data)
        return
    ns = strings.index(ANDROID_NS)
    meta = strings.index("meta-data")
    app = strings.index("application")
    name_attr, value_attr = attr_name_index(ATTR_NAME), attr_name_index(ATTR_VALUE)
    strings.append("android.max_aspect")
    key = len(strings) - 1

    # <profileable android:shell="true"/>: the attribute name needs a resource id. The resource map covers string
    # indices [0, len(map)); extending it with 0 entries ("no id") up to the new string keeps every other name as is.
    strings.append("profileable")
    profileable = len(strings) - 1
    strings.append("shell")
    shell_attr = len(strings) - 1
    resmap_ids.extend([0] * (shell_attr - len(resmap_ids)))
    resmap_ids.append(ATTR_SHELL)

    def attribute(name, raw, dtype, value):
        return struct.pack("<3I", ns, name, raw) + struct.pack("<HBBI", 8, 0, dtype, value)

    attrs = (attribute(name_attr, key, TYPE_STRING, key)
             + attribute(value_attr, 0xFFFFFFFF, TYPE_FLOAT, struct.unpack("<I", struct.pack("<f", ratio))[0]))
    start = (struct.pack("<HHI", RES_XML_START_ELEMENT, 0x10, 0x10 + 0x14 + len(attrs))
             + struct.pack("<2I", 0, 0xFFFFFFFF)
             + struct.pack("<2I", 0xFFFFFFFF, meta) + struct.pack("<6H", 0x14, 0x14, 2, 0, 0, 0) + attrs)
    end = (struct.pack("<HHI", RES_XML_END_ELEMENT, 0x10, 0x18) + struct.pack("<2I", 0, 0xFFFFFFFF)
           + struct.pack("<2I", 0xFFFFFFFF, meta))

    prof_attrs = attribute(shell_attr, 0xFFFFFFFF, TYPE_INT_BOOLEAN, 0xFFFFFFFF)
    prof_start = (struct.pack("<HHI", RES_XML_START_ELEMENT, 0x10, 0x10 + 0x14 + len(prof_attrs))
                  + struct.pack("<2I", 0, 0xFFFFFFFF)
                  + struct.pack("<2I", 0xFFFFFFFF, profileable) + struct.pack("<6H", 0x14, 0x14, 1, 0, 0, 0) + prof_attrs)
    prof_end = (struct.pack("<HHI", RES_XML_END_ELEMENT, 0x10, 0x18) + struct.pack("<2I", 0, 0xFFFFFFFF)
                + struct.pack("<2I", 0xFFFFFFFF, profileable))

    activity = strings.index("activity")
    config_attr = attr_name_index(ATTR_CONFIG_CHANGES)
    main_activity = strings.index(MAIN_ACTIVITY)
    uses_sdk = strings.index("uses-sdk")
    target_attr = attr_name_index(ATTR_TARGET_SDK)

    # Insert right after the <application> start element, and let the main
    # activity handle every configuration change itself.
    out, off, inserted, patched_config = bytearray(), 0, False, False
    while off < len(rest):
        ctype, _, csize = struct.unpack_from("<HHI", rest, off)
        chunk = bytearray(rest[off:off + csize])
        if ctype == RES_XML_RESOURCE_MAP:
            off += csize
            continue
        if ctype == RES_XML_START_ELEMENT:
            name = struct.unpack_from("<I", chunk, 0x10 + 4)[0]
            if name == uses_sdk:
                astart, asize, acount = struct.unpack_from("<3H", chunk, 0x10 + 8)
                for a in (0x10 + astart + i * asize for i in range(acount)):
                    if struct.unpack_from("<I", chunk, a + 4)[0] == target_attr:
                        old = struct.unpack_from("<I", chunk, a + 16)[0]
                        if old < MIN_TARGET_SDK:
                            struct.pack_into("<I", chunk, a + 16, MIN_TARGET_SDK)
                            print(f"targetSdkVersion {old} -> {MIN_TARGET_SDK}")
            if name == activity:
                astart, asize, acount = struct.unpack_from("<3H", chunk, 0x10 + 8)
                attrs_at = [0x10 + astart + i * asize for i in range(acount)]
                names = {struct.unpack_from("<I", chunk, a + 4)[0]: a for a in attrs_at}
                a_name = names.get(name_attr)
                if a_name is not None and struct.unpack_from("<I", chunk, a_name + 8)[0] == main_activity:
                    a_cfg = names.get(config_attr)
                    if a_cfg is None:
                        raise SystemExit("main activity has no android:configChanges to extend")
                    old = struct.unpack_from("<I", chunk, a_cfg + 16)[0]
                    struct.pack_into("<I", chunk, a_cfg + 16, old | ALL_CONFIG_CHANGES)
                    patched_config = True
                    print(f"configChanges 0x{old:x} -> 0x{old | ALL_CONFIG_CHANGES:x}")
        out += chunk
        if not inserted and ctype == RES_XML_START_ELEMENT:
            name = struct.unpack_from("<I", rest, off + 0x10 + 4)[0]
            if name == app:
                out += start + end + prof_start + prof_end
                inserted = True
        off += csize
    if not inserted:
        raise SystemExit("<application> not found")
    if not patched_config:
        raise SystemExit(f"{MAIN_ACTIVITY} not found")

    pool = encode_pool(strings, flags)
    resmap = struct.pack("<HHI", RES_XML_RESOURCE_MAP, 8, 8 + 4 * len(resmap_ids)) + struct.pack(
        f"<{len(resmap_ids)}I", *resmap_ids)
    body = pool + resmap + bytes(out)
    open(dst, "wb").write(struct.pack("<HHI", xml_type, xml_hsize, xml_hsize + len(body)) + body)
    print(f"added android.max_aspect={ratio} and <profileable android:shell=\"true\"/>")


if __name__ == "__main__":
    main()
