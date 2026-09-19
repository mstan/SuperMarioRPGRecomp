[CmdletBinding()]
param(
    [string]$DirectRomPath,
    [string]$RuntimeBin = 'C:\msys64\mingw64\bin',
    [switch]$CheckOnly,
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
$rendererRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$rendererBuild = Join-Path $rendererRoot 'build-custom'
$rendererSource = Join-Path $rendererBuild 'SuperMarioRPGSNESRecomp.exe'
$rendererData = Join-Path $rendererBuild 'playtest'
$rendererExe = Join-Path $rendererData 'SuperMarioRPGSNESRecomp.exe'
if (-not (Test-Path -LiteralPath $rendererSource -PathType Leaf)) {
    throw "Build the renderer first: $rendererSource"
}
if (-not (Test-Path -LiteralPath $RuntimeBin -PathType Container)) {
    throw "Runtime DLL directory is missing: $RuntimeBin"
}
$rendererArguments = @()
if ($DirectRomPath) {
    $rendererArguments += (Resolve-Path -LiteralPath $DirectRomPath).Path
}
if ($CheckOnly) {
    Write-Output "Executable: $rendererExe"
    Write-Output "Settings and saves: $rendererData"
    Write-Output "Runtime DLLs: $RuntimeBin"
    return
}
[void](New-Item -ItemType Directory -Path $rendererData -Force)
Copy-Item -LiteralPath $rendererSource -Destination $rendererExe -Force
Copy-Item -LiteralPath (Join-Path $rendererBuild 'assets') -Destination $rendererData -Recurse -Force
$rendererMods = Join-Path $rendererData 'mods\preloaded'
[void](New-Item -ItemType Directory -Path $rendererMods -Force)
Copy-Item -LiteralPath (Join-Path $rendererRoot 'mods\preloaded\packages') `
    -Destination $rendererMods -Recurse -Force
$rendererState = Join-Path $rendererMods 'state.toml'
if (-not (Test-Path -LiteralPath $rendererState)) {
    @'
format_version = 1
[[package]]
id = "super-mario-rpg.enhancement.widescreen"
version = "1.0.0"
[[feature]]
package_id = "super-mario-rpg.enhancement.widescreen"
id = "widescreen"
enabled = true
[feature.values]
aspect = "fit"
'@ | Set-Content -LiteralPath $rendererState -Encoding ASCII
}
if (-not $DirectRomPath -and -not (Test-Path -LiteralPath (Join-Path $rendererData 'smrpg.sfc'))) {
    $rendererRom = Join-Path $rendererRoot 'smrpg.sfc'
    if (Test-Path -LiteralPath $rendererRom) {
        Copy-Item -LiteralPath $rendererRom -Destination (Join-Path $rendererData 'smrpg.sfc')
    }
}
if ($PrepareOnly) { Write-Output "Prepared: $rendererData"; return }
$rendererSavedPath = $env:PATH
Push-Location -LiteralPath $rendererData
try {
    $env:PATH = "$RuntimeBin;$rendererSavedPath"
    $rendererStart = @{
        FilePath = $rendererExe
        WorkingDirectory = $rendererData
        WindowStyle = 'Hidden'
        PassThru = $true
        Wait = $true
        RedirectStandardOutput = (Join-Path $rendererData 'stdout.log')
        RedirectStandardError = (Join-Path $rendererData 'stderr.log')
    }
    if ($rendererArguments.Count) {
        $rendererStart.ArgumentList = ($rendererArguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    }
    $rendererProcess = Start-Process @rendererStart
    if ($rendererProcess.ExitCode -ne 0) {
        throw "Renderer exited with $($rendererProcess.ExitCode); see $rendererData\stderr.log"
    }
} finally {
    $env:PATH = $rendererSavedPath
    Pop-Location
}
