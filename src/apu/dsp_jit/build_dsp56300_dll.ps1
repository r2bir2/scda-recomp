# Wraps the prebuilt dsp56300 v1.0.0 static archive (Rust, x86_64-pc-windows-gnu,
# MIT; the release asset xemu's subprojects/dsp56300 fetches) into
# dsp56300.dll with MinGW GCC, exporting only the C API in dsp56300.def.
# MSVC cannot link the archive itself; dsp56300_dll.c loads the DLL at runtime.
#
#   powershell -ExecutionPolicy Bypass -File build_dsp56300_dll.ps1 `
#       -OutDir <dir> [-Sdk <dir with lib\libdsp56300_emu_ffi.a>]
param(
    [string]$Sdk = '',
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Gcc = 'C:\msys64\mingw64\bin\gcc.exe'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Sdk) {
    $Sdk = Join-Path $here '..\..\..\scratchpad\audio_surround_sources_20261005\xemu_surround\build_audio_test\subprojects\dsp56300\dsp56300-1.0.0-x86_64-pc-windows-gnu'
}
$lib = Join-Path $Sdk 'lib\libdsp56300_emu_ffi.a'
foreach ($p in @($Gcc, $lib)) {
    if (-not (Test-Path -LiteralPath $p)) { throw "Missing: $p" }
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$out = Join-Path $OutDir 'dsp56300.dll'
# GCC needs its own DLLs on PATH while it runs; the result links libgcc
# statically and needs no MinGW runtime DLL.
$env:PATH = (Split-Path -Parent $Gcc) + ';' + $env:PATH
# Pull exactly the exported API members out of the archive (-u per symbol);
# --whole-archive would also pull duplicate raw-dylib import stubs.
$def = Join-Path $here 'dsp56300.def'
$undef = @(Get-Content -LiteralPath $def | Where-Object { $_ -match '^\s+dsp56300_' } |
    ForEach-Object { '-Wl,-u,' + $_.Trim() })
& $Gcc -shared -static-libgcc -o $out $def @undef $lib `
    '-lws2_32' '-luserenv' '-lbcrypt' '-ladvapi32' '-lntdll' '-lkernel32'
if ($LASTEXITCODE -ne 0) { throw "gcc failed: $LASTEXITCODE" }
"output=$out"
"sha256=$((Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash)"
