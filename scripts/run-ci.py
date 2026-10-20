"""Build, test and package the current platform on a prepared runner."""
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys

ROOT = Path(__file__).resolve().parent.parent
os.chdir(ROOT)
config = json.loads((ROOT / ".deps/environment.json").read_text())
obs, qt = Path(config["obs"]), Path(config["qt"])
windows = sys.platform == "win32"
build = ROOT / "build"
dist = ROOT / "dist"
if dist.is_symlink():
    raise SystemExit("The dist directory must not be a symbolic link.")
if dist.exists():
    for entry in dist.iterdir():
        if entry.is_dir() and not entry.is_symlink():
            shutil.rmtree(entry)
        else:
            entry.unlink()
arguments = ["cmake", "-S", str(ROOT), "-B", str(build), f"-DCMAKE_PREFIX_PATH={config['sdk']};{qt}", "-DPLAYLISTDOCK_BUILD_TESTS=ON"]
if windows:
    arguments += ["-G", "Visual Studio 17 2022", "-A", "x64"]
else:
    arguments += ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0"]
subprocess.run(arguments, check=True)
subprocess.run(["cmake", "--build", str(build), "--config", "Release", "--parallel", "3"], check=True)
env = os.environ.copy()
env["QT_QPA_PLATFORM"] = "minimal" if windows else "cocoa"
env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(qt / "plugins/platforms")
if windows:
    env["PATH"] = str(obs / "bin/64bit") + os.pathsep + str(qt / "bin") + os.pathsep + env["PATH"]
    env["PLAYLISTDOCK_OBS_DATA"] = str(obs / "data/libobs")
    executable = build / "Release/playlistdock-stress.exe"
    module, data = obs / "obs-plugins/64bit/vlc-video.dll", obs / "data/obs-plugins/vlc-video"
else:
    frameworks = obs / "Contents/Frameworks"
    env["DYLD_FRAMEWORK_PATH"] = str(frameworks) + ":" + str(qt / "lib")
    env["DYLD_LIBRARY_PATH"] = str(frameworks)
    env["PLAYLISTDOCK_GRAPHICS_MODULE"] = str(next(p for p in frameworks.rglob("libobs-opengl*") if p.is_file()))
    env["PLAYLISTDOCK_OBS_DATA"] = str(frameworks / "libobs.framework/Resources")
    executable = build / "playlistdock-stress"
    module = obs / "Contents/PlugIns/vlc-video.plugin/Contents/MacOS/vlc-video"
    data = obs / "Contents/PlugIns/vlc-video.plugin/Contents/Resources"
subprocess.run([str(executable), "--fake"], env=env, check=True)
import imageio_ffmpeg
ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
media = build / "media"
media.mkdir(exist_ok=True)
fixtures = [("01-MOV-H264.mov", ["-f", "lavfi", "-i", "color=c=blue:s=160x96:r=30", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac"]),
            ("02-MP3.mp3", ["-c:a", "libmp3lame"]),
            ("03-MOV-ProRes.mov", ["-f", "lavfi", "-i", "color=c=green:s=192x108:r=30", "-c:v", "prores_ks", "-pix_fmt", "yuv422p10le", "-c:a", "pcm_s16le"]),
            ("04-WAV.wav", ["-c:a", "pcm_s16le", "-ar", "48000"]),
            ("05-FLAC.flac", ["-c:a", "flac", "-ar", "44100"])]
files = []
for name, options in fixtures:
    path = media / name
    # Input options precede output codecs.
    video_input = options[:4] if name.endswith(".mov") else []
    codecs = options[4:] if video_input else options
    subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=12", *video_input, *codecs, "-t", "12", "-ac", "2", "-metadata", f"title={path.stem}", str(path)], check=True)
    files.append(str(path))
for mode in ("--end", "--continuity", "--sources", "--mixed"):
    subprocess.run([str(executable), mode, str(module), str(data), *files], env=env, check=True)
if windows:
    smoke_env = os.environ.copy()
    reveal_media = media / "定位 & file,#1.wav"
    shutil.copyfile(media / "04-WAV.wav", reveal_media)
    smoke_env["PLAYLISTDOCK_SMOKE_MEDIA"] = str(reveal_media)
    for locale in ("en-US", "zh-CN", "zh-TW"):
        smoke_env["PLAYLISTDOCK_SMOKE_OUTPUT"] = str(build / f"ui-{locale}")
        subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "scripts/run-smoke.ps1",
                        "-BuildDirectory", str(build), "-ObsRoot", str(obs), "-Destination", str(build / f"smoke-{locale}"),
                        "-Runs", "1", "-Locale", locale], env=smoke_env, check=True)
        subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "tests/check-reveal.ps1",
                        "-FilePath", str(reveal_media)], env=smoke_env, check=True)
if not windows:
    binary = str(build / "obs-playlistdock.plugin/Contents/MacOS/obs-playlistdock")
    assert subprocess.check_output(["lipo", "-archs", binary], text=True).strip() == "arm64"
subprocess.run([sys.executable, "scripts/package.py", "--build", str(build)], check=True)
subprocess.run([sys.executable, "tests/check-release.py"], check=True)
