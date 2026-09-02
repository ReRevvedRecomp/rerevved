<#[System.Management.Automation.Language.Parser]>
.SYNOPSIS
  Plan or run one isolated native-renderer coverage process.
.DESCRIPTION
  Plan is read-only. Run uses the title's tracked ReXGlue driver in a child
  PowerShell, then records the exit and bounded evidence before returning.
#>
[CmdletBinding()]
param(
    [switch]$Run,
    [string]$RunId, [string]$FixtureId,
    [Alias('Transition')][string]$TransitionId,
    [Alias('FixturePath')][string]$Fixture,
    [string]$FixtureSha256,
    [ValidateSet('cold', 'warm')][string]$CacheClass = 'cold',
    [string]$WarmCacheSeed, [string]$WarmCacheSeedSha256,
    [ValidateSet(1, 2)][int]$Repeat = 1,
    [Alias('ExpectedMarks')][string[]]$ExpectedMark = @(),
    [string[]]$ExpectedScreenshot = @(), [string[]]$StopCondition = @(),
    [string]$StartInvariant, [string]$AuthorizedSkipBoundary,
    [string]$InputDigest,
    [Alias('TitleRoot', 'RepoRoot')][string]$TitleRepo,
    [Alias('SdkRoot')][string]$SdkRepo,
    [string]$TitleCommit, [string]$ExecutableSha256,
    [Alias('SdkInstallRoot')][string]$SdkInstall,
    [ValidateRange(1, 16384)][int]$OutputWidth,
    [ValidateRange(1, 16384)][int]$OutputHeight,
    [ValidateRange(1, 8)][int]$ResolutionScale,
    [ValidateSet('windowed', 'borderless')][string]$WindowMode,
    [ValidateSet('normal', 'fast')][string]$CombatSpeed,
    [string]$OsBuild, [string]$GpuName, [string]$GpuVendorId,
    [string]$GpuDeviceId, [string]$DriverVersion, [string]$D3DFeatureLevel,
    [switch]$OwnerReady, [switch]$OverlaysClosed
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Fail([string]$Message) { throw "coverage blocked: $Message" }
function Full-Path([string]$Path) { return [IO.Path]::GetFullPath($Path) }
function Require-NonEmpty([string]$Value, [string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Value)) { Fail "$Name is required" }
}
function Require-Contained([string]$Root, [string]$Child, [string]$Name) {
    $rootFull = (Full-Path $Root).TrimEnd('\') + '\'
    $childFull = Full-Path $Child
    if (-not $childFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        Fail "$Name must be contained by $Root"
    }
    return $childFull
}
function Require-ContainedNoReparse([string]$Root, [string]$Child, [string]$Name) {
    $rootFull = Full-Path $Root
    $childFull = Require-Contained $rootFull $Child $Name
    if (-not (Test-Path -LiteralPath $childFull -PathType Container)) {
        Fail "$Name not found: $childFull"
    }
    $current = Get-Item -LiteralPath $rootFull -Force
    $items = @($current)
    $relative = [IO.Path]::GetRelativePath($rootFull, $childFull)
    if ($relative -ne '.') {
        foreach ($part in ($relative -split '[\\/]')) {
            $current = Get-Item -LiteralPath (Join-Path $current.FullName $part) -Force
            $items += $current
        }
    }
    foreach ($item in $items) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail "$Name path contains a reparse point: $($item.FullName)"
        }
    }
    Require-NoReparse $childFull $Name
    return $childFull
}
function Require-ContainedFileNoReparse([string]$Root, [string]$Child, [string]$Name) {
    $rootFull = Full-Path $Root
    $childFull = Require-Contained $rootFull $Child $Name
    if (-not (Test-Path -LiteralPath $childFull -PathType Leaf)) {
        Fail "$Name not found: $childFull"
    }
    $current = Get-Item -LiteralPath $rootFull -Force
    $items = @($current)
    foreach ($part in ([IO.Path]::GetRelativePath($rootFull, $childFull) -split '[\\/]')) {
        $current = Get-Item -LiteralPath (Join-Path $current.FullName $part) -Force
        $items += $current
    }
    foreach ($item in $items) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail "$Name path contains a reparse point: $($item.FullName)"
        }
    }
    return $childFull
}
function Require-Exact([string]$Actual, [string]$Expected, [string]$Name) {
    if ($Actual -cne $Expected) { Fail "$Name mismatch: expected $Expected, found $Actual" }
}
function Require-SafeLeafList(
    [string[]]$Values, [string]$Name, [int]$Maximum, [switch]$AllowEmpty
) {
    if (-not $AllowEmpty -and $Values.Count -eq 0) { Fail "$Name requires at least one entry" }
    if ($Values.Count -gt $Maximum) { Fail "$Name accepts at most $Maximum entries" }
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    foreach ($value in $Values) {
        if ([string]::IsNullOrWhiteSpace($value) -or
            [IO.Path]::GetFileName($value) -cne $value -or
            $value -in '.', '..' -or -not $seen.Add($value)) {
            Fail "$Name entries must be non-empty, unique leaf names"
        }
    }
}
function Require-PngSignature([string]$Path, [string]$Name) {
    [byte[]]$expected = 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
    [byte[]]$actual = [byte[]]::new($expected.Count)
    $stream = [IO.File]::OpenRead($Path)
    try { $count = $stream.Read($actual, 0, $actual.Count) }
    finally { $stream.Dispose() }
    if ($count -ne $expected.Count) { throw "$Name is not a PNG file" }
    for ($index = 0; $index -lt $expected.Count; ++$index) {
        if ($actual[$index] -ne $expected[$index]) { throw "$Name is not a PNG file" }
    }
}
function Confirm-ReviewChannel([string]$Phase, [string]$RunId) {
    [byte[]]$bytes = [byte[]]::new(16)
    [Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    $challenge = 'NRD-REVIEW-{0}-{1}-{2}' -f (
        $Phase, $RunId, [Convert]::ToHexString($bytes).ToLowerInvariant()
    )
    [Console]::Out.WriteLine("review-channel: echo $challenge")
    [Console]::Out.Flush()
    $answer = [Console]::In.ReadLine()
    return $null -ne $answer -and $answer -ceq $challenge
}
function Git-Text([string]$Repo, [string[]]$Arguments, [string]$Name) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = @(& git -C $Repo @Arguments 2>&1); $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $saved }
    if ($code -ne 0) { Fail "$Name failed: $($output -join ' ')" }
    return ($output -join "`n").Trim()
}
function Require-CleanRepo([string]$Repo, [string]$Name) {
    if (-not (Test-Path -LiteralPath (Join-Path $Repo '.git'))) { Fail "$Name Git metadata not found: $Repo" }
    $top = Git-Text $Repo @('rev-parse', '--show-toplevel') "$Name root check"
    if ((Full-Path $top) -ne (Full-Path $Repo)) { Fail "$Name root mismatch" }
    $status = Git-Text $Repo @('status', '--porcelain', '--untracked-files=all') "$Name status check"
    if (-not [string]::IsNullOrWhiteSpace($status)) { Fail "$Name repository is not clean" }
}
function Read-Json([string]$Path, [string]$Name) {
    try { return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json }
    catch { Fail "$Name is not valid JSON: $($_.Exception.Message)" }
}
function Get-JsonString([object]$Object, [string]$Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or [string]::IsNullOrWhiteSpace([string]$property.Value)) {
        Fail "SDK lock is missing '$Name'"
    }
    return [string]$property.Value
}
function Normalize-GitRemote([string]$Value) {
    $normalized = $Value.Trim()
    if ($normalized -match '^git@github\.com:(.+)$') {
        $normalized = 'https://github.com/' + $Matches[1]
    }
    return $normalized.TrimEnd('/').Replace('.git', '').ToLowerInvariant()
}
function Require-NoReparse([string]$Root, [string]$Name) {
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { Fail "$Name not found: $Root" }
    $items = @((Get-Item -LiteralPath $Root -Force)) + @(Get-ChildItem -LiteralPath $Root -Force -Recurse)
    foreach ($item in $items) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail "$Name contains a reparse point: $($item.FullName)"
        }
    }
}
function Get-DirectoryDigest([string]$Root) {
    Require-NoReparse $Root 'warm cache seed'
    $files = @(Get-ChildItem -LiteralPath $Root -File -Force -Recurse)
    if ($files.Count -eq 0) { Fail 'warm cache seed must contain at least one file' }
    $records = foreach ($file in $files) {
        $relative = [IO.Path]::GetRelativePath($Root, $file.FullName).Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$relative`n$hash`n"
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes((($records | Sort-Object -CaseSensitive) -join ''))
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
}
function Get-RelativeHashRecord([string]$Root, [string]$Path) {
    return [ordered]@{
        path = [IO.Path]::GetRelativePath($Root, $Path).Replace('\', '/')
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

foreach ($required in @(
    @{ Value = $RunId; Name = 'RunId' }, @{ Value = $FixtureId; Name = 'FixtureId' },
    @{ Value = $TransitionId; Name = 'TransitionId' }, @{ Value = $Fixture; Name = 'Fixture' },
    @{ Value = $FixtureSha256; Name = 'FixtureSha256' },
    @{ Value = $InputDigest; Name = 'InputDigest' }, @{ Value = $TitleCommit; Name = 'TitleCommit' },
    @{ Value = $ExecutableSha256; Name = 'ExecutableSha256' },
    @{ Value = $StartInvariant; Name = 'StartInvariant' },
    @{ Value = $AuthorizedSkipBoundary; Name = 'AuthorizedSkipBoundary' },
    @{ Value = $WindowMode; Name = 'WindowMode' }, @{ Value = $CombatSpeed; Name = 'CombatSpeed' },
    @{ Value = $OsBuild; Name = 'OsBuild' }, @{ Value = $GpuName; Name = 'GpuName' },
    @{ Value = $GpuVendorId; Name = 'GpuVendorId' }, @{ Value = $GpuDeviceId; Name = 'GpuDeviceId' },
    @{ Value = $DriverVersion; Name = 'DriverVersion' },
    @{ Value = $D3DFeatureLevel; Name = 'D3DFeatureLevel' }
)) { Require-NonEmpty $required.Value $required.Name }
if ($OutputWidth -eq 0 -or $OutputHeight -eq 0 -or $ResolutionScale -eq 0) {
    Fail 'OutputWidth, OutputHeight, and ResolutionScale are required'
}
if ($Run -and (-not $OwnerReady -or -not $OverlaysClosed)) {
    Fail '-OwnerReady and -OverlaysClosed are required with -Run'
}
if (-not $Run -and ($OwnerReady -or $OverlaysClosed)) {
    Fail '-OwnerReady and -OverlaysClosed are only valid with -Run'
}
if ($RunId -notmatch '^NRD-RUN-[0-9]{8}-[0-9]{4}$') { Fail 'RunId must match NRD-RUN-YYYYMMDD-NNNN' }
if ($FixtureId -notmatch '^NRD-FIX-000[1-6]$') { Fail 'FixtureId must be NRD-FIX-0001 through NRD-FIX-0006' }
if ($TransitionId -notmatch '^NRD-TRANS-(000[1-9]|001[01])$') { Fail 'TransitionId must be NRD-TRANS-0001 through NRD-TRANS-0011' }
$acceptedFixtureByTransition = @{
    'NRD-TRANS-0001' = @('NRD-FIX-0001')
    'NRD-TRANS-0002' = @('NRD-FIX-0002')
    'NRD-TRANS-0003' = @('NRD-FIX-0002')
    'NRD-TRANS-0004' = @('NRD-FIX-0003')
    'NRD-TRANS-0007' = @('NRD-FIX-0003')
    'NRD-TRANS-0010' = @('NRD-FIX-0001')
}
if (-not $acceptedFixtureByTransition.ContainsKey($TransitionId)) {
    Fail 'TransitionId has no currently available accepted fixture'
}
if ($FixtureId -cnotin $acceptedFixtureByTransition[$TransitionId]) {
    Fail 'FixtureId is not accepted for TransitionId'
}
if ($InputDigest -cne '2d1466cf7a203e123d232cda6a4ab59b9618d3841aaee8f032422e9666c1d303') { Fail 'InputDigest is not the accepted compiled configuration digest' }
if ($FixtureSha256 -notmatch '^[0-9a-f]{64}$') { Fail 'FixtureSha256 must be a lowercase SHA-256 hex digest' }
if ($TitleCommit -notmatch '^[0-9a-f]{40}$') { Fail 'TitleCommit must be an exact lowercase 40-character Git commit' }
if ($ExecutableSha256 -notmatch '^[0-9a-f]{64}$') { Fail 'ExecutableSha256 must be a lowercase SHA-256 hex digest' }
if ($GpuVendorId -notmatch '^(0x)?[0-9A-Fa-f]{4,8}$' -or
    $GpuDeviceId -notmatch '^(0x)?[0-9A-Fa-f]{4,8}$') {
    Fail 'GPU vendor and device IDs must be hexadecimal identifiers'
}
Require-SafeLeafList $ExpectedMark 'ExpectedMark' 6 -AllowEmpty
if (@($ExpectedMark | Where-Object { $_.Contains(',') }).Count -ne 0) {
    Fail 'ExpectedMark entries cannot contain commas'
}
Require-SafeLeafList $ExpectedScreenshot 'ExpectedScreenshot' 6
foreach ($name in $ExpectedScreenshot) {
    if ([IO.Path]::GetExtension($name) -cne '.png') {
        Fail 'ExpectedScreenshot entries must use the .png extension'
    }
}
if ($StopCondition.Count -eq 0 -or $StopCondition.Count -gt 8 -or
    @($StopCondition | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -ne 0) {
    Fail 'StopCondition requires one through eight non-empty entries'
}
$lockedNewGameRouteByTransition = @{
    'NRD-TRANS-0002' = @{
        ExpectedMark = @('new-game-setup-romans')
        ExpectedScreenshot = @(
            'main-menu.png', 'single-player-menu.png',
            'difficulty-warlord.png', 'civilization-romans.png'
        )
        StopCondition = @(
            'choose Single Player, New Game, Warlord, and highlight Romans without confirming',
            'close immediately after civilization-romans.png capture without confirming Romans, entering gameplay, or saving'
        )
    }
    'NRD-TRANS-0003' = @{
        ExpectedMark = @('first-settled-human-turn-map')
        ExpectedScreenshot = @(
            'main-menu.png', 'single-player-menu.png',
            'difficulty-warlord.png', 'civilization-romans.png',
            'first-settled-human-turn-map.png'
        )
        StopCondition = @(
            'choose Single Player, New Game, Warlord, and confirm Romans',
            'close immediately after first-settled-human-turn-map.png capture before gameplay input or saving'
        )
    }
}
if ($FixtureId -eq 'NRD-FIX-0002') {
    $lockedRoute = $lockedNewGameRouteByTransition[$TransitionId]
    if ($null -eq $lockedRoute -or
        ($ExpectedMark -join "`n") -cne ($lockedRoute.ExpectedMark -join "`n") -or
        ($ExpectedScreenshot -join "`n") -cne ($lockedRoute.ExpectedScreenshot -join "`n") -or
        ($StopCondition -join "`n") -cne ($lockedRoute.StopCondition -join "`n") -or
        $StartInvariant -cne 'process absent' -or
        $AuthorizedSkipBoundary -cne 'boot intro movie only after owner readiness; do not skip any setup panel') {
        Fail 'NRD-FIX-0002 plan differs from the owner-locked route'
    }
}
if ($CacheClass -eq 'warm') {
    Require-NonEmpty $WarmCacheSeed 'WarmCacheSeed'
    Require-NonEmpty $WarmCacheSeedSha256 'WarmCacheSeedSha256'
    if ($WarmCacheSeedSha256 -notmatch '^[0-9a-f]{64}$') { Fail 'WarmCacheSeedSha256 must be a lowercase SHA-256 hex digest' }
} elseif ($WarmCacheSeed -or $WarmCacheSeedSha256) {
    Fail 'warm cache seed parameters are only valid with -CacheClass warm'
}

$scriptRepo = Full-Path (Join-Path $PSScriptRoot '..')
$TitleRepo = if ($TitleRepo) { Full-Path $TitleRepo } else { $scriptRepo }
$SdkRepo = if ($SdkRepo) { Full-Path $SdkRepo } else { Full-Path (Join-Path $TitleRepo '..\rerevved-rexglue-sdk') }
$fixturePath = if ([IO.Path]::IsPathRooted($Fixture)) { Full-Path $Fixture } else { Full-Path (Join-Path $TitleRepo $Fixture) }
$rexglueScript = Full-Path (Join-Path $TitleRepo 'scripts\rexglue.ps1')
$exePath = Join-Path $TitleRepo 'out\build\win-amd64-release\rerevved.exe'
$baseXexPath = Join-Path $TitleRepo 'game\default.xex'
$titleUpdatePath = Join-Path $TitleRepo 'game\default.xexp'
$outRoot = Full-Path (Join-Path $TitleRepo 'out')
$evidenceRoot = Full-Path (Join-Path $TitleRepo 'out\evidence\native-renderer-d3d')
$runRoot = Full-Path (Join-Path $evidenceRoot $RunId)
foreach ($path in @($fixturePath, $rexglueScript, $exePath, $baseXexPath, $titleUpdatePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Fail "required input not found: $path" }
}
$null = Require-ContainedFileNoReparse $TitleRepo $fixturePath 'fixture path'
$null = Require-ContainedFileNoReparse $TitleRepo $rexglueScript 'ReXGlue script'
$null = Require-ContainedFileNoReparse $TitleRepo $exePath 'executable path'
$null = Require-ContainedFileNoReparse $TitleRepo $baseXexPath 'base XEX path'
$null = Require-ContainedFileNoReparse $TitleRepo $titleUpdatePath 'title update path'
$null = Require-Contained $evidenceRoot $runRoot 'run root'
$fixtureHash = (Get-FileHash -LiteralPath $fixturePath -Algorithm SHA256).Hash.ToLowerInvariant()
Require-Exact $fixtureHash $FixtureSha256.ToLowerInvariant() 'fixture SHA-256'
$acceptedFixtureSha256 = @{
    'NRD-FIX-0001' = '2d1466cf7a203e123d232cda6a4ab59b9618d3841aaee8f032422e9666c1d303'
    'NRD-FIX-0002' = 'a1bcefa50427ec719fe4d5721cb9438ee3f44ec7c09db48fdc73c3d326e9d684'
    'NRD-FIX-0003' = '06e885f11044153d3ddbb7259eeffceb14aa0600d79b6083b526e646630e463e'
}
if ($acceptedFixtureSha256.ContainsKey($FixtureId)) {
    Require-Exact $fixtureHash $acceptedFixtureSha256[$FixtureId] "accepted $FixtureId SHA-256"
}
$fixtureRelative = [IO.Path]::GetRelativePath($TitleRepo, $fixturePath).Replace('\', '/')
if ($FixtureId -in 'NRD-FIX-0001', 'NRD-FIX-0002') {
    $expectedFixturePath = if ($FixtureId -eq 'NRD-FIX-0001') {
        'config/native_renderer_fixture_0001.toml'
    } else {
        'config/native_renderer_fixture_0002.json'
    }
    Require-Exact $fixtureRelative $expectedFixturePath "$FixtureId recipe descriptor path"
}
$fixtureStagedRelative = if ($FixtureId -eq 'NRD-FIX-0003') { 'user-data/save5.sve' } else { $null }
Require-CleanRepo $TitleRepo 'title'
Require-CleanRepo $SdkRepo 'SDK'
$trackedLauncher = Git-Text $TitleRepo @('ls-files', '--error-unmatch', 'scripts/rexglue.ps1') 'tracked ReXGlue script check'
Require-Exact $trackedLauncher.Replace('\', '/') 'scripts/rexglue.ps1' 'tracked ReXGlue script path'
$actualTitleCommit = Git-Text $TitleRepo @('rev-parse', 'HEAD') 'title commit check'
Require-Exact $actualTitleCommit.ToLowerInvariant() $TitleCommit.ToLowerInvariant() 'title commit'

$lockPath = Join-Path $TitleRepo 'rexglue-sdk.lock.json'
if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) { Fail "SDK lock not found: $lockPath" }
$lock = Read-Json $lockPath 'SDK lock'
$lockRepository = Get-JsonString $lock 'repository'
$lockCommit = Get-JsonString $lock 'commit'
$lockVersion = Get-JsonString $lock 'version'
$lockDirty = Get-JsonString $lock 'dirty'
$lockPlatform = Get-JsonString $lock 'platform'
$sdkHead = Git-Text $SdkRepo @('rev-parse', 'HEAD') 'SDK HEAD check'
Require-Exact $sdkHead.ToLowerInvariant() $lockCommit.ToLowerInvariant() 'SDK lock/HEAD commit'
$sdkOrigin = Git-Text $SdkRepo @('config', '--get', 'remote.origin.url') 'SDK origin check'
Require-Exact (Normalize-GitRemote $sdkOrigin) (Normalize-GitRemote $lockRepository) 'SDK lock/origin repository'
Require-Exact $lockDirty 'clean' 'SDK lock dirty state'
$SdkInstall = if ($SdkInstall) { Full-Path $SdkInstall } else { Join-Path $SdkRepo 'out\install\win-amd64' }
$null = Require-ContainedNoReparse $SdkRepo $SdkInstall 'SDK install root'
$configPath = Join-Path $SdkInstall 'lib\cmake\rexglue\rexglueConfig.cmake'
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) { Fail "installed SDK config not found: $configPath" }
$configText = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8
$versionMatch = [regex]::Match($configText, 'set\(REXGLUE_VERSION_STRING "([^"]+)"\)')
if (-not $versionMatch.Success) { Fail "installed SDK version not found: $configPath" }
Require-Exact $versionMatch.Groups[1].Value $lockVersion 'installed SDK version'
foreach ($field in @(
    @{ Name = 'GIT_REVISION'; Expected = $lockCommit },
    @{ Name = 'GIT_DIRTY'; Expected = $lockDirty },
    @{ Name = 'BUILD_PLATFORM'; Expected = $lockPlatform }
)) {
    $match = [regex]::Match($configText, 'set\(REXGLUE_' + $field.Name + ' "([^"]+)"\)')
    if (-not $match.Success) { Fail "installed SDK $($field.Name) not found: $configPath" }
    Require-Exact $match.Groups[1].Value $field.Expected "installed SDK $($field.Name)"
}
$executableHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
Require-Exact $executableHash $ExecutableSha256 'executable SHA-256'
$baseXexHash = (Get-FileHash -LiteralPath $baseXexPath -Algorithm SHA256).Hash.ToLowerInvariant()
Require-Exact $baseXexHash 'b59b8957a3ed9dd90e9296c96d5c7ab1b16078d3f08b015582714a06c7d6a7bd' 'base XEX SHA-256'
$titleUpdateHash = (Get-FileHash -LiteralPath $titleUpdatePath -Algorithm SHA256).Hash.ToLowerInvariant()
Require-Exact $titleUpdateHash 'c1fc6149a63550987d991efdbb80e3697845a9a49d3f2ec180ea9817db8d12d4' 'title update SHA-256'
if (Test-Path -LiteralPath $runRoot) { Fail "run root already exists: $runRoot" }

$warmCacheDigest = $null
if ($CacheClass -eq 'warm') {
    $WarmCacheSeed = Full-Path $WarmCacheSeed
    $warmPrefix = $WarmCacheSeed.TrimEnd('\') + '\'
    if ($runRoot.StartsWith($warmPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        Fail 'warm cache seed must not contain the future run root'
    }
    $warmCacheDigest = Get-DirectoryDigest $WarmCacheSeed
    Require-Exact $warmCacheDigest $WarmCacheSeedSha256 'warm cache seed SHA-256'
}
$exeRelative = [IO.Path]::GetRelativePath($TitleRepo, $exePath).Replace('\', '/')
$rendererConfig = [ordered]@{
    xenos_enabled = $true; rov_enabled = $true; guest_width = 1280; guest_height = 720
    output_width = $OutputWidth; output_height = $OutputHeight
    resolution_scale = $ResolutionScale; window_mode = $WindowMode; combat_speed = $CombatSpeed
}
$rendererConfigText = @(
    'xenos_enabled=true', 'rov_enabled=true', 'guest_width=1280', 'guest_height=720',
    ('output_width=' + $OutputWidth), ('output_height=' + $OutputHeight),
    ('resolution_scale=' + $ResolutionScale), ('window_mode=' + $WindowMode),
    ('combat_speed=' + $CombatSpeed)
) -join "`n"
$rendererConfigBytes = [Text.Encoding]::UTF8.GetBytes($rendererConfigText + "`n")
$rendererConfig['configuration_digest'] = [Convert]::ToHexString(
    [Security.Cryptography.SHA256]::HashData($rendererConfigBytes)
).ToLowerInvariant()
$manifest = [ordered]@{
    schema = 'rerevved.native_renderer.run.v2'; run_id = $RunId
    fixture_id = $FixtureId; fixture = $fixtureRelative; fixture_sha256 = $fixtureHash
    fixture_staged_path = $fixtureStagedRelative
    transition_id = $TransitionId; input_digest = $InputDigest.ToLowerInvariant()
    expected_marks = @($ExpectedMark); repeat = $Repeat; cache_class = $CacheClass
    cache_seed_sha256 = $warmCacheDigest
    title_commit = $actualTitleCommit.ToLowerInvariant(); title_dirty = $false
    sdk_commit = $sdkHead.ToLowerInvariant(); sdk_dirty = $false
    sdk_version = $lockVersion; executable = $exeRelative
    executable_sha256 = $executableHash
    base_xex = 'game/default.xex'
    base_xex_sha256 = $baseXexHash
    title_update = 'game/default.xexp'
    title_update_sha256 = $titleUpdateHash
    xenos_enabled = $true; rov_enabled = $true
    renderer_config = $rendererConfig
    host_graphics = [ordered]@{
        os_build = $OsBuild; gpu_name = $GpuName
        gpu_vendor_id = $GpuVendorId.ToLowerInvariant()
        gpu_device_id = $GpuDeviceId.ToLowerInvariant()
        driver_version = $DriverVersion; d3d_feature_level = $D3DFeatureLevel
    }
    readiness = [ordered]@{
        owner_ready = [bool]$OwnerReady; overlay_policy = 'closed'
        overlays_closed_before_launch = [bool]$OverlaysClosed
        start_invariant = $StartInvariant; authorized_skip_boundary = $AuthorizedSkipBoundary
        expected_screenshots = @($ExpectedScreenshot); stop_conditions = @($StopCondition)
    }
    output_root = '.'; output_directory = 'observer'; screenshot_directory = 'screenshots'
    shader_directory = 'shaders'; user_data_directory = 'user-data'
    cache_directory = ('cache/' + $CacheClass); save_directory = 'user-data'; checkpoint = 'planned'
    timing = [ordered]@{ started_utc = $null; ended_utc = $null }
    command_result = [ordered]@{ exit_code = $null; classification = 'not-run' }
    operator_review = [ordered]@{
        overlays_remained_closed = $null; reached_marks = @(); unexpected_errors = @()
    }
    log = [ordered]@{ path = 'coverage.log'; sha256 = $null }
    artifacts = @(); screenshots = @(); saves = @()
}
$launchArguments = @(
    '--gpu_plugin=xenos', '--render_target_path_d3d12=rov',
    ('--combat_speed=' + $rendererConfig.combat_speed),
    '--video_mode_width=1280', '--video_mode_height=720',
    ('--window_width=' + $rendererConfig.output_width),
    ('--window_height=' + $rendererConfig.output_height),
    ('--resolution_scale=' + $rendererConfig.resolution_scale),
    ('--fullscreen=' + $(if ($rendererConfig.window_mode -eq 'borderless') { 'true' } else { 'false' })),
    ('--native_renderer_coverage_run=' + $RunId),
    ('--native_renderer_coverage_transition=' + $TransitionId),
    ('--native_renderer_coverage_input_digest=' + $InputDigest.ToLowerInvariant()),
    '--native_renderer_coverage_output=observer'
)
$launchArgumentJson = ConvertTo-Json -Compress -InputObject @($launchArguments)
if (-not $Run) {
    $relativeRunRoot = [IO.Path]::GetRelativePath($TitleRepo, $runRoot).Replace('\', '/')
    $relativeSdkRepo = [IO.Path]::GetRelativePath($TitleRepo, $SdkRepo).Replace('\', '/')
    $relativeSdkInstall = [IO.Path]::GetRelativePath($TitleRepo, $SdkInstall).Replace('\', '/')
    Write-Output ("plan: valid run={0} fixture={1} transition={2} cache={3} repeat={4} renderer=xenos/rov run_root={5}" -f $RunId, $FixtureId, $TransitionId, $CacheClass, $Repeat, $relativeRunRoot)
    Write-Output ("plan: marks={0} screenshots={1} stop_conditions={2}" -f ($ExpectedMark -join ','), ($ExpectedScreenshot -join ','), ($StopCondition -join ' | '))
    $planChildArguments = @(
        '-NoProfile', '-NonInteractive', '-File', 'scripts/rexglue.ps1',
        '-Stage', 'Launch', '-Interactive', '-SdkRepo', $relativeSdkRepo,
        '-SdkInstall', $relativeSdkInstall,
        '-UserDataRoot', "$relativeRunRoot/user-data",
        '-CacheRoot', "$relativeRunRoot/cache/$CacheClass",
        '-LogPath', "$relativeRunRoot/coverage.log",
        '-LaunchArgumentJson', $launchArgumentJson
    )
    $plan = [ordered]@{
        schema = 'rerevved.native_renderer.plan.v2'
        run_root = $relativeRunRoot
        launcher = 'scripts/rexglue.ps1'
        metadata = $manifest
        child_pwsh = 'pwsh'
        child_arguments = @($planChildArguments)
        launch_arguments = @($launchArguments)
        launch_argument_json = $launchArgumentJson
    }
    $plan | ConvertTo-Json -Depth 20
    exit 0
}

foreach ($condition in $StopCondition) {
    Write-Output ("operator-stop: planned stop condition: {0}" -f $condition)
}
Write-Output 'operator-stop: after the planned capture, close immediately without entering another transition'
if (-not (Confirm-ReviewChannel 'pre-launch' $RunId)) {
    Fail 'operator review channel pre-launch challenge failed'
}

# The tracked launch driver executes in a child process so its `exit` cannot
# bypass post-launch evidence recording here.
foreach ($existingPath in @($TitleRepo, $outRoot, $evidenceRoot)) {
    if (Test-Path -LiteralPath $existingPath) {
        $existingItem = Get-Item -LiteralPath $existingPath -Force
        if (($existingItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail "evidence path contains a reparse point: $existingPath"
        }
    }
}
if (Test-Path -LiteralPath $outRoot -PathType Leaf) {
    Fail "title out root is not a directory: $outRoot"
}
if (Test-Path -LiteralPath $evidenceRoot -PathType Leaf) {
    Fail "evidence root is not a directory: $evidenceRoot"
}
if (-not (Test-Path -LiteralPath $evidenceRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $evidenceRoot -Force:$false | Out-Null
}
$null = Require-ContainedNoReparse $TitleRepo $evidenceRoot 'evidence root'
New-Item -ItemType Directory -Path $runRoot -Force:$false | Out-Null
$observerDirectory = Join-Path $runRoot 'observer'
$userData = Join-Path $runRoot 'user-data'
$runtimeCache = Join-Path $runRoot ('cache\' + $CacheClass)
$screenshotDirectory = Join-Path $runRoot 'screenshots'
$shaderDirectory = Join-Path $runRoot 'shaders'
New-Item -ItemType Directory -Path $observerDirectory, $userData, $runtimeCache, $screenshotDirectory, $shaderDirectory -Force:$false | Out-Null
$null = Require-NoReparse $runRoot 'run root'
if ($fixtureStagedRelative) {
    $fixtureStagedPath = Join-Path $runRoot $fixtureStagedRelative
    $null = Require-Contained $userData $fixtureStagedPath 'staged fixture path'
    Copy-Item -LiteralPath $fixturePath -Destination $fixtureStagedPath -Force:$false
    $stagedFixtureHash = (
        Get-FileHash -LiteralPath $fixtureStagedPath -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    Require-Exact $stagedFixtureHash $fixtureHash 'staged fixture SHA-256'
}
if ($CacheClass -eq 'warm') {
    foreach ($seedItem in @(Get-ChildItem -LiteralPath $WarmCacheSeed -Force)) {
        Copy-Item -LiteralPath $seedItem.FullName -Destination $runtimeCache -Recurse -Force:$false
    }
    Require-Exact (Get-DirectoryDigest $runtimeCache) $warmCacheDigest 'copied warm cache seed SHA-256'
}
$runPath = Join-Path $runRoot 'run.json'
$manifest['checkpoint'] = 'launching'
$manifest['timing']['started_utc'] = [DateTime]::UtcNow.ToString('o')
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $runPath -Encoding ascii
$logPath = Join-Path $runRoot 'coverage.log'
$pwshPath = (Get-Command pwsh -CommandType Application -ErrorAction Stop).Source
$savedNativePreference = $PSNativeCommandUseErrorActionPreference
$PSNativeCommandUseErrorActionPreference = $false
$childArguments = @(
    '-NoProfile', '-NonInteractive', '-File', $rexglueScript,
    '-Stage', 'Launch', '-Interactive', '-SdkRepo', $SdkRepo,
    '-SdkInstall', $SdkInstall, '-UserDataRoot', $userData,
    '-CacheRoot', $runtimeCache, '-LogPath', $logPath,
    '-LaunchArgumentJson', $launchArgumentJson
)
try {
    $launchOutput = @(& $pwshPath @childArguments 2>&1)
    $launchExitCode = [int]$LASTEXITCODE
} finally {
    $PSNativeCommandUseErrorActionPreference = $savedNativePreference
}
$launchOutput | Write-Output
$manifest['timing']['ended_utc'] = [DateTime]::UtcNow.ToString('o')
$manifest['command_result']['exit_code'] = $launchExitCode
$manifest['command_result']['classification'] = if ($launchExitCode -eq 0) { 'process-exited-zero' } else { 'launch-failed' }
$postLaunchErrors = [Collections.Generic.List[string]]::new()
if (Test-Path -LiteralPath $logPath -PathType Leaf) {
    $manifest['log']['sha256'] = (Get-FileHash -LiteralPath $logPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $logBody = Get-Content -LiteralPath $logPath -Raw -Encoding UTF8
    $beginCount = ([regex]::Matches($logBody, [regex]::Escape('NRD-COVERAGE-BEGIN'))).Count
    $endCount = ([regex]::Matches($logBody, [regex]::Escape('NRD-COVERAGE-END'))).Count
    if ($beginCount -ne 1 -or $endCount -ne 1 -or
        $logBody.IndexOf('NRD-COVERAGE-BEGIN', [StringComparison]::Ordinal) -gt
        $logBody.IndexOf('NRD-COVERAGE-END', [StringComparison]::Ordinal)) {
        $postLaunchErrors.Add('coverage log must contain one ordered begin/end sentinel pair')
    }
} else { $postLaunchErrors.Add('ReXGlue did not create the coverage log') }
$coveragePath = Join-Path $observerDirectory 'coverage.json'
$observerEntries = @(Get-ChildItem -LiteralPath $observerDirectory -Force)
if ($observerEntries.Count -ne 1 -or -not (Test-Path -LiteralPath $coveragePath -PathType Leaf)) {
    $postLaunchErrors.Add('observer output must contain only coverage.json')
} else { $manifest['artifacts'] = @((Get-RelativeHashRecord $runRoot $coveragePath)) }
$screenshotRecords = [Collections.Generic.List[object]]::new()
foreach ($name in $ExpectedScreenshot) {
    $path = Join-Path $screenshotDirectory $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $postLaunchErrors.Add("expected screenshot missing: $name")
    } else {
        try { Require-PngSignature $path "expected screenshot $name" }
        catch { $postLaunchErrors.Add($_.Exception.Message) }
        $screenshotRecords.Add((Get-RelativeHashRecord $runRoot $path))
    }
}
$extraScreenshots = @(Get-ChildItem -LiteralPath $screenshotDirectory -Force | Where-Object {
    $_.PSIsContainer -or $_.Name -cnotin $ExpectedScreenshot
})
if ($extraScreenshots.Count -ne 0) { $postLaunchErrors.Add('screenshot directory contains an unplanned file') }
$manifest['screenshots'] = @($screenshotRecords)
if (@(Get-ChildItem -LiteralPath $shaderDirectory -Force).Count -ne 0) {
    $postLaunchErrors.Add('shader directory contains an unplanned artifact')
}
$saveRecords = [Collections.Generic.List[object]]::new()
foreach ($save in @(Get-ChildItem -LiteralPath $userData -File -Force -Recurse -Filter '*.sve')) {
    $relativeSave = [IO.Path]::GetRelativePath($runRoot, $save.FullName).Replace('\', '/')
    if ($relativeSave -cne $fixtureStagedRelative) {
        $saveRecords.Add((Get-RelativeHashRecord $runRoot $save.FullName))
    }
}
$manifest['saves'] = @($saveRecords)
if ($FixtureId -eq 'NRD-FIX-0002' -and $saveRecords.Count -ne 0) {
    $postLaunchErrors.Add('NRD-FIX-0002 prohibits save output')
}
if ($fixtureStagedRelative) {
    $fixtureStagedPath = Join-Path $runRoot $fixtureStagedRelative
    if (-not (Test-Path -LiteralPath $fixtureStagedPath -PathType Leaf) -or
        (Get-FileHash -LiteralPath $fixtureStagedPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $fixtureHash) {
        $postLaunchErrors.Add('staged fixture changed during the run')
    }
}
if ($launchExitCode -eq 0 -and -not (Confirm-ReviewChannel 'post-exit' $RunId)) {
    $postLaunchErrors.Add('operator review channel post-exit challenge failed')
} elseif ($launchExitCode -eq 0) {
    $expectedStopConfirmation = '{0}:closed-immediately-after-planned-capture-without-entering-another-transition' -f $RunId
    Write-Output ("review: if you closed immediately after the planned capture without entering another transition, enter exactly {0}; otherwise enter no" -f $expectedStopConfirmation)
    $stopAnswer = [Console]::In.ReadLine()
    if ($null -eq $stopAnswer) { $stopAnswer = '' }
    if ([string]::IsNullOrWhiteSpace($stopAnswer)) {
        $postLaunchErrors.Add('planned-stop confirmation was blank')
    } elseif ($stopAnswer -ceq 'no') {
        $postLaunchErrors.Add('operator did not close immediately after the planned capture')
    } elseif ($stopAnswer -cne $expectedStopConfirmation) {
        $postLaunchErrors.Add('planned-stop confirmation was stale, malformed, or mismatched')
    }
    Write-Output 'review: did all overlays remain closed for the entire run? Enter yes or no'
    $overlayAnswer = [Console]::In.ReadLine()
    if ($null -eq $overlayAnswer) { $overlayAnswer = '' }
    $overlayAnswer = $overlayAnswer.Trim().ToLowerInvariant()
    $manifest['operator_review']['overlays_remained_closed'] = ($overlayAnswer -eq 'yes')
    if ($overlayAnswer -notin 'yes', 'no') { $postLaunchErrors.Add('overlay review answer was not yes or no') }
    if ($overlayAnswer -eq 'no') { $postLaunchErrors.Add('an overlay was open during the run') }
    Write-Output 'review: enter reached checkpoint names in order, comma-separated; enter none for no marks'
    $markAnswer = [Console]::In.ReadLine()
    if ($null -eq $markAnswer) { $markAnswer = '' }
    $markAnswer = $markAnswer.Trim()
    if ([string]::IsNullOrWhiteSpace($markAnswer)) {
        $postLaunchErrors.Add('checkpoint review answer was blank')
    }
    $reachedMarks = @(if ($markAnswer -ne 'none' -and
        -not [string]::IsNullOrWhiteSpace($markAnswer)) {
        $markAnswer.Split(',') | ForEach-Object { $_.Trim() }
    })
    if (@($reachedMarks | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -ne 0) {
        $postLaunchErrors.Add('checkpoint review answer contained an empty mark')
    }
    $manifest['operator_review']['reached_marks'] = @($reachedMarks)
    if (($reachedMarks -join "`n") -cne ($ExpectedMark -join "`n")) { $postLaunchErrors.Add('reached checkpoint sequence does not match the plan') }
    Write-Output 'review: enter unexpected errors separated by |; enter none for no errors'
    $errorAnswer = [Console]::In.ReadLine()
    if ($null -eq $errorAnswer) { $errorAnswer = '' }
    $errorAnswer = $errorAnswer.Trim()
    if ([string]::IsNullOrWhiteSpace($errorAnswer)) {
        $postLaunchErrors.Add('unexpected-error review answer was blank')
    }
    $unexpectedErrors = @(if ($errorAnswer -ne 'none' -and
        -not [string]::IsNullOrWhiteSpace($errorAnswer)) {
        $errorAnswer.Split('|') | ForEach-Object { $_.Trim() }
    })
    if (@($unexpectedErrors | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -ne 0) {
        $postLaunchErrors.Add('unexpected-error review answer contained an empty entry')
    }
    $manifest['operator_review']['unexpected_errors'] = @($unexpectedErrors)
    if ($unexpectedErrors.Count -ne 0) { $postLaunchErrors.Add('operator reported unexpected errors') }
}
if ($launchExitCode -ne 0) { $postLaunchErrors.Add("ReXGlue launch failed (exit $launchExitCode)") }
if ($postLaunchErrors.Count -eq 0) {
    $manifest['checkpoint'] = 'complete'; $manifest['command_result']['classification'] = 'accepted'
} else { $manifest['checkpoint'] = 'rejected' }
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $runPath -Encoding ascii
if ($postLaunchErrors.Count -ne 0) { Fail ($postLaunchErrors -join '; ') }
Write-Output "run: complete run_root=$runRoot"
