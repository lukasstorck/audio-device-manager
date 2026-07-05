$ErrorActionPreference = "Stop"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest `
    -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" && cmake -S . -B build_windows -G Ninja && cmake --build build_windows && ctest --test-dir build_windows --output-on-failure"
