# SPDX-License-Identifier: BSD-3-Clause
import os
from pathlib import Path
import subprocess
import sys


image = Path(sys.argv[1]).resolve()
runner = Path(sys.argv[2]).resolve()
mount = image.parent / "mounted"
mount.mkdir()
try:
    subprocess.run(["mount", "-o", "loop", image, mount], check=True)
    try:
        for name in ("peer-a", "peer-b", "peer-c"):
            path = mount / name
            path.touch(exist_ok=False)
            os.setxattr(path, "user.large", b"A" * 700)
    finally:
        subprocess.run(["umount", mount], check=True)

    subprocess.run([runner, image, "attributes-shared"], check=True)
    subprocess.run(["e2fsck", "-fn", image], check=True)
    subprocess.run(["mount", "-o", "loop,ro", image, mount], check=True)
    try:
        assert os.getxattr(mount / "peer-a", "user.large") == b"B" * 700
        assert not (mount / "peer-b").exists()
        assert not (mount / "peer-c").exists()
    finally:
        subprocess.run(["umount", mount], check=True)
finally:
    mount.rmdir()
print("SHARED_ATTRIBUTES_OK")
