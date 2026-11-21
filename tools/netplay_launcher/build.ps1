$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

$python = if( Get-Command py -ErrorAction SilentlyContinue ) { 'py' } else { 'python' }
if( -not (Test-Path '.venv') ) {
    & $python -m venv .venv
}
& .\.venv\Scripts\python.exe -m pip install --upgrade pip
& .\.venv\Scripts\python.exe -m pip install -r requirements-build.txt

& .\.venv\Scripts\python.exe -m PyInstaller `
    --noconfirm --clean --onefile --windowed `
    --name 'DDA-NetPlay-Launcher' `
    --paths . `
    main.py

"Built: $here\dist\DDA-NetPlay-Launcher.exe"
