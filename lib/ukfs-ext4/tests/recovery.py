# SPDX-License-Identifier: BSD-3-Clause
from pathlib import Path
import errno
import os
import struct
import subprocess
import sys


source = Path(sys.argv[1]).resolve()
runner = Path(sys.argv[2]).resolve()
linux = sys.argv[3:] == ["--linux"]


def linux_recover(image, committed):
    with image.open("rb") as disk:
        disk.seek(1024 + 24)
        block_size = 1024 << struct.unpack("<I", disk.read(4))[0]
    mount = image.parent / "recovery-mounted"
    mount.mkdir()
    try:
        subprocess.run(["mount", "-o", "loop", image, mount], check=True)
        try:
            path = mount / "extents"
            with path.open("rb") as file:
                file.seek(4096 * block_size)
                marker = file.read(4)
            if committed:
                assert marker == struct.pack("<I", 4096)
                assert os.getxattr(path, "user.recovery") == b"R" * 700
            else:
                assert marker in (b"", bytes(4))
                try:
                    os.getxattr(path, "user.recovery")
                except OSError as error:
                    assert error.errno == errno.ENODATA
                else:
                    raise AssertionError("uncommitted attribute was recovered")
        finally:
            subprocess.run(["umount", mount], check=True)
    finally:
        mount.rmdir()


for case in ("commit", "abort"):
    prefix = "linux-recovery-" if linux else "recovery-"
    image = source.parent / (prefix + case + ".ext4")
    assert not image.exists()
    subprocess.run(["cp", "--reflink=auto", source, image], check=True)
    result = subprocess.run([runner, image, "interrupt-" + case])
    assert result.returncode == 77, result.returncode
    if linux:
        linux_recover(image, case == "commit")
    else:
        subprocess.run([runner, image, "recover-" + case], check=True)
    result = subprocess.run(
        ["e2fsck", "-fn", image], check=True, text=True, capture_output=True
    )
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    assert "Fix?" not in result.stdout and "WARNING" not in result.stdout
print("JOURNAL_RECOVERY_OK")
