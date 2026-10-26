# SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
# SPDX-License-Identifier: GPL-2.0-only

# Builds the release APK and signs it with a release key.
# In this fork the release key lives in the repository's CI secrets; locally, the
# keystore path, key alias and apksigner binary MUST come from the environment:
#
#   CAPY_KEYSTORE   path to your own keystore (.jks/.keystore)   (required)
#   CAPY_KEY_ALIAS  key alias inside that keystore              (required)
#   CAPY_APKSIGNER  path to the apksigner binary                 (required)
#   CAPY_JAVA_HOME  JDK to build with                            (optional)
#
#   powershell -ExecutionPolicy Bypass -File tools\sign_release.ps1
#
# Run it yourself: it asks for the keystore password once. The password only lives in
# this PowerShell process and is cleared at the end; nothing is printed or saved.
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
if ($env:CAPY_JAVA_HOME) { $env:JAVA_HOME = $env:CAPY_JAVA_HOME }
$apksigner = $env:CAPY_APKSIGNER
$keystore  = $env:CAPY_KEYSTORE
$alias     = $env:CAPY_KEY_ALIAS
if (-not $apksigner) { throw 'set CAPY_APKSIGNER to your apksigner binary' }
if (-not $keystore)  { throw 'set CAPY_KEYSTORE to your release keystore' }
if (-not $alias)     { throw 'set CAPY_KEY_ALIAS to the key alias' }

$gradle = Get-Content "$Root\android\app\build.gradle.kts" -Raw
$version = [regex]::Match($gradle, 'versionName = "([^"]+)"').Groups[1].Value
$dist = "$Root\dist"
$out  = "$dist\ConsoleFivePro-v$version.apk"

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
$secure = Read-Host -AsSecureString "Password of $keystore"
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
