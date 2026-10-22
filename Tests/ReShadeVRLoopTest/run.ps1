# Assembles an isolated test folder (ReShade + chosen add-ons + stub OpenVR +
# harness) and runs vrlooptest.exe in it. Nothing in the game folder is written:
# the shaders are read from it, everything else is a COPY.
#
#   .\run.ps1 -Game <GTA V Enhanced folder>                    # the add-ons installed in the game
#   .\run.ps1 -Game <folder> -Addons none                      # ReShade alone, no add-ons
#   .\run.ps1 -Game <folder> -Addons custom -Only a.addon64    # only the named add-on files
#   .\run.ps1 -Game <folder> -Harness .\bin\vrlooptest_fixed.exe   # the build with OpenVRPrivateLoad
param(
  [Parameter(Mandatory = $true)] [string]$Game,   # folder with ReShade's dxgi.dll, ReShade.ini and reshade-shaders
  [ValidateSet('current','none','custom')] [string]$Addons = 'current',
  [string[]]$Only = @(),          # with -Addons custom: exact game-folder file names to include
  [int]$Seconds = 25,
  [string]$Harness = "$PSScriptRoot\bin\vrlooptest.exe",
  [string]$Stub = "$PSScriptRoot\bin\openvr_api.dll",
  [string]$Work = "$env:TEMP\ReShadeVRLoopTest"
)
$ErrorActionPreference = 'Stop'
$dir = Join-Path $Work $Addons
if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
New-Item -ItemType Directory -Force $dir | Out-Null

Copy-Item "$Game\dxgi.dll" $dir                       # ReShade proxy
Copy-Item $Harness (Join-Path $dir 'vrlooptest.exe')
Copy-Item $Stub (Join-Path $dir 'openvr_api.dll')

# Local COPY of the active preset, so preset auto-save never touches the real one
$preset = (Get-Content "$Game\ReShade.ini" | Where-Object { $_ -like 'PresetPath=*' } | Select-Object -First 1) -replace '^PresetPath=', ''
$localPreset = ''
if ($preset) {
  $src = if ([IO.Path]::IsPathRooted($preset)) { $preset } else { Join-Path $Game $preset }
  if (Test-Path $src) { $localPreset = Join-Path $dir (Split-Path $src -Leaf); Copy-Item $src $localPreset }
}

$shaders = "$Game\reshade-shaders\Shaders\**"
$textures = "$Game\reshade-shaders\Textures\**"
foreach ($ini in 'ReShade.ini','ReShadeVR.ini') {
  if (-not (Test-Path "$Game\$ini")) { continue }
  $lines = Get-Content "$Game\$ini" | ForEach-Object {
    if     ($_ -like 'EffectSearchPaths=*')     { "EffectSearchPaths=$shaders" }
    elseif ($_ -like 'TextureSearchPaths=*')    { "TextureSearchPaths=$textures" }
    elseif ($_ -like 'IntermediateCachePath=*') { "IntermediateCachePath=$dir\cache" }
    elseif ($_ -like 'PresetPath=*')            { "PresetPath=$localPreset" }
    elseif ($_ -like 'StartupPresetPath=*')     { "StartupPresetPath=" }
    else { $_ }
  }
  Set-Content -Path (Join-Path $dir $ini) -Value $lines -Encoding ascii
}

switch ($Addons) {
  'current' {
    Get-ChildItem $Game -File | Where-Object { $_.Name -match '\.addon(64)?$' } | Copy-Item -Destination $dir
  }
  'none' { }
  'custom' { foreach ($n in $Only) { Copy-Item (Join-Path $Game $n) $dir } }
}

"--- test folder: $dir"
"--- add-ons: " + ((Get-ChildItem $dir -File | Where-Object { $_.Name -match '\.addon(64)?$' } | ForEach-Object Name) -join ', ')
Push-Location $dir
$p = Start-Process -FilePath (Join-Path $dir 'vrlooptest.exe') -ArgumentList $Seconds -PassThru -NoNewWindow -RedirectStandardOutput "$dir\stdout.txt" -Wait
Pop-Location
$code = $p.ExitCode
$hex = '0x{0:X8}' -f [uint32](([int64]$code + 4294967296) % 4294967296)
Get-Content "$dir\stdout.txt" -ErrorAction SilentlyContinue
$verdict = switch ($hex) {
  '0x00000000' { 'PASS - survived the full run' }
  '0xC00000FD' { 'STACK OVERFLOW - the add-on event loop reproduced' }
  '0x00000002' { 'FAIL - ran, but too few submits' }
  default      { "DIED - exit $hex" }
}
"=== RESULT [$Addons]: exit $hex -> $verdict"
if (Test-Path "$dir\stub_openvr.log") { "--- stub_openvr.log:"; Get-Content "$dir\stub_openvr.log" | Select-Object -Unique }
if (Test-Path "$dir\ReShade.log") { "--- ReShade.log VR lines:"; Select-String -Path "$dir\ReShade.log" -Pattern 'ReShadeVR|VR_Init|IVRCompositor|Submit' | Select-Object -First 6 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(170, $_.Line.Length)) } }
