Set-Location $PSScriptRoot
python -m PyInstaller --noconfirm --clean --onefile --windowed `
    --distpath dist\windows `
    --workpath build_win `
    --specpath build_win `
    --name TUE_CMSIS_DAP `
    tue_dap_gui.py
Write-Host "Built: $PSScriptRoot\dist\windows\TUE_CMSIS_DAP.exe"
