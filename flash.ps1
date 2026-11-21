param(
    [string]$Uf2Path = (Join-Path $PSScriptRoot "build\pico_os.uf2"),
    [switch]$Build
)

$ErrorActionPreference = "Stop"

if ($Build) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build.ps1")
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path $Uf2Path)) {
    Write-Error "UF2 file not found: $Uf2Path"
    exit 1
}

function Get-BootselDrive {
    $vol = Get-Volume | Where-Object { $_.FileSystemLabel -eq "RPI-RP2" -and $_.DriveLetter } | Select-Object -First 1
    if ($vol) { return "$($vol.DriveLetter):\" }
    return $null
}

$drive = Get-BootselDrive
if (-not $drive) {
    $ports = Get-CimInstance Win32_SerialPort | Select-Object -ExpandProperty DeviceID
    if ($ports) {
        Write-Host "Sending 'bootsel' command to Pico ($($ports -join ', '))..."
        python -c "
import serial, time
for p in '''$($ports -join ' ')'''.split():
    try:
        s = serial.Serial(p, 115200, timeout=0.5)
        s.dtr = True
        time.sleep(0.25)
        s.write(b'\r\nbootsel\r\n')
        s.flush()
        time.sleep(0.15)
        s.close()
    except Exception:
        pass
" 2>$null
    } else {
        Write-Host "Waiting for RPI-RP2 drive (hold BOOTSEL and plug in USB if needed)..."
    }

    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 400
        $drive = Get-BootselDrive
        if ($drive) { break }
    }
}

if (-not $drive) {
    Write-Error "RPI-RP2 drive did not appear."
    exit 1
}

$dest = Join-Path $drive (Split-Path $Uf2Path -Leaf)
Write-Host "Flashing $Uf2Path -> $dest ..."
Copy-Item -Force $Uf2Path $dest
Write-Host "Done! Pico is rebooting into PicoOS."
