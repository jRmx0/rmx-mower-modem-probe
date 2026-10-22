# Build a signed probe image and upload it to the VPS for OTA.
#
#   tools\release.ps1                 # builds version.txt's version, uploads, prints the ota command
#   tools\release.ps1 -Send           # ... and sends it to the device (tools\mqtt_cmd.ps1 ota)
#
# Needs the ESP-IDF environment (. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1),
# the signing key in keys\ota_signing_key.pem (README "Firmware updates") and `ssh rmx-vps`.
# Bump version.txt first: the device refuses the version it already runs.
param(
    [switch]$Send,
    [string]$Device = 'probe-device',
    [string]$RemoteDir = '/opt/rmx-vps/services/caddy/firmware/probe'
)
$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot '..' | Resolve-Path
Push-Location $root
try {
    if (-not (Test-Path keys\ota_signing_key.pem)) { throw 'no keys\ota_signing_key.pem (see README "Firmware updates")' }
    $version = (Get-Content version.txt -Raw).Trim()
    idf.py build
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    $bin = 'build\rmx_mower_modem_probe.bin'
    $size = (Get-Item $bin).Length
    $sha = (Get-FileHash $bin -Algorithm SHA256).Hash.ToLower()
    Write-Host "image $version, $size bytes, sha256 $sha"

    # New SSH connections to the VPS are sometimes dropped: retry a few times.
    $ok = $false
    for ($try = 1; $try -le 3 -and -not $ok; $try++) {
        if ($try -gt 1) { Write-Host "upload failed, retrying in 20 s ($try/3)"; Start-Sleep 20 }
        ssh -o BatchMode=yes -o ConnectTimeout=15 rmx-vps "mkdir -p $RemoteDir"
        if ($LASTEXITCODE -eq 0) {
            scp -o BatchMode=yes -o ConnectTimeout=15 $bin "rmx-vps:$RemoteDir/$version.bin"
            $ok = $LASTEXITCODE -eq 0
        }
    }
    if (-not $ok) { throw 'upload to rmx-vps failed (3 tries)' }
    Write-Host "uploaded: https://api.rmx.lt/fw/probe/$version.bin"

    $cmd = "tools\mqtt_cmd.ps1 ota -Version $version -Size $size -Device $Device"
    if ($Send) {
        & (Join-Path $PSScriptRoot 'mqtt_cmd.ps1') ota -Version $version -Size $size -Device $Device
        Write-Host 'the device acks "started", downloads, acks "done" and restarts; check with: tools\mqtt_cmd.ps1 status'
    } else {
        Write-Host "send it with: $cmd"
    }
} finally {
    Pop-Location
}
