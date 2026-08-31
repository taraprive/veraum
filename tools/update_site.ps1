# Regenerates the public verifiable track-record site from the live forward
# evidence, then verifies the hash chain. Publishing the generated site/ folder
# to any static host (GitHub Pages, Cloudflare Pages - all $0) makes it public.
#
#   .\tools\update_site.ps1
#
# The journal + bars are copied into site/ so anyone can re-verify the chain
# from the hosted files alone:  python tools/verify_trackrecord.py
#   --signals site\forward_signals.jsonl --bars site\mt5_bars.jsonl
#   --published site\trackrecord.json
#
param([string]$Signals = "", [string]$Bars = "", [string]$Out = "site")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$py = (Get-Command python).Source
if (-not $Signals) { $Signals = Join-Path $root "build\forward\forward_signals.jsonl" }
if (-not $Bars)    { $Bars    = Join-Path $root "build\mt5_bars.jsonl" }
$outAbs = Join-Path $root $Out

if (-not (Test-Path $outAbs)) { New-Item -ItemType Directory -Path $outAbs | Out-Null }
& $py -X utf8 (Join-Path $root "tools\track_record.py") --signals $Signals --bars $Bars --out $outAbs
if ($LASTEXITCODE -ne 0) { throw "track_record.py failed" }

& $py -X utf8 (Join-Path $root "tools\verify_trackrecord.py") `
    --signals (Join-Path $outAbs (Split-Path -Leaf $Signals)) `
    --bars (Join-Path $outAbs (Split-Path -Leaf $Bars)) `
    --published (Join-Path $outAbs "trackrecord.json")
if ($LASTEXITCODE -ne 0) { throw "verify_trackrecord.py failed" }

Write-Host ""
Write-Host "Site ready: $outAbs (index.html + trackrecord.json)"
