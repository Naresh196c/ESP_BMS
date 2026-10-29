param([switch]$Link)
$taskRoot = Split-Path $PSScriptRoot -Parent
$validationRoot = Join-Path $env:TEMP 'balisto-validation'
New-Item -ItemType Directory -Path $validationRoot -Force | Out-Null
$database = Get-Content -Raw -LiteralPath 'C:\Users\chowd\AppData\Local\arduino\sketches\42E87C1E6C4F19F42CDB9BE1C358AA62\compile_commands.json' | ConvertFrom-Json
$entry = $database | Where-Object { $_.file.EndsWith('.ino.cpp') } | Select-Object -First 1
if (!$entry) { throw 'No saved sketch compilation command found' }
$sourcePath = Join-Path $validationRoot 'sketch-check.cpp'
$objectPath = Join-Path $validationRoot 'sketch-check.o'
$sketch = Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'Balisto_Live_BMS_Dashboard_Names.ino')
[IO.File]::WriteAllText($sourcePath, "#include <Arduino.h>`n" + $sketch)
$compiler = $entry.arguments[0]
$compilerArguments = @($entry.arguments | Select-Object -Skip 1)
for ($i = 0; $i -lt $compilerArguments.Count; $i++) {
  if ($compilerArguments[$i] -eq $entry.file) { $compilerArguments[$i] = $sourcePath }
  if ($compilerArguments[$i] -eq '-o') { $compilerArguments[$i + 1] = $objectPath }
}
& $compiler @compilerArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output 'PASS: current sketch compiled into an ESP32 object using the saved Arduino 3.3.9 board flags.'


if ($Link) {
  $savedRoot = Split-Path (Split-Path $entry.file -Parent) -Parent
  $mapPath = Join-Path $savedRoot 'Balisto_Live_BMS_Dashboard_Names.ino.map'
  $savedMap = Get-Content -Raw -LiteralPath $mapPath
  $corePath = [regex]::Match($savedMap, 'C:[^\r\n]*?[\\/]core\.a').Value
  if (!(Test-Path -LiteralPath $corePath)) { throw 'Matching cached Arduino core archive not found' }
  $linkLine = Get-Content -LiteralPath (Join-Path $taskRoot 'build-r4.log') | Where-Object { $_.Contains('--start-group') -and $_.Contains(' -o ') -and $_.Contains('.elf') } | Select-Object -Last 1
  if (!$linkLine) { throw 'Saved Arduino link recipe not found' }
  $linkTokens = @([regex]::Matches($linkLine, '"([^"]*)"|(\S+)') | ForEach-Object {
    if ($_.Groups[1].Success) { $_.Groups[1].Value.Replace('\\','\') }
    else { $_.Groups[2].Value.Replace('\\','\') }
  })
  $oldRoot = 'C:\Users\chowd\AppData\Local\Temp\balisto-research-build'
  $linkArguments = @($linkTokens | Select-Object -Skip 1 | ForEach-Object { $_.Replace($oldRoot,$savedRoot) })
  $elfPath = Join-Path $validationRoot 'balisto-r7.elf'
  for ($i = 0; $i -lt $linkArguments.Count; $i++) {
    if ($linkArguments[$i].EndsWith('Balisto_Live_BMS_Dashboard_Names.ino.cpp.o')) { $linkArguments[$i] = $objectPath }
    if ($linkArguments[$i].EndsWith('core.a')) { $linkArguments[$i] = $corePath }
    if ($linkArguments[$i].StartsWith('-Wl,--Map=')) { $linkArguments[$i] = '-Wl,--Map=' + (Join-Path $validationRoot 'balisto-r7.map') }
    if ($linkArguments[$i] -eq '-o') { $linkArguments[$i + 1] = $elfPath }
    if ($linkArguments[$i].EndsWith('.o') -and !(Test-Path -LiteralPath $linkArguments[$i])) { throw ('Cached object missing: ' + $linkArguments[$i]) }
  }
  & $compiler @linkArguments
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  Write-Output 'PASS: final R7 firmware linked with the matching cached Arduino 3.3.9 core and library objects.'
  & (Join-Path (Split-Path $compiler -Parent) 'xtensa-esp32-elf-size.exe') $elfPath
  exit $LASTEXITCODE
}
