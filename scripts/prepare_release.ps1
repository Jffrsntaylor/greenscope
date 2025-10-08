[CmdletBinding()]
param(
    [string]$Version = "0.6.0",
    [switch]$Publish,
    [string]$ReleaseNotesPath,
    [string]$ReleaseNotes
)

$ErrorActionPreference = 'Stop'
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $projectRoot

function Get-GitBranch {
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        throw 'git executable not found in PATH. Install Git before preparing a release.'
    }
    return (& git rev-parse --abbrev-ref HEAD).Trim()
}

function Get-ChangelogSection {
    param([string]$Tag)
    $defaultChangelog = Join-Path $projectRoot 'CHANGELOG.md'
    if (-not (Test-Path $defaultChangelog)) {
        return $null
    }
    $content = Get-Content -Path $defaultChangelog -Raw
    $escapedTag = [regex]::Escape($Tag)
    $pattern = "(?ms)^##\s+$escapedTag\s*(.+?)(?=^##\s+v|\Z)"
    $match = [regex]::Match($content, $pattern)
    if ($match.Success) {
        return "## $Tag`n" + $match.Groups[1].Value.Trim()
    }
    return $null
}

$branch = Get-GitBranch
if ($branch -ne 'main') {
    throw "Releases must be prepared from the main branch. Current branch: $branch"
}

$buildScript = Join-Path $projectRoot 'build.bat'
if (-not (Test-Path $buildScript)) {
    throw "Unable to find build.bat at $buildScript"
}

Write-Host "[i] Building GreenScope.exe..."
& $buildScript '--no-pause'

$exePath = Join-Path $projectRoot 'GreenScope.exe'
if (-not (Test-Path $exePath)) {
    throw "Build did not produce GreenScope.exe"
}

$distRoot = Join-Path $projectRoot 'dist'
if (-not (Test-Path $distRoot)) {
    New-Item -ItemType Directory -Path $distRoot | Out-Null
}

$payloadDirName = "GreenScope-$Version"
$payloadDir = Join-Path $distRoot $payloadDirName
if (Test-Path $payloadDir) {
    Write-Host "[i] Removing existing payload directory $payloadDir"
    Remove-Item -Recurse -Force $payloadDir
}
New-Item -ItemType Directory -Path $payloadDir | Out-Null

$iconDir = Join-Path $projectRoot 'icons'

$filesToCopy = @(
    @{ Source = $exePath; Target = 'GreenScope.exe' },
    @{ Source = Join-Path $projectRoot 'README.md'; Target = 'README.md' },
    @{ Source = Join-Path $projectRoot 'LICENSE'; Target = 'LICENSE' },
    @{ Source = Join-Path $iconDir 'greenscope.png'; Target = 'greenscope.png' },
    @{ Source = Join-Path $iconDir 'greenscope.ico'; Target = 'greenscope.ico' }
)

foreach ($file in $filesToCopy) {
    if (-not (Test-Path $file.Source)) {
        throw "Missing release asset: $($file.Source)"
    }
    Copy-Item -Path $file.Source -Destination (Join-Path $payloadDir $file.Target)
}

$zipPath = Join-Path $distRoot "$payloadDirName.zip"
if (Test-Path $zipPath) {
    Write-Host "[i] Removing existing archive $zipPath"
    Remove-Item -Force $zipPath
}

Write-Host "[i] Creating archive $zipPath"
Compress-Archive -Path (Join-Path $payloadDir '*') -DestinationPath $zipPath -Force

Write-Host "[OK] Release artifact created at $zipPath"

if ($Publish) {
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    if (-not $gh) {
        throw 'GitHub CLI (gh) is required to publish releases automatically. Install it or rerun without -Publish.'
    }

    $tagName = "v$Version"
    $releaseTitle = "GreenScope $tagName"

    if ($ReleaseNotesPath) {
        if (-not (Test-Path $ReleaseNotesPath)) {
            throw "Release notes file not found: $ReleaseNotesPath"
        }
        $ReleaseNotes = Get-Content -Path $ReleaseNotesPath -Raw
    }

    if (-not $ReleaseNotes) {
        $ReleaseNotes = Get-ChangelogSection -Tag $tagName
    }

    if (-not $ReleaseNotes) {
        $ReleaseNotes = "GreenScope $tagName release."
    }

    $notesPath = Join-Path $distRoot "release-notes-$Version.txt"
    Set-Content -Path $notesPath -Value ($ReleaseNotes.Trim() + [Environment]::NewLine)

    Write-Host "[i] Publishing $tagName via gh release create..."
    & gh release create $tagName $zipPath --title $releaseTitle --notes-file $notesPath
    Write-Host "[OK] Release published."
}
