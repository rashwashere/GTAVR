# GTAVive.asi trace-equivalence test.
#
# Builds stub ScriptHookV.dll / OVRInject.dll that log every native call,
# then runs a REFERENCE GTAVive.asi (for example the original alpha release)
# and the REBUILT one through identical simulated frame sequences
# (7 scenarios) and diffs the traces.
#
# Usage:  .\run_traces.ps1 -OriginalAsi <reference .asi> [-RebuiltAsi <path>]
#
# Against the original alpha build, the expected result is: all scenarios
# IDENTICAL except two known 1-ULP rounding differences in the "fire"
# scenario's shotgun spread (VS2015 vs VS2022 floating-point instruction
# scheduling; ~0.15mm at 120m).
param(
    [Parameter(Mandatory = $true)] [string]$OriginalAsi,
    [string]$RebuiltAsi = "$PSScriptRoot\..\..\x64\Release\GTAVive.asi"
)
$ErrorActionPreference = "Stop"
$h = $PSScriptRoot

$vcvars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$h`" && cl /nologo /O2 /MT /LD shv_stub.cpp /link /OUT:ScriptHookV.dll && cl /nologo /O2 /MT /LD ovr_stub.cpp /link /OUT:OVRInject.dll && cl /nologo /O2 /MT harness.cpp /link /OUT:harness.exe" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "stub build failed" }

foreach ($d in @("$h\run_orig", "$h\run_new")) {
    New-Item -ItemType Directory -Force $d | Out-Null
    Copy-Item "$h\ScriptHookV.dll", "$h\OVRInject.dll", "$h\harness.exe" $d -Force
}
Copy-Item $OriginalAsi "$h\run_orig\GTAVive.asi" -Force
Copy-Item $RebuiltAsi "$h\run_new\GTAVive.asi" -Force

$scenarios = @("idle", "vehicle", "fire", "teleport", "teleport_veh", "melee", "fly")
$totalDiffs = 0
foreach ($s in $scenarios) {
    $env:GTAVIVE_SCENARIO = $s
    Push-Location "$h\run_orig"; & .\harness.exe | Out-Null; Pop-Location
    Push-Location "$h\run_new";  & .\harness.exe | Out-Null; Pop-Location

    $a = Get-Content "$h\run_orig\native_trace.log"
    $b = Get-Content "$h\run_new\native_trace.log"
    $diffs = @()
    $max = [Math]::Max($a.Count, $b.Count)
    for ($i = 0; $i -lt $max; $i++) {
        if ($a[$i] -ne $b[$i]) { $diffs += "  line $($i+1): orig='$($a[$i])' new='$($b[$i])'" }
    }
    $totalDiffs += $diffs.Count
    $status = if ($diffs.Count -eq 0) { "IDENTICAL" } else { "$($diffs.Count) DIFFS" }
    "{0,-14} {1,5} lines  {2}" -f $s, $a.Count, $status
    $diffs | Select-Object -First 12
}
"`ntotal diffs: $totalDiffs (2 expected: shotgun-spread ULP rounding in 'fire')"
