<#
.SYNOPSIS
  Safe one-command driver for the ReRevved ReXGlue runtime path.

.DESCRIPTION
  Initializes a child cmd.exe with Visual Studio and LLVM, then runs one explicit
  ReXGlue stage. The sibling SDK is consumed only from its installed package; this
  script never configures, builds, or writes to ../rerevved-rexglue-sdk.

.PARAMETER Stage
  Configure, Codegen, Build, Launch, or All. All runs configure, codegen,
  configure again to load generated sources, build, and a bounded launch check.

.PARAMETER SelfTest
  Resolve and validate all required paths and print the constructed commands, but
  do not run CMake or launch the game.

.PARAMETER Interactive
  For Launch or All, wait for the game to exit normally without a probe timeout.

.PARAMETER LaunchArgument
  Additional single-token -- arguments appended to the game command.

.EXAMPLE
  cd <repo>; .\scripts\rexglue.ps1 -Stage All

.EXAMPLE
  cd <repo>; .\scripts\rexglue.ps1 -Stage Launch -ProbeSeconds 30

.EXAMPLE
  cd <repo>; .\scripts\rexglue.ps1 -Stage Launch -Interactive

#>
[CmdletBinding()]
param(
    [ValidateSet('Configure', 'Codegen', 'Build', 'Launch', 'All')]
    [string]$Stage = 'All',
    [ValidateRange(1, 3600)]
    [int]$ProbeSeconds = 20,
    [switch]$Interactive,
    [string[]]$LaunchArgument = @(),
    [switch]$SelfTest,
    [string]$VcVarsAll,
    [string]$LlvmBin = 'C:\Program Files\LLVM\bin'
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$sdkRepo = Join-Path $repo '..\rerevved-rexglue-sdk'
$sdkInstall = Join-Path $sdkRepo 'out\install\win-amd64'
$sdkLock = Join-Path $repo 'rexglue-sdk.lock.json'
$manifest = Join-Path $repo 'rerevved_manifest.toml'
$xex = Join-Path $repo 'game\default.xex'
$buildDir = Join-Path $repo 'out\build\win-amd64-release'
$exe = Join-Path $buildDir 'rerevved.exe'
$userData = Join-Path $repo 'out\rexglue-user'
$cache = Join-Path $repo 'out\rexglue-cache'
$log = Join-Path $repo 'out\rexglue_boot.log'
$batch = Join-Path $repo 'out\_rexglue_spike.bat'

# Visual Studio discovery: prefer the standard VS 2022 Build Tools location,
# then use vswhere to locate another installed VS 2022 instance.
if (-not $VcVarsAll) {
    $default = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
    if (Test-Path -LiteralPath $default) {
        $VcVarsAll = $default
    } else {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path -LiteralPath $vswhere) {
            $vsRoot = (& $vswhere -latest -products * -property installationPath) | Select-Object -First 1
            if ($vsRoot) {
                $VcVarsAll = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvarsall.bat'
            }
        }
    }
}

function Require-Path([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description not found: $Path"
    }
}

function Quote-Cmd([string]$Value) {
    return '"' + $Value.Replace('"', '""') + '"'
}

function Normalize-GitRemote([string]$Value) {
    $normalized = $Value.Trim()
    if ($normalized -match '^git@github\.com:(.+)$') {
        $normalized = 'https://github.com/' + $Matches[1]
    }
    return $normalized.TrimEnd('/').Replace('.git', '').ToLowerInvariant()
}

foreach ($argument in $LaunchArgument) {
    if ([string]::IsNullOrWhiteSpace($argument) -or
        -not $argument.StartsWith('--', [System.StringComparison]::Ordinal) -or
        $argument -match '[\s"]') {
        throw "LaunchArgument must be one -- token without whitespace or quotes: $argument"
    }
}

function Write-LogSummary {
    if (-not (Test-Path -LiteralPath $log)) {
        Write-Host "log      : not created ($log)" -ForegroundColor DarkYellow
        return
    }

    # A bounded tail avoids loading a potentially large, long-lived probe log.
    $tail = Get-Content -LiteralPath $log -Tail 512
    $fatalOrError = $tail | Where-Object { $_ -match '(?i)\b(fatal|error)\b' } | Select-Object -Last 1
    $unregistered = $tail | Where-Object { $_ -match '(?i)unregistered.*(guest|function|address)' } | Select-Object -Last 1
    if ($fatalOrError) { Write-Host "latest log fatal/error: $fatalOrError" -ForegroundColor Red }
    if ($unregistered) { Write-Host "latest unregistered : $unregistered" -ForegroundColor Yellow }
}

function Invoke-CmakeStages([string[]]$Commands) {
    $lines = @(
        '@echo off',
        "call $(Quote-Cmd $VcVarsAll) x64 || exit /b 1",
        "set `"PATH=$LlvmBin;%PATH%`"",
        "pushd $(Quote-Cmd $repo) || exit /b 1"
    )
    foreach ($command in $Commands) {
        $lines += "$command || exit /b 1"
    }
    $lines += 'popd'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $batch) | Out-Null
    Set-Content -LiteralPath $batch -Value $lines -Encoding ascii
    & cmd.exe /d /c "`"$batch`""
}

Require-Path $sdkRepo 'Sibling ReXGlue SDK checkout'
Require-Path (Join-Path $sdkRepo '.git') 'Sibling ReXGlue SDK Git metadata'
Require-Path $sdkLock 'ReXGlue SDK lock'
Require-Path $sdkInstall 'Installed sibling ReXGlue SDK directory'
$sdkInstall = (Resolve-Path -LiteralPath $sdkInstall).Path
$sdkConfig = Join-Path $sdkInstall 'lib\cmake\rexglue'
Require-Path $sdkConfig 'Installed sibling ReXGlue SDK CMake package'
$sdkConfigFile = Join-Path $sdkConfig 'rexglueConfig.cmake'
Require-Path $sdkConfigFile 'Installed sibling ReXGlue SDK config'

$lock = Get-Content -Raw -LiteralPath $sdkLock | ConvertFrom-Json
foreach ($property in @('repository', 'commit', 'version')) {
    if ([string]::IsNullOrWhiteSpace($lock.$property)) {
        throw "ReXGlue SDK lock is missing '$property': $sdkLock"
    }
}

$sdkHead = (& git -C $sdkRepo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not resolve sibling ReXGlue SDK HEAD.' }
if ($sdkHead -ne $lock.commit) {
    throw "Sibling ReXGlue SDK commit mismatch: expected $($lock.commit), found $sdkHead"
}

$sdkOrigin = (& git -C $sdkRepo config --get remote.origin.url).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($sdkOrigin)) {
    throw 'Could not resolve sibling ReXGlue SDK origin URL.'
}
if ((Normalize-GitRemote $sdkOrigin) -ne (Normalize-GitRemote $lock.repository)) {
    throw "Sibling ReXGlue SDK origin mismatch: expected $($lock.repository), found $sdkOrigin"
}

$sdkConfigText = Get-Content -Raw -LiteralPath $sdkConfigFile
if ($sdkConfigText -notmatch 'set\(REXGLUE_VERSION_STRING "([^"]+)"\)') {
    throw "Installed ReXGlue SDK version not found: $sdkConfigFile"
}
$sdkVersion = $Matches[1]
if ($sdkVersion -ne $lock.version) {
    throw "Installed ReXGlue SDK version mismatch: expected $($lock.version), found $sdkVersion"
}

Require-Path $manifest 'ReXGlue manifest'
Require-Path $xex 'Game default.xex'
Require-Path $VcVarsAll 'vcvarsall.bat'
Require-Path (Join-Path $LlvmBin 'clang.exe') 'clang.exe'
Require-Path (Join-Path $LlvmBin 'clang++.exe') 'clang++.exe'

$configureCommand = 'cmake --preset win-amd64-release "-Drexglue_DIR=' + $sdkConfig + '"'
$codegenCommand = 'cmake --build --preset win-amd64-release --target rerevved_codegen'
$buildCommand = 'cmake --build --preset win-amd64-release'
$launchArguments = @(
    '--gpu_plugin=xenos',
    '--render_target_path_d3d12=rov',
    ('--game_data_root=' + (Quote-Cmd (Join-Path $repo 'game'))),
    ('--user_data_root=' + (Quote-Cmd $userData)),
    ('--cache_root=' + (Quote-Cmd $cache)),
    ('--log_file=' + (Quote-Cmd $log)),
    '--log_level=debug'
)
$launchArguments += $LaunchArgument
$arguments = $launchArguments -join ' '

Write-Host "repo     : $repo" -ForegroundColor DarkGray
Write-Host "sdk      : $sdkInstall" -ForegroundColor DarkGray
Write-Host "sdk ref  : $sdkHead ($sdkVersion)" -ForegroundColor DarkGray
Write-Host "vcvarsall: $VcVarsAll" -ForegroundColor DarkGray
Write-Host "llvm bin : $LlvmBin" -ForegroundColor DarkGray
Write-Host "stage    : $Stage" -ForegroundColor Cyan

if ($SelfTest) {
    Write-Host 'self-test: resolved paths and constructed commands only; no CMake or game launch.' -ForegroundColor Cyan
    Write-Host "  $configureCommand"
    Write-Host "  $codegenCommand"
    Write-Host "  $buildCommand"
    Write-Host ('  ' + (Quote-Cmd $exe) + ' ' + $arguments)
    exit 0
}

$cmakeCommands = @()
switch ($Stage) {
    'Configure' { $cmakeCommands = @($configureCommand) }
    'Codegen' { $cmakeCommands = @($codegenCommand) }
    'Build' { $cmakeCommands = @($buildCommand) }
    'All' { $cmakeCommands = @($configureCommand, $codegenCommand, $configureCommand, $buildCommand) }
}
if ($cmakeCommands.Count -gt 0) {
    Invoke-CmakeStages $cmakeCommands
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        Write-Host "CMake stage failed ($code)." -ForegroundColor Red
        exit $code
    }
}

if ($Stage -notin @('Launch', 'All')) {
    Write-Host 'Stage completed.' -ForegroundColor Green
    exit 0
}

Require-Path $exe 'Candidate executable'
if (Get-Process -Name 'rerevved' -ErrorAction SilentlyContinue) {
    throw 'Refusing to launch: rerevved is already running.'
}

New-Item -ItemType Directory -Force -Path $userData, $cache | Out-Null

if ($Interactive) {
    Write-Host "launch   : $exe (interactive; no timeout)" -ForegroundColor Cyan
} else {
    Write-Host "launch   : $exe (probe timeout: $ProbeSeconds seconds)" -ForegroundColor Cyan
}
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $exe
$startInfo.Arguments = $arguments
$startInfo.WorkingDirectory = $repo
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$process = [System.Diagnostics.Process]::Start($startInfo)
if (-not $process) { throw 'Failed to start the candidate executable.' }

if ($Interactive) {
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    if ($exitCode -eq 0) {
        Write-Host "interactive result: normal exit (0)" -ForegroundColor Green
    } else {
        Write-Host "interactive result: crash or error exit ($exitCode)" -ForegroundColor Red
    }
    Write-LogSummary
    exit $exitCode
}

if ($process.WaitForExit($ProbeSeconds * 1000)) {
    $exitCode = $process.ExitCode
    if ($exitCode -eq 0) {
        Write-Host "probe result: normal exit (0)" -ForegroundColor Green
    } else {
        Write-Host "probe result: crash or error exit ($exitCode)" -ForegroundColor Red
    }
    Write-LogSummary
    exit $exitCode
}

Write-Host "probe result: timeout after $ProbeSeconds seconds; stopping launched PID $($process.Id)." -ForegroundColor Yellow
$process.Kill()
if (-not $process.WaitForExit(5000)) {
    Write-Host "Could not confirm exit for launched PID $($process.Id)." -ForegroundColor Red
    exit 125
}
Write-Host "Confirmed exit for launched PID $($process.Id)." -ForegroundColor Yellow
Write-LogSummary
exit 124
