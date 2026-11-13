# Builds the Linux installer as one static x86_64 binary in an Alpine
# container (musl), runs its tests, and copies it to
# dist\polarity-installer-linux. Needs Docker.
#   scripts\build-linux.ps1
param([string]$Image = 'alpine:3.20')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = 'build/linux'
$steps = @(
  'apk add --no-cache build-base cmake linux-headers >/dev/null'
  "cmake -S . -B $build -DCMAKE_BUILD_TYPE=Release"
  "cmake --build $build -j"
  "$build/installer_tests tests"
) -join ' && '
docker run --rm -v "${root}:/src" -w /src $Image sh -c $steps
if ($LASTEXITCODE -ne 0) { throw "the Linux installer build failed ($LASTEXITCODE)" }
New-Item -ItemType Directory -Force (Join-Path $root 'dist') | Out-Null
$out = Join-Path $root 'dist\polarity-installer-linux'
Copy-Item (Join-Path $root "$build\polarity-installer") $out -Force
"$out ($([int]((Get-Item $out).Length / 1KB)) KB)"
