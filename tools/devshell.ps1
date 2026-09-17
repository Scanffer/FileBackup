# tools/devshell.ps1 - repo entry point: locate VS and enter the build environment
#
# Usage (MUST be dot-sourced, otherwise the env vars do not persist):
#     . ./tools/devshell.ps1
#
# Run this once per new terminal. MSVC's cl.exe is deliberately NOT on the
# system PATH -- Visual Studio keeps it out so that one machine can switch
# between the x86 / x64 / ARM toolchains. Forgetting this step is the #1 cause
# of "cl is not recognized" (see SETUP.md section 6).
#
# vswhere is used to locate VS rather than a hardcoded path: VS may live under
# "C:\Program Files (x86)\Microsoft Visual Studio\" or "C:\Program Files\...".
#
# ---------------------------------------------------------------------------
# NOTE: this file is intentionally ASCII-only.
#
# Windows PowerShell 5.1 reads .ps1 files as ANSI (GBK on a Simplified Chinese
# system) unless they carry a UTF-8 BOM. Non-ASCII text here would therefore be
# garbled -- and can even break parsing outright, because a mis-decoded
# multi-byte sequence may happen to decode to a quote or backtick character.
#
# The C++ sources do contain Chinese comments and are safe, because MSVC is
# explicitly passed /utf-8 (see the root CMakeLists.txt). PowerShell has no
# equivalent switch, so the same approach does not transfer here.
# ---------------------------------------------------------------------------

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere not found at: $vswhere -- install Visual Studio Build Tools (see SETUP.md section 3.2)."
    return
}

$install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath

if (-not $install) {
    Write-Error "No Visual Studio with the C++ workload was found. Open Visual Studio Installer, choose Modify, and check 'Desktop development with C++' (see SETUP.md section 3.2)."
    return
}

# vswhere lives in the Visual Studio Installer directory, which is NOT on PATH
# by default. Launch-VsDevShell.ps1 calls vswhere by its BARE NAME while
# initialising the dev shell, so without this the call fails and the script
# silently falls back to a degraded lookup: it prints
#     'vswhere.exe' is not recognized as an internal or external command
# and then reports the wrong VS version ("v18.0" instead of the real "v18.9.2").
# The environment still comes up, which is what makes this easy to miss -- but
# it is a real degradation, not cosmetic noise. Putting the directory on PATH
# fixes both symptoms.
$installerDir = Split-Path -Parent $vswhere
if ($env:PATH -notlike "*$installerDir*") {
    $env:PATH = "$installerDir;$env:PATH"
}

& "$install\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
