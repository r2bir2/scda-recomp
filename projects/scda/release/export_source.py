"""Copy the publishable SCDA Recomp source into a clean folder (docs Update 661).

    py -3 projects\\scda\\release\\export_source.py [dest]

Default dest: C:\\VScode\\SCDA_Recomp_source. Takes tracked and untracked
(not ignored) files from an allow-list of folders, drops everything that is
game data, user data, generated output or local tooling, adds the files the
GitHub repository needs (README.md, .gitignore), and finally scans the result:
no game/save/key files, no file over 50 MB, and the console key bytes from a
local xbox_keys.bin must not occur in any file. Exits non-zero on a finding.
The source repository is only read.
"""
import fnmatch
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
HERE = os.path.dirname(os.path.abspath(__file__))
DEST = sys.argv[1] if len(sys.argv) > 1 else r"C:\VScode\SCDA_Recomp_source"

TOP_FILES = {
    "CMakeLists.txt", "LICENSE", "NOTICE", "CONTRIBUTORS.md", "CONTRIBUTING.md",
    "PROJECT_STATUS.md",
}
TOP_DIRS = ("src/", "include/", "templates/", "tests/", "tools/", "projects/",
            "docs/", "LICENSES/")
# tools/debug holds 1700 one-off outputs; only its tracked scripts go along
TRACKED_ONLY = ("tools/debug/",)

DENY_PATTERNS = [
    "*.xbe", "*.iso", "*.xiso", "*.umd", "*.sav", "*.xbx", "*.dmp", "*.qcow2",
    "*.img", "*xbox_keys*", "*.exe", "*.pdb", "*.obj", "*.ilk", "*.log",
    "*.bmp", "*.mp4", "*.wav", "*.tsv", "*.ips",
    "*/.vs/*", "*/__pycache__/*", "*/output/*", "projects/*/saves/*",
    "projects/scda/diag_launchers/*", "docs/bug_screenshots/*",
    "projects/scda/scda_options_vorlage.txt",
]
# third-party runtime DLLs the build copies next to the EXE (licences in
# LICENSES/ and the release's licenses folder)
EXTRA = [
    ("src/gpu/thirdparty/glslang/bin/glslang.dll", None),
    ("src/gpu/thirdparty/glslang/bin/libSPIRV-Tools-opt.dll", None),
    ("src/gpu/thirdparty/glslang/bin/libSPIRV-Tools.dll", None),
    ("src/gpu/thirdparty/glslang/bin/libgcc_s_seh-1.dll", None),
    ("src/gpu/thirdparty/glslang/bin/libstdc++-6.dll", None),
    ("src/gpu/thirdparty/glslang/bin/libwinpthread-1.dll", None),
    ("build/scda_upgrade/bin/RelWithDebInfo/dsp56300.dll",
     "src/apu/dsp_jit/bin/dsp56300.dll"),
]
ALLOWED_DLLS = {os.path.normpath(d or s) for s, d in EXTRA}


def git_files(*args):
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z", *args],
                         capture_output=True, check=True).stdout
    return [f for f in out.decode("utf-8", "replace").split("\0") if f]


def wanted(path, tracked):
    if "/" not in path:
        return path in TOP_FILES
    if not path.startswith(TOP_DIRS):
        return False
    if path.startswith(TRACKED_ONLY) and not tracked:
        return False
    return not any(fnmatch.fnmatch(path, p) or fnmatch.fnmatch("/" + path, p)
                   for p in DENY_PATTERNS)


def main():
    if os.path.exists(DEST) and os.listdir(DEST):
        sys.exit("destination not empty: " + DEST)
    tracked = set(git_files())
    untracked = set(git_files("--others", "--exclude-standard"))
    files = sorted(f for f in tracked | untracked
                   if os.path.isfile(os.path.join(ROOT, f))
                   and wanted(f, f in tracked))
    for f in files:
        dst = os.path.join(DEST, f)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, f), dst)
    for src, dst in EXTRA:
        d = os.path.join(DEST, dst or src)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(os.path.join(ROOT, src), d)
    # the toolkit's own README stays, the repository gets the SCDA one
    shutil.copy2(os.path.join(ROOT, "README.md"),
                 os.path.join(DEST, "docs", "xboxrecomp_README.md"))
    shutil.copy2(os.path.join(HERE, "repo_README.md"), os.path.join(DEST, "README.md"))
    shutil.copy2(os.path.join(HERE, "repo_gitignore"), os.path.join(DEST, ".gitignore"))

    # ---- scan ----
    problems = []
    key = b""
    for kp in (os.path.join(ROOT, "build", "scda_upgrade", "bin",
                            "RelWithDebInfo", "xbox_keys.bin"),):
        if os.path.isfile(kp):
            key = open(kp, "rb").read()
    key_parts = [key[i:i + 16] for i in (0, 16)] if len(key) == 32 else []
    key_hex = [k.hex().encode() for k in key_parts] + \
              [k.hex().upper().encode() for k in key_parts]
    total = 0
    for base, _, names in os.walk(DEST):
        for n in names:
            p = os.path.join(base, n)
            rel = os.path.relpath(p, DEST).replace(os.sep, "/")
            size = os.path.getsize(p)
            total += size
            if size > 50 * 1024 * 1024:
                problems.append("too large: " + rel)
            if any(fnmatch.fnmatch(rel.lower(), pat) for pat in
                   ("*.xbe", "*.iso", "*.umd", "*.sav", "*xbox_keys*", "*.dmp")):
                problems.append("forbidden type: " + rel)
            if rel.lower().endswith(".dll") and \
                    os.path.normpath(rel) not in ALLOWED_DLLS:
                problems.append("unexpected dll: " + rel)
            if key_parts:
                data = open(p, "rb").read()
                if any(k in data for k in key_parts) or any(h in data for h in key_hex):
                    problems.append("console key bytes in: " + rel)
    print("files:", len(files) + len(EXTRA) + 3, " size: %.1f MB" % (total / 1048576))
    print("key check:", "done" if key_parts else "skipped (no xbox_keys.bin)")
    if problems:
        print("PROBLEMS:")
        for p in problems:
            print("  " + p)
        sys.exit(1)
    print("scan clean:", DEST)


if __name__ == "__main__":
    main()
