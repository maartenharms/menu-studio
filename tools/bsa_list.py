#!/usr/bin/env python3
"""List full folder\\file paths inside Skyrim BSA archives (v104 LE / v105 SSE).

Used to verify vanilla mesh paths before wiring them into a BackgroundPreset
(a bad path just logs "backdrop: demand failed" and wastes a field round).

    python tools/bsa_list.py <bsa> [filter_substring]
"""
import struct
import sys


def list_bsa(path, needle=None):
    needle = needle.lower() if needle else None
    with open(path, "rb") as f:
        data = f.read()
    (magic, version, folder_offset, flags, folder_count, file_count,
     tot_folder_nl, tot_file_nl, file_flags) = struct.unpack_from(
        "<4sIIIIIIII", data, 0)
    if magic != b"BSA\x00":
        raise SystemExit("not a BSA: %s" % path)
    frec = 24 if version >= 105 else 16          # 64-bit offsets in v105

    # folder record table → per-folder file counts
    counts = []
    off = folder_offset
    for _ in range(folder_count):
        _name_hash, count = struct.unpack_from("<QI", data, off)
        counts.append(count)
        off += frec

    # folder blocks follow the record table, contiguous: each is a bzstring
    # name (len byte includes the null) + count 16-byte file records.
    block = folder_offset + folder_count * frec
    folder_of_file = []
    for ci in range(folder_count):
        nlen = data[block]
        fname = data[block + 1:block + 1 + nlen - 1].decode("cp1252", "replace")
        block += 1 + nlen + counts[ci] * 16
        folder_of_file.extend([fname] * counts[ci])

    # the file-name block starts exactly where the folder blocks end (file
    # DATA follows it - so it is NOT at the archive tail).
    names = data[block:block + tot_file_nl].split(b"\x00")

    out = []
    for i, fp in enumerate(folder_of_file):
        fn = names[i].decode("cp1252", "replace") if i < len(names) else "?"
        full = (fp + "\\" + fn) if fp else fn
        if needle is None or needle in full.lower():
            out.append(full)
    return out


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    needle = sys.argv[2] if len(sys.argv) > 2 else None
    for p in list_bsa(sys.argv[1], needle):
        print(p)
