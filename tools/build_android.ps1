# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

# Builds libcapyps5.so, packages the debug APK and installs it on the device.
#
#   powershell -ExecutionPolicy Bypass -File tools\build_android.ps1 [-NoInstall] [-Device 282f344c]
#
# Paths default to this machine's setup; override with the environment variables below.
param(
    [switch]$NoInstall,
    # Empty device: adb installs to the single connected device (no -s flag).
    [string]$Device = $(if ($env:CAPY_DEVICE) { $env:CAPY_DEVICE } else { "" })
)
$ErrorActionPreference = "Stop"

$Root   = Split-Path -Parent $PSScriptRoot
$Sdk    = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { "$env:LOCALAPPDATA\Android\Sdk" }
$Ndk    = "$Sdk\ndk\28.2.13676358"
$CMake  = "$Sdk\cmake\3.31.6\bin"
$Ninja  = if ($env:CAPY_NINJA_DIR) { $env:CAPY_NINJA_DIR } else { "$env:LOCALAPPDATA\Python\pythoncore-3.14-64\Scripts" }
$Git    = "$env:ProgramFiles\Git\cmd\git.exe"
$Build  = "$Root\build-android-arm64"
$JniDir = "$Root\android\app\src\main\jniLibs\arm64-v8a"
$Strip  = "$Ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
$LibCxx = "$Ndk\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so"
# Ninja must come first: the SDK's cmake/bin ships an older ninja whose log format differs,
# and alternating between the two discards the build log and forces a full rebuild.
$env:PATH = "$Ninja;$CMake;$env:PATH"
if (-not $env:JAVA_HOME) {
    # Prefer the JBR of any installed Android Studio; the old hardcoded path was one
    # machine's setup.
    $Jbr = Get-ChildItem "$env:ProgramFiles\Android" -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "jbr" } | Where-Object { Test-Path (Join-Path $_ "bin\java.exe") } |
        Select-Object -First 1
    if ($Jbr) { $env:JAVA_HOME = $Jbr } else { $env:JAVA_HOME = "C:\Program Files\Android\Android Studio2\jbr" }
}

if (-not (Test-Path "$Build\build.ninja")) {
    Write-Host "== Configuring native build"
    $FetchSources = @()
    if ($env:CAPY_FETCH_DEPS_ROOT) {
        foreach ($Name in @('opus', 'xbyak', 'zydis', 'zstd', 'zarchive_source')) {
            $FetchSources += "-DFETCHCONTENT_SOURCE_DIR_$($Name.ToUpper())=$env:CAPY_FETCH_DEPS_ROOT\$Name-src"
        }
    }
    if ($env:CAPY_FFMPEG_PREBUILT_DIR) {
        $FetchSources += "-DFFMPEG_PREBUILT_DIR=$env:CAPY_FFMPEG_PREBUILT_DIR"
    }
    & "$CMake\cmake.exe" -S "$Root\core" -B $Build -G Ninja `
        "-DCMAKE_TOOLCHAIN_FILE=$Ndk\build\cmake\android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DCMAKE_BUILD_TYPE=Release `
        -DKYTY_BUILD_LAUNCHER=OFF "-DGIT_EXECUTABLE=$Git" $FetchSources
    if ($LASTEXITCODE) { throw "cmake configure failed" }
}

Write-Host "== Building libcapyps5.so"
& "$Ninja\ninja.exe" -C $Build capyps5
if ($LASTEXITCODE) { throw "native build failed" }

# Debug info makes the library ~280 MB; keep the symbol table for readable tombstones.
New-Item -ItemType Directory -Force $JniDir | Out-Null
& $Strip --strip-debug -o "$JniDir\libcapyps5.so" "$Build\libcapyps5.so"
if ($LASTEXITCODE) { throw "strip failed" }
Copy-Item $LibCxx "$JniDir\libc++_shared.so" -Force
# libadrenotools hooks must sit in nativeLibraryDir to load custom GPU drivers.
foreach ($Hook in @('main_hook', 'file_redirect_hook', 'gsl_alloc_hook', 'hook_impl')) {
    $HookLib = Get-ChildItem -Path $Build -Recurse -Filter "lib$Hook.so" | Select-Object -First 1
    if (-not $HookLib) { throw "missing lib$Hook.so" }
    & $Strip --strip-debug -o "$JniDir\lib$Hook.so" $HookLib.FullName
}

Write-Host "== Packaging APK"
Push-Location "$Root\android"
try {
    & .\gradlew.bat assembleDebug --console=plain -q
    if ($LASTEXITCODE) { throw "gradle build failed" }
} finally { Pop-Location }

$Apk = "$Root\android\app\build\outputs\apk\debug\app-debug.apk"
Write-Host ("APK: {0} ({1:N1} MB)" -f $Apk, ((Get-Item $Apk).Length / 1MB))

if (-not $NoInstall) {
    if ($Device) {
        Write-Host "== Installing on $Device"
        & adb -s $Device install -r $Apk
    } else {
        Write-Host "== Installing on the connected device"
        & adb install -r $Apk
    }
    if ($LASTEXITCODE) { throw "adb install failed" }
}
