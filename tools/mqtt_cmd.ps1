# Send a remote command to the probe device and print its reply.
#
#   tools\mqtt_cmd.ps1 ping
#   tools\mqtt_cmd.ps1 status
#   tools\mqtt_cmd.ps1 create -Value "hello from the PC"
#   tools\mqtt_cmd.ps1 delete -RecordId 31
#   tools\mqtt_cmd.ps1 https_create -Value "via HTTPS"
#   tools\mqtt_cmd.ps1 create -Gps          # value = the device's GPS location (or why there is none)
#   tools\mqtt_cmd.ps1 https_create -Gps
#   tools\mqtt_cmd.ps1 list | https_list | restart
#   tools\mqtt_cmd.ps1 ota -Version 0.2.1 -Size 851234   # url = $OtaBase/<version>.bin (tools\release.ps1 prints this)
#
# Publishes {"id","cmd","value","gps","record_id","url","version","size","expires_at"} on mower/<device>/cmd as the
# MQTT login "app" (the backend role: write mower/#) and waits for the answer on
# mower/<device>/cmd/ack. Needs the Mosquitto clients for Windows and `ssh rmx-vps`.
# The app password is read from the server into memory (or from $env:RMX_APP_MQTT_PW)
# and never printed. On 8883 the client id is forced to "app": a running backend
# using that login would be disconnected while this runs.
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Cmd,
    [string]$Value,
    [long]$RecordId = -1,
    [switch]$Gps,             # "gps": true (create / https_create)
    [string]$Version,         # ota: the image version (must match its header)
    [string]$Url,             # ota: image URL (default $OtaBase/<Version>.bin)
    [long]$Size = 0,          # ota: image size in bytes (0 = not checked)
    [string]$OtaBase = 'https://api.rmx.lt/fw/probe',
    [int]$TtlS = 60,          # expires_at = now + TtlS (0 = no expiry, negative = already expired, for tests)
    [int]$TimeoutS = 45,
    [string]$Device = 'probe-device'
)
$ErrorActionPreference = 'Stop'
$rr = 'C:\Program Files\mosquitto\mosquitto_rr.exe'
$ca = Join-Path $PSScriptRoot '..\certs\rmx-ca-bundle.pem' | Resolve-Path

$pw = $env:RMX_APP_MQTT_PW
if (-not $pw) {
    # No double quotes in the remote command: Windows PowerShell 5.1 strips them from native args.
    # New SSH connections to the VPS are sometimes dropped: retry a few times.
    for ($try = 1; $try -le 3 -and -not $pw; $try++) {
        if ($try -gt 1) { Write-Host "ssh failed, retrying in 20 s ($try/3)"; Start-Sleep 20 }
        $pw = ssh -o BatchMode=yes -o ConnectTimeout=15 rmx-vps 'sudo awk ''/^app /{print $2}'' /opt/rmx-vps/services/mosquitto/data/credentials.txt'
    }
    if (-not $pw) { throw 'could not read the app password over ssh rmx-vps (3 tries)' }
}

$id = 'pc-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$msg = [ordered]@{ id = $id; cmd = $Cmd }
if ($PSBoundParameters.ContainsKey('Value')) { $msg.value = $Value }
if ($Gps) { $msg.gps = $true }
if ($RecordId -ge 0) { $msg.record_id = $RecordId }
if ($Cmd -eq 'ota') {
    if (-not $Version) { throw 'ota needs -Version' }
    $msg.url = if ($Url) { $Url } else { "$OtaBase/$Version.bin" }
    $msg.version = $Version
    if ($Size -gt 0) { $msg.size = $Size }
}
if ($TtlS -ne 0) { $msg.expires_at = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + $TtlS }
$json = $msg | ConvertTo-Json -Compress

$out = New-TemporaryFile; $err = New-TemporaryFile
# mosquitto_rr has no -f; pass the JSON with -m, quotes escaped for the Windows command line
$a = @('-h', 'mqtt.rmx.lt', '-p', '8883', '--cafile', "`"$ca`"", '-V', '5', '-q', '1',
       '-u', 'app', '-P', $pw, '-t', "mower/$Device/cmd", '-e', "mower/$Device/cmd/ack",
       '-m', ('"' + ($json -replace '(\\*)"', '$1$1\"') + '"'), '-W', $TimeoutS)
Write-Host "-> mower/$Device/cmd $json"
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process $rr -ArgumentList $a -RedirectStandardOutput $out -RedirectStandardError $err -NoNewWindow -PassThru
# mosquitto's -W timeout is unreliable on Windows: enforce it here too
if (-not $p.WaitForExit(($TimeoutS + 5) * 1000)) { $p.Kill(); $p.WaitForExit() }
$reply = (Get-Content $out -Raw)
$e = (Get-Content $err -Raw)
Remove-Item $out, $err -ErrorAction SilentlyContinue
Remove-Variable pw

if ($reply) {
    Write-Host "<- mower/$Device/cmd/ack ($($sw.ElapsedMilliseconds) ms round trip)"
    try { $reply | ConvertFrom-Json | ConvertTo-Json -Depth 5 } catch { $reply }
    $obj = $reply | ConvertFrom-Json -ErrorAction SilentlyContinue
    if ($obj.id -ne $id) { Write-Warning "reply id '$($obj.id)' does not match '$id'" }
    if (-not $obj.ok) { exit 1 }
    exit 0
} else {
    Write-Warning "no reply within $TimeoutS s $(if ($e) { ": $e" })"
    exit 2
}
