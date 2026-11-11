$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$items = @()
foreach ($name in @('logs','saves','config.ini','keybinds.ini','last_run_report.json','mods/preloaded/state.toml')) {
  $path = Join-Path $root $name
  if (Test-Path -LiteralPath $path) { $items += $path }
}
$items += @(Get-ChildItem -LiteralPath $root -File | Where-Object { $_.Name -like 'crash_report_*.json' -or $_.Name -like 'crash_minidump_*.dmp' } | Select-Object -ExpandProperty FullName)
if (-not $items.Count) { throw 'No diagnostic files found. Run Start diagnostics first.' }
$destination = Join-Path $root ('smrpg-diagnostics-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.zip')
if (Test-Path -LiteralPath $destination) { throw 'This collection filename already exists; wait a second and retry.' }
Compress-Archive -LiteralPath $items -DestinationPath $destination -CompressionLevel Optimal
Write-Output "Created $destination"
Write-Output 'This archive contains game memory, settings and saves. Nothing was uploaded. Keep your originals.'
