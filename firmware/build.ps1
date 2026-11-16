# Build passkey firmware on Windows/macOS/Linux with Docker (no local ESP-IDF needed).
# Usage: pwsh firmware/build.ps1 -Target esp32s3
param(
    [ValidateSet('esp32s3', 'esp32s2', 'esp32', 'esp32c3', 'esp32c6')]
    [string]$Target = 'esp32s3',
    [string]$IdfImage = 'espressif/idf:v5.5.4',
    [string]$UsbVid = '',
    [string]$UsbPid = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$envArgs = @()
if ($UsbVid) { $envArgs += @('-e', "USB_VID=$UsbVid") }
if ($UsbPid) { $envArgs += @('-e', "USB_PID=$UsbPid") }

docker run --rm `
    -v "${root}:/src" `
    -v passkey-idf-cache:/root/.cache `
    @envArgs `
    $IdfImage `
    bash -c "tr -d '\r' < /src/firmware/build.sh > /tmp/build.sh && bash /tmp/build.sh $Target /src/dist"
if ($LASTEXITCODE -ne 0) { throw "Firmware build failed ($LASTEXITCODE)" }
Write-Host "Firmware: $root\dist\passkey_$Target.bin"
