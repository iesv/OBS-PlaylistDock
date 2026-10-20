"""Prepare an isolated portable OBS for startup/shutdown smoke tests (Windows)."""
from pathlib import Path
import argparse
import json
import os
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--obs-root", required=True, type=Path)
parser.add_argument("--build", required=True, type=Path)
parser.add_argument("--out", required=True, type=Path)
parser.add_argument("--locale", default="en-US", choices=("en-US", "zh-CN", "zh-TW"))
args = parser.parse_args()
original = args.obs_root.resolve(strict=True)
portable = args.out.resolve()
if portable.exists() and not (portable / ".playlistdock-smoke").is_file():
    raise SystemExit("Refusing to write into an existing non-test directory.")
portable.mkdir(parents=True, exist_ok=True)
(portable / ".playlistdock-smoke").write_text("OBS-PlaylistDock isolated test\n", encoding="ascii")
modules = {"vlc-video.dll", "obs-ffmpeg.dll", "obs-outputs.dll", "obs-transitions.dll", "image-source.dll", "win-wasapi.dll", "obs-filters.dll", "rtmp-services.dll"}
for directory in ("bin", "data", "obs-plugins"):
    for source in (original / directory).rglob("*"):
        if not source.is_file() or (directory == "obs-plugins" and source.name not in modules):
            continue
        target = portable / source.relative_to(original)
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            try:
                os.link(source, target)
            except PermissionError:
                shutil.copy2(source, target)
for name in ("OBS-PlaylistDock", "playlistdock-smoke-exit"):
    target = portable / "obs-plugins" / "64bit" / f"{name}.dll"
    shutil.copy2(args.build / "Release" / f"{name}.dll", target)
    metadata = portable / "data" / "obs-plugins" / name
    metadata.mkdir(parents=True, exist_ok=True)
    if name == "OBS-PlaylistDock":
        shutil.copy2(args.build / "manifest.json", metadata / "manifest.json")
        shutil.copytree(Path(__file__).resolve().parent.parent / "data" / "locale", metadata / "locale", dirs_exist_ok=True)

config = portable / "config" / "obs-studio"
config.mkdir(parents=True, exist_ok=True)
(portable / "portable_mode.txt").write_text("", encoding="ascii")
(config / "global.ini").write_text(
    "[General]\nLastVersion=536870916\nEnableAutoUpdates=false\n\n"
    "[Basic]\nProfile=PlaylistDockTest\nProfileDir=PlaylistDockTest\n"
    "SceneCollection=PlaylistDockTest\nSceneCollectionFile=PlaylistDockTest\n", encoding="utf-8")
(config / "user.ini").write_text(f"[General]\nFirstRun=true\nLanguage={args.locale}\n\n[BasicWindow]\nPreviewEnabled=false\n", encoding="utf-8")
profile = config / "basic" / "profiles" / "PlaylistDockTest"
profile.mkdir(parents=True, exist_ok=True)
(profile / "basic.ini").write_text(
    "[General]\nName=PlaylistDockTest\n\n[Video]\nBaseCX=1280\nBaseCY=720\n"
    "OutputCX=1280\nOutputCY=720\nFPSType=0\nFPSCommon=30\n\n"
    "[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n", encoding="utf-8")
(config / "basic" / "scenes").mkdir(parents=True, exist_ok=True)

# Disable installed third-party modules only in this portable instance's plugin-manager config.
global_plugins = Path(os.environ["PROGRAMDATA"]) / "obs-studio" / "plugins"
disabled = []
for binary in global_plugins.rglob("*.dll"):
    name = binary.stem
    if name == "OBS-PlaylistDock":
        continue
    disabled.append({"display_name": name, "module_name": name, "id": name, "version": "",
                     "enabled": False, "sources": [], "outputs": [], "encoders": [], "services": []})
manager = config / "plugin_manager"
manager.mkdir(parents=True, exist_ok=True)
(manager / "modules.json").write_text(json.dumps(disabled), encoding="utf-8")
print(f"Portable smoke-test instance prepared at {portable}")
