# Builds OVRInject.dll and GTAVive.asi (Release x64) into x64\Release.
# Nothing is copied to the game folder.
#
# Needs Visual Studio 2022 (MSVC v143 + Windows 10 SDK) and three import
# libraries in ThirdParty, which are not part of the repository:
#   ScriptHookV.lib      from the ScriptHookV SDK
#   openvr_api.lib       from the OpenVR SDK (x64)
#   libMinHook.x64.lib   built from ThirdParty\minhook
# If ScriptHookV.lib or openvr_api.lib is missing but a matching .def file is
# in ThirdParty, the import library is generated from it.
$ErrorActionPreference = 'Stop'

$repo = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1

$thirdParty = Join-Path $repo 'ThirdParty'
foreach ($name in 'ScriptHookV', 'openvr_api') {
    $lib = Join-Path $thirdParty "$name.lib"
    $def = Join-Path $thirdParty "$name.def"
    if (-not (Test-Path $lib) -and (Test-Path $def)) {
        $libexe = & $vswhere -latest -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\lib.exe' | Select-Object -First 1
        & $libexe /nologo /machine:x64 "/def:$def" "/out:$lib"
        if ($LASTEXITCODE -ne 0) { throw "could not generate $name.lib" }
    }
}

foreach ($project in 'OVRInject\OVRInject.vcxproj', 'GTAVive\GTAVive.vcxproj') {
    # SolutionDir needs its trailing slash; doubled so it can't escape the quote
    & $msbuild (Join-Path $repo $project) /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$repo\\" /v:m /nologo
    if ($LASTEXITCODE -ne 0) { throw "$project failed to build" }
}

Get-ChildItem (Join-Path $repo 'x64\Release') -Include OVRInject.dll, GTAVive.asi -Recurse |
    Select-Object Length, LastWriteTime, FullName
