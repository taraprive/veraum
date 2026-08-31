# Live forward-monitoring session for the directional signal service.
#
# Starts two detached background processes:
#   1. the MT5 feed (closed H1 bars -> build\mt5_bars.jsonl)
#   2. the bot, reading that feed with the tuned H1 parameters
# The bot runs a two-week session (ops.run_seconds only counts the crypto loop;
# 1209600 = 14 days). Restart the session with the same script whenever it has
# expired; the feed resumes at the file tail and the bar file is append-only
# (deduplicated by timestamp), so no history is replayed and none is lost.
# Stop with -Stop.
#
#  .\tools\run_forward_live.ps1
#  .\tools\run_forward_live.ps1 -Stop
#
# Evidence lives in:
#   build\forward\forward_signals.jsonl   (signals as they are emitted)
#   build\mt5_bars.jsonl                  (bars, appended by the feed)
# Resolve outcome vs expectation weekly:
#   python tools\forward_report.py --signals build\forward\forward_signals.jsonl `
#       --bars build\mt5_bars.jsonl

param([switch]$Stop)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$fwd = Join-Path $build "forward"
$feedFile = Join-Path $build "mt5_bars.jsonl"
$feedPy = Join-Path $root "tools\mt5_feed.py"
$botExe = Join-Path $build "hft_arbitrage_bot.exe"
$py = (Get-Command python).Source

if ($Stop) {
    $pidFile = "feed.pid"
    $f = Join-Path $fwd $pidFile
    if (Test-Path $f) {
        $pid_ = (Get-Content $f).Trim()
        if ($pid_ -and $pid_ -match "^\d+$") {
            Stop-Process -Id ([int]$pid_) -Force -ErrorAction SilentlyContinue
            Write-Host "stopped $pid_ ($pidFile)"
        }
        Remove-Item $f -ErrorAction SilentlyContinue
    }
    # Stop the feed watchdog (it would respawn the feed) by its own marker.
    $wdMark = Join-Path $fwd "watchdog.pid"
    if (Test-Path $wdMark) {
        $wid = (Get-Content $wdMark).Trim()
        Stop-Process -Id ([int]$wid) -Force -ErrorAction SilentlyContinue
        Remove-Item $wdMark -ErrorAction SilentlyContinue
    }
    # the feed is a grandchild of the launching shells, kill anything orphaned.
    Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
        Where-Object { $_.CommandLine -like "*mt5_feed.py*" -or $_.CommandLine -like "*feed_watchdog.py*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Get-Process hft_arbitrage_bot -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    return
}

if (-not (Test-Path $fwd)) { New-Item -ItemType Directory -Path $fwd | Out-Null }

# Forward config: same model/params as the demo, but a day-long session and a
# dedicated log + signal journal.
$cfg = Get-Content (Join-Path $fwd "..\tryrun\config.json") -Raw | ConvertFrom-Json
$cfg.ops.run_seconds = 1209600
$cfg.ops.log_file = "forward.log"
$cfg.ops.state_file = "forward_balances.json"
$cfg.signals.jsonl = "forward_signals.jsonl"
# This session is for the directional (gold/silver/nasdaq) signal service.
# Silence the legacy crypto arbitrage flood so the journal shows only
# directional signals: raw directional delivery is not gated by this field.
$cfg.signals.min_net_spread = 1.0
$cfg | ConvertTo-Json -Depth 20 | Set-Content (Join-Path $fwd "config.json") -Encoding utf8

Copy-Item (Join-Path $fwd "..\tryrun\exchanges.json") $fwd -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $fwd "..\tryrun\credentials.json") $fwd -Force -ErrorAction SilentlyContinue
if (-not (Test-Path $feedFile)) { New-Item -ItemType File -Path $feedFile | Out-Null }

# 1. Feed: H1 closed bars appended to the shared live file. Detached via a
#    batch file (Start-Process of python.exe alone does not work on this box).
#    A watchdog (tools\feed_watchdog.py) keeps the feed alive: the feed's
#    polling loop occasionally hangs, and the watchdog restarts it if the bar
#    file stops growing.
$feedLog = Join-Path $fwd "feed.log"
$wdLog = Join-Path $fwd "watchdog.log"
$wdMark = Join-Path $fwd "watchdog.pid"
$wdPy = Join-Path $root "tools\feed_watchdog.py"
$wdBat = Join-Path $fwd "run_watchdog.bat"
@"
@echo off
"$py" -X utf8 "$wdPy" --feed "$feedFile" --out "$feedFile" --mark "$wdMark" --log "$wdLog" --stall-sec 1200 --poll-sec 60
"@ | Set-Content -LiteralPath $wdBat -Encoding ascii
Start-Process -FilePath "cmd.exe" -ArgumentList @("/c", "`"$wdBat`"") -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 4
Write-Host "feed watchdog started (log: $wdLog)"

$bat = Join-Path $fwd "run_feed.bat"
@"
@echo off
"$py" -X utf8 "$feedPy" --tf H1 --out "$feedFile" >> "$feedLog" 2>&1
"@ | Set-Content -LiteralPath $bat -Encoding ascii
Start-Process -FilePath "cmd.exe" -ArgumentList @("/c", "`"$bat`"") -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 5
Write-Host "feed started -> $feedFile (log: $feedLog)"

# 2. Bot: detached via batch (same proven method as the feed). Logs go to
#    forward.log so we do not need a separate output redirection.
$botLog = Join-Path $fwd "forward.log"
$botBat = Join-Path $fwd "run_bot.bat"
@"
@echo off
cd /d "$fwd"
start "" /b "$botExe" config.json exchanges.json credentials.json >> "$botLog" 2>&1
"@ | Set-Content -LiteralPath $botBat -Encoding ascii
Start-Process -FilePath "cmd.exe" -ArgumentList @("/c", "`"$botBat`"") -WindowStyle Hidden | Out-Null
Start-Sleep -Seconds 10
$botAlive = Get-CimInstance Win32_Process -Filter "Name='hft_arbitrage_bot.exe'" -ErrorAction SilentlyContinue
if ($botAlive) { Write-Host "bot alive (pid=$($botAlive.ProcessId)); log: $botLog" }
else { Write-Host "bot EXITED (see $botLog)" }

Write-Host ""
Write-Host "view signals:  Get-Content -Wait $fwd\forward_signals.jsonl"
Write-Host "two-week report: python tools\forward_report.py --signals $fwd\forward_signals.jsonl --bars $feedFile"
Write-Host "stop session:  .\tools\run_forward_live.ps1 -Stop"