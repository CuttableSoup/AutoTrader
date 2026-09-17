# Start the paper stack on Windows (one process per window). Stop with scripts\stop_paper.ps1 or close the windows.
# Prerequisites: .venv with `uv pip install -e python -e watchdog`, tools\nats\nats-server.exe, C++ build in $BuildDir, config\paper.json, config\watchdog.json.
param(
  [string]$BuildDir = "cpp\build\cyg-full\bin",
  [string]$Config = "config\paper.json",
  [switch]$NoWatchdog
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$py = Join-Path $root ".venv\Scripts\python.exe"
New-Item -ItemType Directory -Force var\jetstream, var\log, var\state | Out-Null

function Start-Svc($name, $cmd) {
  Write-Host "starting $name : $cmd"
  Start-Process -FilePath "powershell.exe" -ArgumentList "-NoExit", "-Command", "`$host.UI.RawUI.WindowTitle='$name'; Set-Location '$root'; $cmd" -WindowStyle Minimized
}

Start-Svc "nats" "tools\nats\nats-server.exe -c config\nats\nats-server.conf"
Start-Sleep -Seconds 2
& $py scripts\bootstrap_streams.py
Start-Svc "logger"     "& '$py' -m autotrader.logger_service --config $Config"
Start-Svc "pager"      "& '$py' -m autotrader.pager --config $Config"
if (-not $NoWatchdog) { Start-Svc "watchdog" "& '$py' -m watchdog.main config\watchdog.json" }
Start-Svc "portfolio"  "$BuildDir\at_portfolio_svc.exe --config $Config"
Start-Svc "reconciler" "$BuildDir\at_reconciler_svc.exe --config $Config"
Start-Svc "ingestor"   "$BuildDir\at_ingestor_svc.exe --config $Config"
Start-Svc "events"     "& '$py' -m autotrader.events_feed.feed --config $Config"
Start-Svc "strategy"   "$BuildDir\at_strategy_svc.exe --config $Config"
Start-Svc "tsmom"      "$BuildDir\at_tsmom_svc.exe --config $Config"
Start-Svc "validator"  "& '$py' -m autotrader.validator.sidecar --config $Config"
Start-Svc "risk"       "$BuildDir\at_risk_svc.exe --config $Config"
Start-Svc "execution"  "$BuildDir\at_execution_svc.exe --config $Config"
Write-Host "paper stack started. Nothing trades until broker.reconcile reports CLEAN (see var\log\reconciler.log)."
