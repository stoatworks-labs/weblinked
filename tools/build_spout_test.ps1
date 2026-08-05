# Builds the Spout verification pair on Windows, without building WebLinked.
#
# WebLinked itself has never been built on Windows — CEF and everything above
# the outputs is untested there — so this compiles the real Spout backend and
# the two core files it needs, and nothing else. See the header of
# tools/spout_send_test.cpp for exactly what that does and does not prove.
#
# Targets x64 rather than the host architecture: real Spout applications
# (Resolume, TouchDesigner, OBS) are x64, so that is the configuration worth
# testing. On an ARM64 Windows machine both binaries then run emulated, which
# is a caveat for the record, not a problem for the test.
#
# Usage, from a plain PowerShell prompt:
#   .\build_spout_test.ps1 -Repo C:\wl
#
# Then, in two windows:
#   .\out\spout_send_test.exe --pattern alphabars
#   .\out\spout_probe.exe --list
#   .\out\spout_probe.exe --source WLTest --pattern alphabars

param(
    [string]$Repo = "$PSScriptRoot\..",
    [string]$Out  = "$PSScriptRoot\..\out",
    [string]$Arch = "x64"
)

$ErrorActionPreference = "Stop"

$vsRoot = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars"
}

New-Item -ItemType Directory -Force -Path $Out | Out-Null

$spout   = Join-Path $Repo "third_party\spout\Spout"
$src     = Join-Path $Repo "src"
$tools   = Join-Path $Repo "tools"

# The vendored Spout sources, plus the two core files VideoFrame needs. Nothing
# from src/engine, src/browser or src/control: this deliberately does not drag
# in CEF.
$spoutSources = (Get-ChildItem -Path $spout -Filter *.cpp | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
$coreSources  = @(
    (Join-Path $src "core\frame.cpp"),
    (Join-Path $src "core\video_format.cpp")
) | ForEach-Object { '"' + $_ + '"' }
$coreSources = $coreSources -join ' '

$backend = '"' + (Join-Path $src "outputs\shared_surface_win.cpp") + '"'
$sender  = '"' + (Join-Path $tools "spout_send_test.cpp") + '"'
$probe   = '"' + (Join-Path $tools "spout_probe.cpp") + '"'

# /w on the Spout translation units only would need two passes; since this
# harness is not the shipping build, everything is compiled at /W1 and the
# warnings that matter are the ones in our own file.
$common = "/nologo /EHsc /std:c++20 /O2 /W1 /D_CRT_SECURE_NO_WARNINGS " +
          "/I`"$src`" /I`"$spout`""
# d3d11 and dxgi are Spout's actual dependencies. The rest are the MSVC default
# standard libraries, which a plain `cl` invocation does *not* add but CMake's
# CMAKE_CXX_STANDARD_LIBRARIES does — SpoutUtils and SpoutDX call MessageBox,
# clipboard, GDI text-measurement and registry functions from their diagnostic
# helpers. Their absence here is an artefact of driving the compiler directly,
# not something the real build has to solve.
$libs   = "d3d11.lib dxgi.lib user32.lib gdi32.lib advapi32.lib shell32.lib " +
          "ole32.lib comdlg32.lib"

$build = @"
call "$vcvars" $Arch || exit /b 1
cd /d "$Out"
cl $common $backend $coreSources $spoutSources $sender /Fe:spout_send_test.exe /link $libs || exit /b 1
cl $common $probe $spoutSources /Fe:spout_probe.exe /link $libs || exit /b 1
echo BUILD_OK
"@

$batch = Join-Path $env:TEMP "wl_build_spout.bat"
Set-Content -Path $batch -Value $build -Encoding ASCII
& cmd.exe /c "`"$batch`""
if ($LASTEXITCODE -ne 0) { throw "build failed with $LASTEXITCODE" }
Write-Host "built into $Out"
