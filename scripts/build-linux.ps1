# Builds the Linux installer as one static x86_64 binary in an Alpine
# container (musl), runs its tests, and copies it to
# dist\polarity-installer-linux. The same binary with symbols goes to
# dist\symbols\<commit>\ (for scripts\read-crash.ps1, never shipped).
# Needs Docker. Runs on Windows or Linux (the build workflow runs it on Linux).
#   scripts\build-linux.ps1
param([string]$Image = 'alpine:3.20')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = 'build/linux'
$commit = git -C $root rev-parse --short HEAD
if (git -C $root status --porcelain) { $commit += '-dirty' }
$steps = @(
  'apk add --no-cache build-base cmake linux-headers >/dev/null'
  "cmake -S . -B $build -DCMAKE_BUILD_TYPE=Release -DPOLARITY_INSTALLER_COMMIT=$commit"
  "cmake --build $build -j"
  "$build/installer_tests tests"
  "strip -o $build/polarity-installer.stripped $build/polarity-installer"
) -join ' && '
docker run --rm -v "${root}:/src" -w /src $Image sh -c $steps
if ($LASTEXITCODE -ne 0) { throw "the Linux installer build failed ($LASTEXITCODE)" }
$symbols = [IO.Path]::Combine($root, 'dist', 'symbols', $commit)
New-Item -ItemType Directory -Force $symbols | Out-Null
$out = [IO.Path]::Combine($root, 'dist', 'polarity-installer-linux')
Copy-Item ([IO.Path]::Combine($root, $build, 'polarity-installer.stripped')) $out -Force
Copy-Item ([IO.Path]::Combine($root, $build, 'polarity-installer')) (Join-Path $symbols 'polarity-installer-linux.debug') -Force
"$out ($([int]((Get-Item $out).Length / 1KB)) KB, commit $commit)"
