$ErrorActionPreference = "Stop"
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $dir

New-Item -ItemType Directory -Force -Path "build" | Out-Null

# Prefer MSYS2 UCRT64 or Qt MinGW64 for clean modern Win32 + C++17 static linking
$gpp = "C:\msys64\ucrt64\bin\g++.exe"
$windres = "C:\msys64\ucrt64\bin\windres.exe"
if (-not (Test-Path $gpp)) {
    $gpp = "g++"
    $windres = "windres"
}

& $windres app.rc -O coff -o build/app_res.o
& $gpp -std=c++17 -O2 -s -Wall -Wextra -DUNICODE -D_UNICODE -municode -mwindows -static `
    main_gui.cpp hid_device.cpp firmware_loader.cpp cmsis_dap.cpp serial_port.cpp openocd_runner.cpp build/app_res.o `
    -o build/TUE_CMSIS_DAP_CPP.exe `
    -lcomctl32 -lhid -lsetupapi -lcomdlg32 -lgdi32 -luser32 -luuid

Get-Item build/TUE_CMSIS_DAP_CPP.exe | Select-Object Name, Length, LastWriteTime
