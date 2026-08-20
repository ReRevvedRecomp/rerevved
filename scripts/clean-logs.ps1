<#
.SYNOPSIS
  Delete loose run, build, and crash logs.

.DESCRIPTION
  Deletes top-level out\*.log files, known out\*.txt dumps, and root *.log
  files. The search is non-recursive. Use -WhatIf for a dry run or -Days N to
  limit deletion to files older than N days.

.EXAMPLE
  scripts\clean-logs.ps1 -WhatIf
.EXAMPLE
  scripts\clean-logs.ps1 -Days 7
#>
[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'Low')]
param(
    [int]$Days = 0
)
$ErrorActionPreference = 'Stop'
$repo   = (Resolve-Path "$PSScriptRoot\..").Path
$outDir = Join-Path $repo 'out'
$cutoff = (Get-Date).AddDays(-$Days)

$knownTxt = @('build_diag.txt','build_gate.txt','dbg_stderr.txt','run_stderr.txt','run_stdout.txt')

$targets = @()
if (Test-Path $outDir) {
    $targets += Get-ChildItem -LiteralPath $outDir -File -Filter *.log
    $targets += Get-ChildItem -LiteralPath $outDir -File | Where-Object { $knownTxt -contains $_.Name }
}
$targets += Get-ChildItem -LiteralPath $repo -File -Filter *.log

$targets = $targets | Where-Object { $_.LastWriteTime -le $cutoff } | Sort-Object FullName -Unique

if (-not $targets) { Write-Host "no loose logs to clean." -ForegroundColor DarkGray; return }

$freed = 0
foreach ($f in $targets) {
    $kb = [Math]::Round($f.Length / 1KB)
    if ($PSCmdlet.ShouldProcess($f.FullName, "delete ($kb KB)")) {
        Remove-Item -LiteralPath $f.FullName -Force
        Write-Host ("deleted  {0,8} KB  {1}" -f $kb, ($f.FullName.Replace("$repo\", ''))) -ForegroundColor DarkGray
        $freed += $f.Length
    } else {
        Write-Host ("would delete {0,6} KB  {1}" -f $kb, ($f.FullName.Replace("$repo\", ''))) -ForegroundColor Yellow
    }
}
if ($freed) { Write-Host ("freed {0} KB across {1} file(s)." -f [Math]::Round($freed / 1KB), $targets.Count) -ForegroundColor Green }
