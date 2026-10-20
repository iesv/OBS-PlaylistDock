"""Prepare import libraries from an installed OBS, using only Python and MSVC.

The source headers and installed binaries must both be OBS 32.0.4.
No OBS/Qt runtime files are copied or modified.
"""
from pathlib import Path
import argparse
import re
import struct
import subprocess


def exports(binary: Path) -> list[str]:
    data = binary.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", data, pe + 4)[0] != 0x8664:
        raise ValueError(f"Not a Windows x64 PE binary: {binary}")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    sections = [struct.unpack_from("<IIII", data, optional + optional_size + i * 40 + 8) for i in range(count)]

    def offset(rva: int) -> int:
        for virtual_size, virtual_address, raw_size, raw_offset in sections:
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                return raw_offset + rva - virtual_address
        raise ValueError(f"Invalid RVA {rva:x} in {binary}")

    export_rva = struct.unpack_from("<I", data, optional + 112)[0]
    directory = struct.unpack_from("<IIHHIIIIIII", data, offset(export_rva))
    names = offset(directory[9])
    result = []
    for index in range(directory[7]):
        name = offset(struct.unpack_from("<I", data, names + index * 4)[0])
        result.append(data[name:data.index(b"\0", name)].decode("ascii"))
    return sorted(result)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--obs-source", required=True, type=Path)
    parser.add_argument("--obs-bin", required=True, type=Path)
    parser.add_argument("--lib-tool", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    source = args.obs_source.resolve(strict=True)
    binaries = args.obs_bin.resolve(strict=True)
    output = args.out.resolve()
    config = (source / "libobs" / "obs-config.h").read_text(encoding="utf-8")
    for name, expected in (("MAJOR", 32), ("MINOR", 0), ("PATCH", 4)):
        match = re.search(rf"#define LIBOBS_API_{name}_VER\s+(\d+)", config)
        if not match or int(match.group(1)) != expected:
            raise ValueError("Source headers must be OBS 32.0.4")
    for folder in ("include", "lib", "cmake"):
        (output / folder).mkdir(parents=True, exist_ok=True)
    (output / "include" / "obsconfig.h").write_text(
        "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n", encoding="ascii")
    for name, target in (("obs", "libobs"), ("obs-frontend-api", "obs-frontend-api")):
        definition = output / "lib" / f"{name}.def"
        definition.write_text(f'LIBRARY "{name}.dll"\nEXPORTS\n' + "\n".join(exports(binaries / f"{name}.dll")) + "\n", encoding="ascii")
        library = definition.with_suffix(".lib")
        subprocess.run([str(args.lib_tool.resolve(strict=True)), "/nologo", "/machine:x64", f"/def:{definition}", f"/out:{library}"], check=True)
        includes = [source / "libobs", output / "include"]
        if name == "obs-frontend-api":
            includes.append(source / "frontend" / "api")
        cmake = (
            f"if(NOT TARGET OBS::{target})\n"
            f"  add_library(OBS::{target} SHARED IMPORTED GLOBAL)\n"
            f"  set_target_properties(OBS::{target} PROPERTIES\n"
            f'    IMPORTED_IMPLIB "{library.as_posix()}"\n'
            f'    IMPORTED_LOCATION "{(binaries / (name + ".dll")).as_posix()}"\n'
            f'    INTERFACE_INCLUDE_DIRECTORIES "{";".join(path.as_posix() for path in includes)}")\n'
            f"endif()\nset({target}_FOUND TRUE)\n")
        (output / "cmake" / f"{target}Config.cmake").write_text(cmake, encoding="utf-8")
    print(f"SDK prepared: {output / 'cmake'}")


if __name__ == "__main__":
    main()
