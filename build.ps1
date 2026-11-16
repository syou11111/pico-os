$ErrorActionPreference = "Stop"

$toolsDir = "$env:USERPROFILE\.pico-sdk\tools"
$armBin = (Get-ChildItem $toolsDir -Directory -Filter "arm-gnu-toolchain*" | Select-Object -First 1).FullName + "\bin"
$gitBin = "$toolsDir\mingit\cmd"
$pioasmDir = "$toolsDir\pico-sdk-tools\pioasm"
$picotoolDir = "$toolsDir\picotool\picotool"

$env:Path = "$armBin;$gitBin;$pioasmDir;$picotoolDir;" + [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
$env:PICO_SDK_PATH = "$env:USERPROFILE\.pico-sdk\pico-sdk"

$buildDir = Join-Path $PSScriptRoot "build"
cmake -B $buildDir -S $PSScriptRoot -G Ninja "-Dpioasm_DIR=$pioasmDir" "-Dpicotool_DIR=$picotoolDir"
cmake --build $buildDir
