# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

# Builds the release APK and signs it with the release key (the same key as Lemon).
#
#   powershell -ExecutionPolicy Bypass -File tools\sign_release.ps1
#
# Run it yourself: it asks for the password of lemon-release.jks once. The password only lives in
# this PowerShell process and is cleared at the end; nothing is printed or saved.
$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
$env:JAVA_HOME = 'C:\Users\LeYJa\.jdks\temurin-17.0.19'
$apksigner = "$env:LOCALAPPDATA\Android\Sdk\build-tools\36.1.0\apksigner.bat"
$keystore  = 'C:\Users\LeYJa\lemon-keys\lemon-release.jks'
$alias     = 'lemon'

$gradle = Get-Content "$Root\android\app\build.gradle.kts" -Raw
$version = [regex]::Match($gradle, 'versionName = "([^"]+)"').Groups[1].Value
$dist = "$Root\dist"
$out  = "$dist\CapyPS5-v$version.apk"

Write-Host "== Building native library and debug APK"
& powershell -ExecutionPolicy Bypass -File "$Root\tools\build_android.ps1" -NoInstall
if ($LASTEXITCODE) { throw 'native build failed' }

Write-Host "== Packaging release APK (unsigned)"
Push-Location "$Root\android"
try {
    & .\gradlew.bat assembleRelease --console=plain -q
    if ($LASTEXITCODE) { throw 'gradle assembleRelease failed' }
} finally { Pop-Location }
$unsigned = "$Root\android\app\build\outputs\apk\release\app-release-unsigned.apk"
if (-not (Test-Path $unsigned)) { throw "missing $unsigned" }

New-Item -ItemType Directory -Force $dist | Out-Null
$secure = Read-Host -AsSecureString 'Password of lemon-release.jks'
$env:CAPY_KS_PASS = [Runtime.InteropServices.Marshal]::PtrToStringAuto(
    [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure))
try {
    Write-Host "== Signing $out"
    & $apksigner sign --ks $keystore --ks-key-alias $alias `
        --ks-pass env:CAPY_KS_PASS --key-pass env:CAPY_KS_PASS --out $out $unsigned
    if ($LASTEXITCODE) { throw 'apksigner sign failed' }
}
finally {
    Remove-Item Env:CAPY_KS_PASS -ErrorAction SilentlyContinue
}
& $apksigner verify --print-certs $out | Select-String 'Signer #1 certificate (DN|SHA-256)'
Write-Host "== Done: $out"
