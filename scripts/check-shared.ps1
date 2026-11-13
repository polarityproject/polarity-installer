# Compares this repo's copies of the client's update code with the client's
# own (polarity-client, the sfv-rollback folder beside this one by default).
# The release key must match: a mismatch exits 1 (the installer would refuse
# every release). The rest is reported for a look:
#   json.h/.cpp        should be identical
#   update_info.h/.cpp differ only by this copy's Linux (mbedTLS) parts
#   scripts\check-shared.ps1 [-Client <folder>]
param([string]$Client = (Join-Path $PSScriptRoot '..\..\sfv-rollback'))
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Client = (Resolve-Path $Client).Path

function KeyBytes([string]$path) {
  $text = Get-Content $path -Raw
  $block = $text.Substring($text.IndexOf('kUpdatePublicKey'))
  $block = $block.Substring(0, $block.IndexOf('};'))
  ([regex]::Matches($block, '0x([0-9a-fA-F]{2})') | ForEach-Object { $_.Groups[1].Value.ToLower() }) -join ''
}

$mine = KeyBytes (Join-Path $root 'src\mod\update_key.h')
$theirs = KeyBytes (Join-Path $Client 'src\mod\update_key.h')
if ($mine.Length -ne 128 -or $mine -ne $theirs) {
  "release key: DIFFERS from $Client\src\mod\update_key.h - copy the client's key array into src\mod\update_key.h"
  exit 1
}
'release key: matches the client'

foreach ($f in 'src\core\json.h', 'src\core\json.cpp') {
  $a = (Get-Content (Join-Path $root $f) -Raw) -replace "`r`n", "`n"
  $b = (Get-Content (Join-Path $Client $f) -Raw) -replace "`r`n", "`n"
  "${f}: $(if ($a -eq $b) { 'identical' } else { 'differs from the client' })"
}
foreach ($f in 'src\core\update_info.h', 'src\core\update_info.cpp') {
  "${f}: changes from the client's copy:"
  git -c core.autocrlf=false diff --no-index --stat -- (Join-Path $Client $f) (Join-Path $root $f)
}
