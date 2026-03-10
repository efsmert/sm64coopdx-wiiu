#!/usr/bin/env python3
import struct
import sys


def fail(msg: str) -> None:
    print(msg, file=sys.stderr)
    raise SystemExit(1)


def convert_sequences(src: bytes) -> bytes:
    if len(src) < 4:
        fail("sequences input too small")

    revision, seq_count = struct.unpack_from("<HH", src, 0)
    out = bytearray(src)
    struct.pack_into(">HH", out, 0, revision, seq_count)

    src_entry_size = 16  # little-endian host build layout (64-bit pointer + 32-bit len + pad)
    dst_entry_size = 8   # Wii U runtime layout (32-bit pointer + 32-bit len)

    for i in range(seq_count):
        src_off = 4 + i * src_entry_size
        if src_off + src_entry_size > len(src):
            fail(f"sequences table truncated at entry {i}")

        offset64 = struct.unpack_from("<Q", src, src_off)[0]
        length32 = struct.unpack_from("<I", src, src_off + 8)[0]

        if offset64 > 0xFFFFFFFF:
            fail(f"entry {i} offset exceeds 32-bit range: {offset64}")

        dst_off = 4 + i * dst_entry_size
        if dst_off + dst_entry_size > len(out):
            fail(f"output table truncated at entry {i}")

        struct.pack_into(">II", out, dst_off, int(offset64), int(length32))

    return bytes(out)


def convert_bank_sets(src: bytes) -> bytes:
    if len(src) % 2 != 0:
        fail("bank_sets input must be even-sized")

    out = bytearray(len(src))
    for i in range(0, len(src), 2):
        value = struct.unpack_from("<H", src, i)[0]
        struct.pack_into(">H", out, i, value)
    return bytes(out)


def main() -> None:
    if len(sys.argv) != 4:
        fail("usage: wiiu_convert_audio_tables.py <sequences|bank_sets> <input> <output>")

    mode, input_path, output_path = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(input_path, "rb") as f:
        src = f.read()

    if mode == "sequences":
        out = convert_sequences(src)
    elif mode == "bank_sets":
        out = convert_bank_sets(src)
    else:
        fail("mode must be 'sequences' or 'bank_sets'")

    with open(output_path, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
