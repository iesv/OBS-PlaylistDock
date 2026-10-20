"""Package the native Release build with its installer."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--dist", type=Path, default=Path("dist"))
parser.add_argument("--cmake", default="cmake")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
version = re.search(r"project\(OBS-PlaylistDock VERSION ([\d.]+)", (root / "CMakeLists.txt").read_text()).group(1)
mac = sys.platform == "darwin"
system = "macos-arm64" if mac else "windows-x64"
args.dist.mkdir(parents=True, exist_ok=True)
output = args.dist / f"OBS-PlaylistDock-{version}-{system}.zip"
with tempfile.TemporaryDirectory(prefix="playlistdock-package-") as temporary:
    staging = Path(temporary)
    subprocess.run([args.cmake, "--install", str(args.build), "--config", "Release", "--prefix", str(staging)], check=True)
    extension = "sh" if mac else "bat"
    installer = staging / f"install_OBS-PlaylistDock.{extension}"
    shutil.copy2(root / "scripts" / installer.name, installer)
    if mac:
        installer.chmod(0o755)
        subprocess.run(["codesign", "--force", "--sign", "-", str(staging / "obs-playlistdock.plugin")], check=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(staging.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(staging).as_posix())
print(output.resolve())
