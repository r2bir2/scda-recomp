# Splinter Cell: Double Agent recompilation

This is the game-specific host project. It contains no Xbox game files and
references regenerable C output under
`tools/recomp/output/scda_seeded18_trace/gen`.

## Generate C

After running the seeded disassembly command below:

```powershell
py -3 -m tools.recomp game_files\scda\default.xbe --all --split 1000 `
  --gen-dir tools\recomp\output\scda_seeded18_trace\gen `
  --output-dir tools\recomp\output\scda_seeded18_trace `
  --disasm-dir tools\disasm\output\scda_seeded18_xonline `
  --func-id-dir tools\func_id\output\scda_dynamic --verbose
```

The generated game-derived files are ignored by Git.

## Configure and build

Run from the repository root in PowerShell:

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S projects\scda -B build\scda -G 'Visual Studio 17 2022' -A x64
& $cmake --build build\scda --config Release --target scda_recomp --parallel
```

Expected executable:

`build/scda/bin/Release/scda_recomp.exe`

For a Visual Studio debugging session with PDB symbols, build and select
`RelWithDebInfo`:

```powershell
& $cmake --build build\scda --config RelWithDebInfo --target scda_recomp --parallel
```

Open `build/scda/scda_recomp.sln` in Visual Studio, select `RelWithDebInfo`,
then press F5. CMake marks `scda_recomp` as the startup project; if an already
open Visual Studio instance still shows `ALL_BUILD` in bold, right-click
`scda_recomp`, choose **Set as Startup Project**, and press F5 again. The
project supplies the repository root as working directory and enables
trace-level kernel logging.

## Runtime layout

- XBE: `game_files/scda/default.xbe`
- Game data: `game_files/scda`
- Entry point: `0x00054775`
- Save directory: `projects/scda/saves` (created later if needed)

When the executable is copied beside `default.xbe`, it automatically uses that
directory as the game root and keeps recomp-only persistent data in the
separate `recomp_saves` subdirectory.

## Diagnostics and result terminology

Every Windows launch writes `scda_runtime.log` and `xbox_kernel.log` beside the
executable. An unhandled Windows exception additionally writes a timestamped
`SCDA_crash_YYYY-MM-DD_HH-MM-SS.dmp` there. A normal game-requested shutdown
does not create a dump.

Status reports distinguish these independent outcomes:

1. build successful;
2. automated tests successful;
3. executable starts without an immediate crash;
4. visible menu or gameplay reached.

The fourth outcome has not been reached.

The current executable is an early bootstrap, not a playable port. Do not run
it without expecting crashes or hangs: thousands of call targets are still
stubbed and the generated code has many warnings about uninitialized local
frame-pointer state. See `docs/SCDA_WORKLOG.md` for the reproducible history.

## Game-specific function seeds

`function_seeds.json` records code entry points confirmed during controlled
runtime tests but missed by heuristic function detection. Pass it in addition
to the `func_id` output; the locally extended `--seed-functions` option accepts
repeated paths:

```powershell
py -3 -m tools.disasm game_files\scda\default.xbe `
  --analysis-json game_files\scda\scda_analysis.json `
  --text-only `
  --extra-sections XONLINE `
  --seed-functions tools\func_id\output\scda_dynamic\identified_functions.json `
  --seed-functions projects\scda\function_seeds.json `
  --exclude-seed-functions projects\scda\function_seed_excludes.json `
  -o tools\disasm\output\scda_seeded18_xonline --force -v
```

The current executable is still a diagnostic bootstrap. Seed 17 passes the
former `sub_00048B15` early-exit condition and reaches a later path check in
`sub_0005355F`; see `PROJECT_STATUS.md` for the exact current blocker.
