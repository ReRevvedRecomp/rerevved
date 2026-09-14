[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Capture,

    [string]$Output,

    [string]$Markers,

    [string]$RenderDocRoot,

    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 300
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

Add-Type @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
public static class RenderDocWindowCloser {
    [DllImport("user32.dll")]
    private static extern bool EnumThreadWindows(uint threadId, EnumWindowsProc callback, IntPtr data);
    [DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr handle, uint message, IntPtr wParam, IntPtr lParam);
    private delegate bool EnumWindowsProc(IntPtr handle, IntPtr data);
    public static int CloseForProcess(int processId) {
        int count = 0;
        Process process = Process.GetProcessById(processId);
        foreach (ProcessThread thread in process.Threads) {
            EnumThreadWindows((uint)thread.Id, (handle, data) => {
                PostMessage(handle, 0x0010, IntPtr.Zero, IntPtr.Zero);
                count++;
                return true;
            }, IntPtr.Zero);
        }
        return count;
    }
}
'@

if (-not $RenderDocRoot) {
    $RenderDocRoot = Join-Path $repo 'out\tools\renderdoc-1.46\portable\RenderDoc_1.46_64'
}
$RenderDocRoot = (Resolve-Path $RenderDocRoot).Path
$qrenderdoc = Join-Path $RenderDocRoot 'qrenderdoc.exe'
if (-not (Test-Path -LiteralPath $qrenderdoc -PathType Leaf)) {
    throw "qrenderdoc.exe not found: $qrenderdoc"
}

$capturePath = (Resolve-Path $Capture).Path
if (-not (Test-Path -LiteralPath $capturePath -PathType Leaf)) {
    throw "capture file not found: $capturePath"
}

$scriptPath = (Resolve-Path (Join-Path $PSScriptRoot 'inspect-renderdoc-frame.py')).Path
if (-not $Output) {
    $Output = Join-Path (Split-Path -Parent $capturePath) 'renderdoc-frame-report.json'
}
$outputPath = [IO.Path]::GetFullPath($Output)

if (Test-Path -LiteralPath $outputPath -PathType Leaf) {
    throw "report already exists; choose a fresh output path: $outputPath"
}

$outputDirectory = Split-Path -Parent $outputPath
$runRoot = Join-Path $outputDirectory ('renderdoc-launch-' + [Guid]::NewGuid().ToString('N'))
$appData = Join-Path $runRoot 'appdata'
$stdoutPath = Join-Path $runRoot 'qrenderdoc.stdout.log'
$stderrPath = Join-Path $runRoot 'qrenderdoc.stderr.log'
New-Item -ItemType Directory -Force -Path $appData | Out-Null

$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $qrenderdoc
$startInfo.Arguments = '--python "' + $scriptPath.Replace('"', '\"') + '"'
$startInfo.WorkingDirectory = $repo
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.EnvironmentVariables['APPDATA'] = $appData
$startInfo.EnvironmentVariables['RENDERDOC_CAPTURE_PATH'] = $capturePath
$startInfo.EnvironmentVariables['RENDERDOC_REPORT_PATH'] = $outputPath
if ($Markers) {
    $markerPath = (Resolve-Path $Markers).Path
    $startInfo.EnvironmentVariables['RENDERDOC_MARKERS_PATH'] = $markerPath
}

$process = New-Object System.Diagnostics.Process
$process.StartInfo = $startInfo
if (-not $process.Start()) {
    throw 'failed to start qrenderdoc.exe'
}

$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$finished = $process.WaitForExit($TimeoutSeconds * 1000)
if (-not $finished) {
    # qrenderdoc creates a hidden main window during --python. Closing that
    # window lets Qt unwind normally without killing the helper process.
    [RenderDocWindowCloser]::CloseForProcess($process.Id) | Out-Null
    $process.WaitForExit(5000) | Out-Null
    throw "qrenderdoc timed out after $TimeoutSeconds seconds; report path: $outputPath"
}

[IO.File]::WriteAllText($stdoutPath, $stdoutTask.Result)
[IO.File]::WriteAllText($stderrPath, $stderrTask.Result)

if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
    throw "qrenderdoc produced no report: $outputPath"
}

$report = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
if ($report.status -ne 'ok') {
    Write-Output (Get-Content -Raw -LiteralPath $outputPath)
    exit 1
}

Write-Output (Get-Content -Raw -LiteralPath $outputPath)
exit 0
