"""Check translations, version metadata and release ZIP contents."""
import json
from pathlib import Path
import plistlib
import re
import struct
import sys
import tempfile
import subprocess
import zipfile

root = Path(__file__).resolve().parent.parent
version = re.search(r"project\(OBS-PlaylistDock VERSION ([\d.]+)", (root / "CMakeLists.txt").read_text()).group(1)
locales = {}
for path in sorted((root / "data/locale").glob("*.ini")):
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, value = line.split("=", 1)
        assert key not in values, (path, key)
        values[key] = json.loads(value)
        assert values[key], (path, key)
    locales[path.stem] = values
assert set(locales) == {"en-US", "zh-CN", "zh-TW"}
base = locales["en-US"]
for values in locales.values():
    assert values.keys() == base.keys()
    for key in base:
        assert sorted(re.findall(r"%\d+", base[key])) == sorted(re.findall(r"%\d+", values[key])), key
code = "\n".join(p.read_text(encoding="utf-8") for p in (root / "src").glob("*.cpp"))
keys = set(re.findall(r'(?:ui|obs_module_text)\("([^"\n]+)"\)', code))
assert keys <= base.keys(), keys - base.keys()
assert set(base) <= keys, set(base) - keys
assert not re.search(r"TODO|FIXME|XXX|修复|修復|legacy|obsolete", code, re.I)
bat = (root / "scripts/install_OBS-PlaylistDock.bat").read_bytes()
assert not bat.startswith(b"\xef\xbb\xbf") and b"\n" not in bat.replace(b"\r\n", b"")
assert not re.search(rb"backup|Move-Item|Rename-Item|\.bak", bat, re.I)
archives = sorted((root / "dist").glob("*.zip"))
assert archives, "No release ZIP"
for path in archives:
    assert f"-{version}-" in path.name
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None
        files = {info.filename: info for info in archive.infolist() if not info.is_dir()}
        assert all(not n.startswith("/") and ".." not in Path(n).parts for n in files)
        assert not any(Path(n).name.lower() in ("agent.md", "agents.md", ".gitattributes", ".gitignore") or n.endswith((".pdb", ".obj", ".exe", ".log", ".pyc", ".bak")) for n in files)
        mac = "-macos-" in path.name
        prefix = "obs-playlistdock.plugin/Contents/Resources" if mac else "obs-playlistdock/data"
        metadata = json.loads(archive.read(prefix + "/manifest.json"))
        assert metadata["version"] == version
        assert metadata["os_arch"] == ("macos-arm64" if mac else "windows-x64")
        for locale in locales:
            name = f"data/locale/{locale}.ini"
            assert archive.read(prefix + f"/locale/{locale}.ini") == (root / name).read_bytes()
        installer = "install_OBS-PlaylistDock." + ("sh" if mac else "bat")
        assert archive.read(installer) == (root / "scripts" / installer).read_bytes()
        expected = {"README.md", "LICENSE", installer, prefix + "/manifest.json"}
        expected.update(prefix + f"/locale/{locale}.ini" for locale in locales)
        if mac:
            expected.update({"obs-playlistdock.plugin/Contents/Info.plist", "obs-playlistdock.plugin/Contents/MacOS/obs-playlistdock", "obs-playlistdock.plugin/Contents/_CodeSignature/CodeResources"})
        else:
            expected.add("obs-playlistdock/bin/64bit/OBS-PlaylistDock.dll")
        assert set(files) == expected, (set(files) - expected, expected - set(files))
        if mac:
            assert plistlib.loads(archive.read("obs-playlistdock.plugin/Contents/Info.plist"))["CFBundleShortVersionString"] == version
            binary = archive.read("obs-playlistdock.plugin/Contents/MacOS/obs-playlistdock")
            magic, cpu = struct.unpack_from("<II", binary)
            assert magic == 0xFEEDFACF and cpu == 0x0100000C
            if sys.platform == "darwin":
                with tempfile.TemporaryDirectory(prefix="playlistdock-install-") as temporary:
                    extracted = Path(temporary) / "package"
                    archive.extractall(extracted)
                    bundle = extracted / "obs-playlistdock.plugin"
                    (bundle / "Contents/MacOS/obs-playlistdock").chmod(0o755)
                    subprocess.run(["xattr", "-w", "com.apple.quarantine", "0083;00000000;OBS-PlaylistDock;", str(bundle)], check=True)
                    import os
                    env = os.environ.copy()
                    env["HOME"] = str(Path(temporary) / "home")
                    subprocess.run(["bash", str(extracted / installer)], env=env, stdin=subprocess.DEVNULL, check=True)
                    installed = Path(env["HOME"]) / "Library/Application Support/obs-studio/plugins/obs-playlistdock.plugin"
                    assert "com.apple.quarantine" not in subprocess.check_output(["xattr", "-lr", str(installed)], text=True)
                    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(installed)], check=True)
        else:
            binary = archive.read("obs-playlistdock/bin/64bit/OBS-PlaylistDock.dll")
            pe = struct.unpack_from("<I", binary, 0x3c)[0]
            assert binary[pe:pe+4] == b"PE\0\0" and struct.unpack_from("<H", binary, pe+4)[0] == 0x8664
        print(f"RELEASE_ZIP {path.name} crc_runtime_only_version_installer_architecture=PASS")
print(f"RELEASE_CHECK version={version} locales=3 keys={len(base)} translation_coverage=PASS")
