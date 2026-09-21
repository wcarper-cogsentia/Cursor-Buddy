# Run the Buddy service on Windows.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location (Join-Path $Root "service")

if (-not (Test-Path ".venv")) {
    python -m venv .venv
    & .\.venv\Scripts\python.exe -m pip install -q -r requirements.txt
}

if (-not $env:BUDDY_HOST) { $env:BUDDY_HOST = "0.0.0.0" }
if (-not $env:BUDDY_PORT) { $env:BUDDY_PORT = "8787" }
Write-Host "Local receiver: http://127.0.0.1:$($env:BUDDY_PORT)/"
& .\.venv\Scripts\python.exe -m buddy
