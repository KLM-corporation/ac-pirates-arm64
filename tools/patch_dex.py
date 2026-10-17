"""Fixes a bug in the game's Java code before repacking.

org.ubisoft.geea.spark2.AlarmService.SafeParseInt(String) calls itself instead
of Integer.parseInt(String), so it always ends in a StackOverflowError that
kills the process once the game has scheduled notification alarms. The patch
rewrites `invoke-direct {this, s}, SafeParseInt` into
`invoke-static {s}, Integer.parseInt` (same instruction length).

usage: patch_dex.py <in.dex> <out.dex>   (copies unchanged if the bug is absent)
"""
import hashlib
import struct
import sys
import zlib


def uleb128(data, off):
    result = shift = 0
    while True:
        b = data[off]
        off += 1
        result |= (b & 0x7F) << shift
        if b < 0x80:
            return result, off
        shift += 7


class Dex:
    def __init__(self, data):
        self.d = data
        (self.string_ids_size, self.string_ids_off, self.type_ids_size, self.type_ids_off,
         self.proto_ids_size, self.proto_ids_off, _, _, self.method_ids_size, self.method_ids_off,
         self.class_defs_size, self.class_defs_off) = struct.unpack_from("<12I", data, 0x38)

    def string(self, idx):
        off = struct.unpack_from("<I", self.d, self.string_ids_off + 4 * idx)[0]
        _, off = uleb128(self.d, off)
        end = self.d.index(0, off)
        return self.d[off:end].decode("utf-8", "replace")

    def type_name(self, idx):
        return self.string(struct.unpack_from("<I", self.d, self.type_ids_off + 4 * idx)[0])

    def proto(self, idx):
        _, ret, params_off = struct.unpack_from("<3I", self.d, self.proto_ids_off + 12 * idx)
        params = []
        if params_off:
            n = struct.unpack_from("<I", self.d, params_off)[0]
            params = [self.type_name(struct.unpack_from("<H", self.d, params_off + 4 + 2 * i)[0]) for i in range(n)]
        return "(" + "".join(params) + ")" + self.type_name(ret)

    def method(self, idx):
        cls, proto, name = struct.unpack_from("<HHI", self.d, self.method_ids_off + 8 * idx)
        return self.type_name(cls), self.string(name), self.proto(proto)

    def find_method(self, cls, name, sig):
        for i in range(self.method_ids_size):
            if self.method(i) == (cls, name, sig):
                return i
        return None

    def code_of(self, method_idx):
        """Returns the code_item offset of a method defined in this dex."""
        for c in range(self.class_defs_size):
            class_data_off = struct.unpack_from("<I", self.d, self.class_defs_off + 32 * c + 24)[0]
            if not class_data_off:
                continue
            off = class_data_off
            sf, off = uleb128(self.d, off)
            inf, off = uleb128(self.d, off)
            dm, off = uleb128(self.d, off)
            vm, off = uleb128(self.d, off)
            for _ in range(sf + inf):
                _, off = uleb128(self.d, off)
                _, off = uleb128(self.d, off)
            idx = 0
            for k in range(dm + vm):
                if k == dm:
                    idx = 0
                diff, off = uleb128(self.d, off)
                idx += diff
                _, off = uleb128(self.d, off)
                code_off, off = uleb128(self.d, off)
                if idx == method_idx:
                    return code_off
        return None


def fix_header(data):
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into("<I", data, 8, zlib.adler32(bytes(data[12:])) & 0xFFFFFFFF)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, "rb").read())
    dex = Dex(data)
    target = dex.find_method("Lorg/ubisoft/geea/spark2/AlarmService;", "SafeParseInt", "(Ljava/lang/String;)I")
    parse = dex.find_method("Ljava/lang/Integer;", "parseInt", "(Ljava/lang/String;)I")
    patched = False
    if target is not None:
        code = dex.code_of(target)
        if code:
            insns = code + 16
            want = bytes([0x70, 0x20]) + struct.pack("<H", target) + bytes([0x32, 0x00])
            # Bytecode: const/4 v1, 0 ; invoke-direct {v2, v3}, SafeParseInt
            if data[insns + 2:insns + 8] == want:
                if parse is None:
                    sys.exit("Integer.parseInt(String) is not referenced by this dex; cannot patch")
                data[insns + 2:insns + 8] = bytes([0x71, 0x10]) + struct.pack("<H", parse) + bytes([0x03, 0x00])
                fix_header(data)
                patched = True
    open(dst, "wb").write(data)
    print(f"{src}: {'patched AlarmService.SafeParseInt' if patched else 'unchanged'}")


if __name__ == "__main__":
    main()
