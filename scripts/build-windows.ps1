# Builds polarity-installer.exe with the MSVC x64 toolchain and Ninja, and
# copies it to dist\polarity-installer.exe. A copy with its .pdb goes to
# dist\symbols\<commit>\ (for scripts\read-crash.ps1, never shipped).
#   scripts\build-windows.ps1
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio (C++ build tools) not found' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ x64 build tools not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$build = Join-Path $root 'build\windows'
$commit = git -C $root rev-parse --short HEAD
if (git -C $root status --porcelain) { $commit += '-dirty' }
cmd /c "`"$vcvars`" >nul && cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPOLARITY_INSTALLER_COMMIT=$commit && cmake --build `"$build`""
if ($LASTEXITCODE -ne 0) { throw "the Windows installer build failed ($LASTEXITCODE)" }
$symbols = Join-Path $root "dist\symbols\$commit"
New-Item -ItemType Directory -Force $symbols | Out-Null
$out = Join-Path $root 'dist\polarity-installer.exe'
Copy-Item (Join-Path $build 'polarity-installer.exe') $out -Force
Copy-Item (Join-Path $build 'polarity-installer.exe'), (Join-Path $build 'polarity-installer.pdb') $symbols -Force
"$out ($([int]((Get-Item $out).Length / 1KB)) KB, commit $commit)"
