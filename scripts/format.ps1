# Formats the C++ sources under src/ and tests/ with clang-format and the
# repository's .clang-format. -Check formats nothing: it lists the files that
# are not formatted and exits 1 when there are any.
#   scripts/format.ps1 [-Check]
param([switch]$Check)
$root = Split-Path -Parent $PSScriptRoot
$cf = Get-Command clang-format -ErrorAction SilentlyContinue
$exe = if ($cf) { $cf.Source } else { "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\bin\clang-format.exe" }
if (-not (Test-Path $exe)) { Write-Host "format: clang-format not found (Build Tools' C++ Clang tools component)"; exit 2 }
$files = @(Get-ChildItem "$root\src", "$root\tests" -Recurse -File -Include *.cpp, *.h | ForEach-Object { $_.FullName })
# Batches keep each command line well under Windows' length limit.
$batches = [System.Collections.Generic.List[string[]]]::new()
for ($i = 0; $i -lt $files.Count; $i += 40) { $batches.Add($files[$i..([Math]::Min($i + 39, $files.Count - 1))]) }
if ($Check) {
    $bad = [System.Collections.Generic.SortedSet[string]]::new()
    foreach ($batch in $batches) {
        $out = & $exe --style=file --dry-run @batch 2>&1
        foreach ($line in $out) {
            if ("$line" -match '^(.+?):\d+:\d+: (warning|error):') { [void]$bad.Add($Matches[1].Substring($root.Length + 1)) }
        }
    }
    if ($bad.Count) {
        Write-Host "format: $($bad.Count) of $($files.Count) files not formatted:"
        $bad | ForEach-Object { Write-Host "  $_" }
        exit 1
    }
    Write-Host "format: $($files.Count) files ok"
} else {
    foreach ($batch in $batches) {
        & $exe --style=file -i @batch
        if ($LASTEXITCODE -ne 0) { Write-Host "format: clang-format failed"; exit 1 }
    }
    Write-Host "format: $($files.Count) files formatted"
}
