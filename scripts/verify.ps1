Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$failures = New-Object 'System.Collections.Generic.List[string]'

function Add-Failure([string]$Message) {
    [void]$failures.Add($Message)
}

function Relative-Path([string]$Path) {
    return $Path.Substring($repo.Length + 1).Replace('\', '/')
}

try {
    $tracked = @(git -C $repo ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0 -or $tracked.Count -eq 0) {
        throw 'git ls-files returned no tracked files'
    }
} catch {
    Add-Failure "tracked file listing failed: $($_.Exception.Message)"
    $tracked = @()
}

# A dirty checkout may contain an intentionally deleted tracked file. Parse
# present tracked and nonignored untracked files so pre-stage checks cover the
# same candidate content that CI will inspect after commit.
$trackedPaths = @($tracked | ForEach-Object { Join-Path $repo $_ } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })

# Parse every tracked JSON file without looking at generated or local-only output.
$jsonFiles = @($trackedPaths | Where-Object { $_ -match '(?i)\.json$' })
foreach ($file in $jsonFiles) {
    try {
        $null = Get-Content -LiteralPath $file -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        Add-Failure "JSON parse failed: $(Relative-Path $file): $($_.Exception.Message)"
    }
}

# TOML has no Windows PowerShell built-in parser; tomllib is Python's standard library.
$tomlFiles = @($trackedPaths | Where-Object { $_ -match '(?i)\.toml$' })
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    Add-Failure 'python executable is required for TOML, AST, and unit-test checks'
} else {
$tomlCode = @'
import pathlib, sys, tomllib
for name in sys.argv[1:]:
    if name != chr(45) * 2:
        tomllib.loads(pathlib.Path(name).read_bytes().decode())
'@
    if ($tomlFiles.Count -eq 0) {
        Add-Failure 'no tracked TOML files found'
    } else {
        $savedErrorAction = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $tomlOutput = @(& $python.Source -c $tomlCode -- $tomlFiles 2>&1)
            $tomlExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedErrorAction
        }
        if ($tomlExitCode -ne 0) {
            Add-Failure "TOML parse failed: $($tomlOutput -join ' ')"
        }
    }
}

# Mirror CI's clang-format check for tracked first-party C/C++ only. Generated
# guest code and dependencies are outside this repository-hygiene gate.
$cppFiles = @($trackedPaths | Where-Object {
    $relative = Relative-Path $_
    $relative -match '^src/' -and
        $_ -match '(?i)\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$'
})
$clangFormat = Get-Command clang-format -ErrorAction SilentlyContinue
$clangFormatStatus = 'skipped'
if ($clangFormat) {
    $clangFormatStatus = 'clean'
    foreach ($file in $cppFiles) {
        $savedErrorAction = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $formatOutput = @(& $clangFormat.Source --dry-run --Werror $file 2>&1)
            $formatExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedErrorAction
        }
        if ($formatExitCode -ne 0) {
            $clangFormatStatus = 'failed'
            Add-Failure "clang-format failed: $(Relative-Path $file): $($formatOutput -join ' ')"
        }
    }
}

# Parse all tracked Python and run only the repository's standard-library tests.
$pythonFiles = @($trackedPaths | Where-Object { $_ -match '(?i)\.py$' })
if ($python) {
$astCode = @'
import ast, pathlib, sys
for name in sys.argv[1:]:
    if name != chr(45) * 2:
        ast.parse(pathlib.Path(name).read_bytes().decode(), filename=name)
'@
    if ($pythonFiles.Count -eq 0) {
        Add-Failure 'no tracked Python files found'
    } else {
        $savedErrorAction = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $astOutput = @(& $python.Source -c $astCode -- $pythonFiles 2>&1)
            $astExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedErrorAction
        }
        if ($astExitCode -ne 0) {
            Add-Failure "Python AST parse failed: $($astOutput -join ' ')"
        }
    }
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $testOutput = @(& $python.Source -B -m unittest discover -s (Join-Path $repo 'scripts/tests') -p 'test_*.py' 2>&1)
        $testExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorAction
    }
    if ($testExitCode -ne 0) {
        Add-Failure "Python tests failed: $($testOutput -join ' ')"
    }
}

# Parse every tracked PowerShell file with the native language parser.
$psFiles = @($trackedPaths | Where-Object { $_ -match '(?i)\.ps1$' })
$parser = [System.Management.Automation.Language.Parser]
foreach ($file in $psFiles) {
    try {
        $tokens = $null
        $errors = $null
        $null = $parser::ParseFile($file, [ref]$tokens, [ref]$errors)
        if ($errors.Count -gt 0) { throw $errors[0].Message }
    } catch {
        Add-Failure "PowerShell AST parse failed: $(Relative-Path $file): $($_.Exception.Message)"
    }
}

# Validate local Markdown links against the tracked checkout.
$markdownFiles = @($trackedPaths | Where-Object { $_ -match '(?i)\.md$' })
foreach ($file in $markdownFiles) {
    $markdown = Get-Content -LiteralPath $file -Raw -Encoding UTF8
    foreach ($match in [regex]::Matches($markdown, '\]\(([^)]*)\)')) {
        $target = $match.Groups[1].Value.Trim()
        if (-not $target -or $target -match '^(?i)(https?|mailto):' -or $target.StartsWith('#')) {
            continue
        }
        $target = ($target -split '\s+', 2)[0].Trim('<', '>')
        $target = ($target -split '#', 2)[0]
        if (-not $target) { continue }
        try { $target = [Uri]::UnescapeDataString($target) } catch { }
        $candidate = Join-Path (Split-Path -Parent $file) $target
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            Add-Failure "Markdown link missing: $(Relative-Path $file) -> $target"
        }
    }
}

# Tracked text is intentionally ASCII so byte-oriented tools remain deterministic.
# Binary release artwork is validated by its consuming build tools instead.
$asciiFiles = @($trackedPaths | Where-Object { $_ -notmatch '(?i)\.(ico|png)$' })
foreach ($file in $asciiFiles) {
    try {
        foreach ($byte in [IO.File]::ReadAllBytes($file)) {
            if ($byte -gt 127) {
                throw "non-ASCII byte 0x$('{0:X2}' -f $byte)"
            }
        }
    } catch {
        Add-Failure "ASCII check failed: $(Relative-Path $file): $($_.Exception.Message)"
    }
}

# Reject proprietary inputs, reverse-engineering state, and generated guest code
# even if an ignore rule is later weakened or bypassed.
$forbidden = @($tracked | Where-Object {
    ($_ -match '(^|/)(game|private|logs|out|scratch)/') -or
    ($_ -match '(?i)\.(bin|iso|xex|xexp|gpr|gzf)$') -or
    ($_ -match '^generated/')
})
if ($forbidden.Count -gt 0) {
    Add-Failure "Forbidden tracked path: $($forbidden -join ', ')"
}

# Check both worktree and index whitespace errors without changing git state.
foreach ($cached in @($false, $true)) {
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($cached) {
            $diffOutput = @(& git -C $repo diff --cached --check 2>&1)
        } else {
            $diffOutput = @(& git -C $repo diff --check 2>&1)
        }
        $diffExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorAction
    }
    if ($diffExitCode -ne 0) {
        $label = if ($cached) { 'cached git diff' } else { 'git diff' }
        Add-Failure "$label check failed: $($diffOutput -join ' ')"
    }
}

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) { Write-Error $failure }
    exit 1
}

Write-Output ("verify: passed JSON={0} TOML={1} ClangFormat={2}:{3} PythonAST={4} PythonTests=1 PowerShellAST={5} MarkdownLinks={6} ASCII=1 repository-hygiene=1 git-diff=1 cached-diff=1" -f $jsonFiles.Count, $tomlFiles.Count, $clangFormatStatus, $cppFiles.Count, $pythonFiles.Count, $psFiles.Count, $markdownFiles.Count)
