# Reads the crashes in a player's polarity-installer.log: each "stack:" line
# becomes functions with file and line, from the symbols the build scripts
# kept in dist\symbols\<commit>\ for that build.
#   scripts\read-crash.ps1 <polarity-installer.log>
# Windows stacks need Visual Studio's llvm-symbolizer; Linux stacks need
# addr2line in WSL, or Docker.
param([Parameter(Mandatory)][string]$Log)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Find-Symbolizer {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { return $null }
  foreach ($vs in & $vswhere -products * -property installationPath) {
    $found = Get-ChildItem (Join-Path $vs 'VC\Tools') -Recurse -Filter llvm-symbolizer.exe -ErrorAction SilentlyContinue |
      Where-Object FullName -like '*x64*' | Select-Object -First 1
    if ($found) { return $found.FullName }
  }
  return $null
}

function Resolve-Windows([string]$symbols, [string]$stack) {
  $exe = Join-Path $symbols 'polarity-installer.exe'
  if (-not $script:symbolizer) { $script:symbolizer = Find-Symbolizer }
  if (-not $script:symbolizer) { '    (llvm-symbolizer not found: install the Visual Studio C++ build tools)'; return }
  $bytes = [IO.File]::ReadAllBytes($exe)
  $pe = [BitConverter]::ToInt32($bytes, 0x3C)
  $base = [BitConverter]::ToUInt64($bytes, $pe + 48)
  foreach ($frame in $stack.Trim() -split '\s+') {
    if ($frame -match '^polarity-installer\.exe\+0x([0-9a-f]+)$') {
      $address = '0x{0:X}' -f ($base + [Convert]::ToUInt64($Matches[1], 16))
      & $script:symbolizer --obj=$exe --pretty-print --inlining $address 2>&1 | Where-Object { $_ } | ForEach-Object { "    $_" }
    }
    else { "    $frame" }
  }
}

function Resolve-Linux([string]$symbols, [string]$stack) {
  $debug = Join-Path $symbols 'polarity-installer-linux.debug'
  $addresses = ($stack.Trim() -split '\s+') -join ' '
  if (Get-Command wsl -ErrorAction SilentlyContinue) {
    $drive = $debug.Substring(0, 1).ToLower()
    $path = "/mnt/$drive" + ($debug.Substring(2) -replace '\\', '/')
    $out = wsl -e sh -c "addr2line -f -C -i -p -s -e '$path' $addresses" 2>&1
    if ($LASTEXITCODE -eq 0) { $out | ForEach-Object { "    $_" }; return }
  }
  $out = docker run --rm -v "${symbols}:/s" alpine:3.20 sh -c "apk add --no-cache binutils >/dev/null && addr2line -f -C -i -p -s -e /s/polarity-installer-linux.debug $addresses" 2>&1
  $out | ForEach-Object { "    $_" }
}

$commit = $null; $os = $null
foreach ($line in Get-Content -LiteralPath $Log) {
  if ($line -match '=== Polarity Installer (\S+) \(commit ([^)]+)\), (Windows|Linux)') {
    $commit = $Matches[2]; $os = $Matches[3]
    ''; $line
    continue
  }
  if ($line -match '^\s*stack:(.*)$') {
    $line
    $symbols = Join-Path $root "dist\symbols\$commit"
    if (-not $commit -or -not (Test-Path $symbols)) { "    (no symbols for commit $commit in dist\symbols)"; continue }
    if ($os -eq 'Windows') { Resolve-Windows $symbols $Matches[1] } else { Resolve-Linux $symbols $Matches[1] }
    continue
  }
  if ($line -match 'crash:|uncaught exception|std::terminate|unexpected error|stopped by signal|crashing on purpose') { $line }
}
