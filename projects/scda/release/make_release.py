"""Build the SCDA Recomp release folder and zip (docs Update 659).

    py -3 projects\\scda\\release\\make_release.py [--version 1.0]

Takes scda_recomp.exe and its DLLs from the canonical build
(build\\scda_upgrade\\bin\\RelWithDebInfo), writes scda_options.txt from the
template in projects\\scda\\src\\scda_options.c, adds README, licences and the
empty "gamefiles" and "user" folders, and zips everything to
release\\SCDA_Recomp_v<version>.zip. Never copies game files, saves or
xbox_keys.bin.
"""
import argparse
import os
import re
import shutil
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
BIN = os.path.join(ROOT, "build", "scda_upgrade", "bin", "RelWithDebInfo")
HERE = os.path.dirname(os.path.abspath(__file__))

DLLS = [
    "dsp56300.dll", "glslang.dll", "libSPIRV-Tools-opt.dll",
    "libSPIRV-Tools.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll",
    "libwinpthread-1.dll",
]

LICENSES = {
    "xboxrecomp_LICENSE.txt": "LICENSE",
    "xboxrecomp_NOTICE.txt": "NOTICE",
    "LGPL-2.1.txt": "LICENSES/LGPL-2.1.txt",
    "xemu_LICENSE.txt": "src/gpu/xemu/LICENSE",
    "xemu_COPYING_GPL-2.0.txt": "src/gpu/xemu/COPYING",
    "xemu_COPYING.LIB_LGPL-2.1.txt": "src/gpu/xemu/COPYING.LIB",
    "dsp56300_LICENSE.txt": "src/apu/dsp_jit/LICENSE.dsp56300",
    "dsp_jit_PROVENANCE.md": "src/apu/dsp_jit/PROVENANCE.md",
    "a52dec_COPYING_GPL-2.0.txt": "src/apu/third_party/a52dec/COPYING",
    "libsamplerate_COPYING.txt": "src/apu/third_party/libsamplerate/COPYING",
    "glslang_LICENSE.txt": "src/gpu/thirdparty/glslang/LICENSE.txt",
    "SPIRV-Reflect_LICENSE.txt": "src/gpu/thirdparty/SPIRV-Reflect/LICENSE",
    "volk_LICENSE.md": "src/gpu/thirdparty/volk/LICENSE.md",
    "Vulkan-Headers_LICENSE.txt": "src/gpu/thirdparty/Vulkan-Headers/LICENSE",
    "VulkanMemoryAllocator_LICENSE.txt":
        "src/gpu/thirdparty/VulkanMemoryAllocator/LICENSE.txt",
    # HLE renderer code taken from SCDA-XBERecomp (off by default, docs Update 686)
    "SCDA-XBERecomp_LICENSE_MIT.txt": "projects/scda/src/hle/tgp/LICENSE",
}

OTHER_LICENSES = """Further components without a licence file in this folder
==========================================================

- SPIRV-Tools (libSPIRV-Tools.dll, libSPIRV-Tools-opt.dll):
  Apache License 2.0, https://github.com/KhronosGroup/SPIRV-Tools
- GCC runtime (libgcc_s_seh-1.dll, libstdc++-6.dll): GPL v3 with the GCC
  Runtime Library Exception, https://gcc.gnu.org/onlinedocs/libstdc++/
- mingw-w64 winpthreads (libwinpthread-1.dll): MIT/BSD-style licence,
  https://www.mingw-w64.org/
"""

GAMEFILES_NOTE = """Put Splinter Cell: Double Agent (original Xbox, USA/Europe disc) here:

- the Xbox ISO file (.iso, XISO or full disc image) - it is unpacked once on
  the first start, or
- the unpacked game folder (the folder with default.xbe, Default, Media, adv).

See README.txt.
"""


def options_template():
    src = open(os.path.join(ROOT, "projects", "scda", "src", "scda_options.c"),
               encoding="utf-8").read()
    block = src.split("static const char s_template[] =", 1)[1].split(";\n", 1)[0]
    text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', block))
    return (text.replace("\\r", "\r").replace("\\n", "\n")
                .replace('\\"', '"').replace("\\\\", "\\"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.0")
    ap.add_argument("--replace", action="store_true",
                    help="rebuild a version this script made before")
    args = ap.parse_args()

    name = "SCDA_Recomp_v" + args.version
    out_root = os.path.join(ROOT, "release")
    out = os.path.join(out_root, name)
    if os.path.exists(out):
        # only ever the script's own output: never a folder someone filled
        if not args.replace or os.listdir(os.path.join(out, "user")) or \
                set(os.listdir(os.path.join(out, "gamefiles"))) != \
                {"put_the_game_here.txt"}:
            sys.exit("already exists (use --replace for an unused build): " + out)
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "licenses"))
    os.makedirs(os.path.join(out, "gamefiles"))
    os.makedirs(os.path.join(out, "user"))

    for f in ["scda_recomp.exe"] + DLLS:
        shutil.copy2(os.path.join(BIN, f), os.path.join(out, f))
    with open(os.path.join(out, "scda_options.txt"), "w", newline="") as f:
        f.write(options_template())
    shutil.copy2(os.path.join(HERE, "README.txt"), os.path.join(out, "README.txt"))
    shutil.copy2(os.path.join(HERE, "CHANGELOG.md"), os.path.join(out, "CHANGELOG.txt"))
    with open(os.path.join(out, "gamefiles", "put_the_game_here.txt"), "w",
              newline="\r\n") as f:
        f.write(GAMEFILES_NOTE)
    for dst, src in LICENSES.items():
        shutil.copy2(os.path.join(ROOT, src), os.path.join(out, "licenses", dst))
    with open(os.path.join(out, "licenses", "OTHER_COMPONENTS.txt"), "w",
              newline="\r\n") as f:
        f.write(OTHER_LICENSES)

    zip_path = os.path.join(out_root, name + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, dirs, files in os.walk(out):
            rel = os.path.relpath(base, out_root)
            if not files and not dirs:
                z.writestr(rel.replace(os.sep, "/") + "/", "")
            for f in files:
                p = os.path.join(base, f)
                z.write(p, os.path.relpath(p, out_root))
    print("folder:", out)
    print("zip:   ", zip_path, os.path.getsize(zip_path) // (1024 * 1024), "MB")


if __name__ == "__main__":
    main()
