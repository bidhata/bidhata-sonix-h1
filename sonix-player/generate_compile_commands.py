#!/usr/bin/env python3
import json
import os
import glob
import shlex
import subprocess

# The host flags of the Makefile (CFLAGS, HOST_CFLAGS, GB_CFLAGS), without the
# ones that only write dependency files.
C_FLAGS = "-O3 -g -Wall -D_FILE_OFFSET_BITS=64 -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE=1 -DGB_CORE=1 -DHOST_BUILD=1"
GB_FLAGS = "-DGEARBOY_DISABLE_DISASSEMBLER -fno-exceptions -fno-rtti -Wno-attributes -Isrc/gb/core -Isrc/gb/miniz"

def tool_flags(command):
    # What sdl2-config and pkg-config answer, as the Makefile asks them. A
    # tool that is not installed adds nothing.
    try:
        out = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, universal_newlines=True)
    except OSError:
        return ""
    return " ".join(out.stdout.split()) if out.returncode == 0 else ""

def main():
    print("Scanning source files...")
    cwd = os.getcwd()
    db = []

    libs = " ".join(f for f in (
        tool_flags(["sdl2-config", "--cflags"]),
        tool_flags(["pkg-config", "--cflags", "freetype2"]),
        tool_flags(["pkg-config", "--cflags", "opusfile", "wavpack"]),
    ) if f)
    if not libs:
        libs = "-I/usr/include/SDL2"
    c_command = f"gcc {C_FLAGS} {libs}"
    cxx_command = f"g++ {C_FLAGS} {libs} {GB_FLAGS}"

    # The player's own sources, as the Makefile finds them: every .c under
    # src, and the C++ under src/gb
    c_files = sorted(glob.glob("src/**/*.c", recursive=True))
    cxx_files = sorted(glob.glob("src/gb/**/*.cpp", recursive=True))

    # Find all LVGL C source files recursively
    c_files.extend(sorted(glob.glob("lvgl/src/**/*.c", recursive=True)))

    for f in c_files:
        db.append({
            "directory": cwd,
            "command": f"{c_command} -c {shlex.quote(f)}",
            "file": f
        })
    for f in cxx_files:
        db.append({
            "directory": cwd,
            "command": f"{cxx_command} -c {shlex.quote(f)}",
            "file": f
        })

    with open("compile_commands.json", "w") as out:
        json.dump(db, out, indent=2)
    print(f"Generated compile_commands.json with {len(db)} entries successfully.")

if __name__ == "__main__":
    main()
