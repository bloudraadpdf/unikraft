# SPDX-License-Identifier: BSD-3-Clause
from pathlib import Path
import struct
import subprocess
import sys


image = Path(sys.argv[1])


def superblock():
    with image.open("rb") as disk:
        disk.seek(1024)
        return disk.read(1024)


before = superblock()
assert struct.unpack_from("<I", before, 0x60)[0] & 0x2000
subprocess.run(["tune2fs", "-U", "random", image], check=True)
after = superblock()
assert before[0x68:0x78] != after[0x68:0x78]
assert before[0x270:0x274] == after[0x270:0x274]
subprocess.run(["e2fsck", "-fn", image], check=True)
print("CHECKSUM_SEED_UUID_CHANGED")
