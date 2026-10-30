<#
Builds everything a release needs into dist\ and, with -Publish, uploads it as a GitHub release.

    powershell -ExecutionPolicy Bypass -File tools\release.ps1            # build only
    powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Publish   # build + GitHub release v<VERSION>

dist\ gets: WinDrop-Setup.exe (Windows installer), WinDrop.apk (Android), WinDrop.zip (both + install notes).
Asset names carry no version, so .../releases/latest/download/<name> links always point at the newest.

Needs: Visual Studio 2022 Build Tools (C++ + Windows SDK), Android Studio (for its JDK) or JAVA_HOME,
Inno Setup 6, and gh (logged in) for -Publish. The APK is signed with the key described in README.
#>
param([switch]$Publish)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$version = (Get-Content VERSION -Raw).Trim()

# CMake and Gradle both read VERSION, so the exe, the installer and the APK always agree.
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "VERSION must look like 1.2.3 (got '$version')" }

function Find-First($paths) { foreach ($p in $paths) { if ($p -and (Test-Path $p)) { return $p } }; throw "not found: $($paths -join ', ')" }

Write-Host "== Windows app $version"
$vcvars = Find-First @(
  "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
  "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat")
cmd /c "`"$vcvars`" >nul && cmake -S windows -B windows\build-msvc -G `"NMake Makefiles`" -DCMAKE_BUILD_TYPE=Release && cmake --build windows\build-msvc"
if ($LASTEXITCODE) { throw "Windows build failed" }

Write-Host "== Android app $version"
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = Find-First @("$env:ProgramFiles\Android\Android Studio\jbr") }
Push-Location android
.\gradlew.bat --quiet assembleRelease
$ok = $LASTEXITCODE -eq 0
# The Gradle daemon outlives the build and keeps this script's output pipe open, so a caller
# piping our output would wait on it forever.
.\gradlew.bat --quiet --stop | Out-Null
Pop-Location
if (-not $ok) { throw "Android build failed" }

Write-Host "== Installer"
$iscc = Find-First @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe")
if (Test-Path dist) { Remove-Item dist -Recurse -Force }
New-Item -ItemType Directory dist | Out-Null
& $iscc /Q "/DAppVersion=$version" "/DExe=$root\windows\build-msvc\WinDrop.exe" installer\WinDrop.iss
if ($LASTEXITCODE) { throw "Inno Setup failed" }

Copy-Item android\app\build\outputs\apk\release\app-release.apk dist\WinDrop.apk
Copy-Item "installer\How to install.txt" dist\
Compress-Archive -Path dist\WinDrop-Setup.exe, dist\WinDrop.apk, "dist\How to install.txt" -DestinationPath dist\WinDrop.zip
Get-ChildItem dist | Format-Table Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 2) } }

if ($Publish) {
  $tag = "v$version"
  $notes = @"
Downloads:

- Windows: [WinDrop-Setup.exe](https://github.com/Skyblock127/WinDrop/releases/download/$tag/WinDrop-Setup.exe)
- Android: [WinDrop.apk](https://github.com/Skyblock127/WinDrop/releases/download/$tag/WinDrop.apk)
- Both: [WinDrop.zip](https://github.com/Skyblock127/WinDrop/releases/download/$tag/WinDrop.zip)

Install the exe on the PC and the apk on the phone, then pair: on the PC, tray icon, Pair phone; on the phone, Pair with PC. Setup details are in the [README](https://github.com/Skyblock127/WinDrop#setup).
"@
  $ErrorActionPreference = 'Continue'  # "release not found" on stderr is an answer here, not a failure
  gh release view $tag *> $null
  $exists = $LASTEXITCODE -eq 0
  $ErrorActionPreference = 'Stop'
  if ($exists) {
    gh release upload $tag dist\WinDrop-Setup.exe dist\WinDrop.apk dist\WinDrop.zip --clobber
  } else {
    gh release create $tag dist\WinDrop-Setup.exe dist\WinDrop.apk dist\WinDrop.zip --title "WinDrop $version" --notes $notes
  }
  if ($LASTEXITCODE) { throw "gh release failed" }
}
