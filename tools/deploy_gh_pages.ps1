# Deploy the static site (site/) to GitHub Pages via an orphan `gh-pages` branch.
#
# Zero-cost static hosting. The built site lives in site/, which is gitignored in
# the main branch (it is derived output), so we publish it as an orphan branch
# that carries no source history. GitHub Pages serves that branch's root.
#
# Uses `git worktree` so your main working tree is never disturbed.
#
# One-time setup:
#   1. Create a GitHub repo and add a remote:
#        git remote add origin https://github.com/<user>/<repo>.git
#   2. In GitHub: Settings > Pages > Source = "Deploy from a branch" > gh-pages / (root)
#
# Usage:
#   .\tools\deploy_gh_pages.ps1
#
# Rebuilds the site first so published pages always match the latest journals.

$root = Split-Path -Parent $PSScriptRoot
$site = Join-Path $root "site"
$work = Join-Path $env:TEMP ("aurum-gh-pages-" + [guid]::NewGuid().ToString("N"))

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "git not found on PATH. Add the Machine/User PATH containing git.exe."
}

# Build script needs a mutable env; deploy step itself is a git shell.
$ErrorActionPreference = "Continue"

# 1. Rebuild the site from the live journals (always current).
& (Join-Path $PSScriptRoot "update_site.ps1")
if ($LASTEXITCODE -ne 0) { throw "update_site.ps1 failed; aborting deploy." }
if (-not (Test-Path (Join-Path $site "index.html"))) {
    throw "site/index.html missing after rebuild; cannot deploy."
}

Push-Location $root
try {
    # Work in a detached worktree so the main checkout is never disturbed.
    git worktree add --detach "$work" 2>&1 | Out-Null
    if (-not (Test-Path $work)) { throw "could not create worktree" }

    Push-Location $work
    try {
        # 2. Replace worktree contents with the freshly built static site.
        git rm -rq --ignore-unmatch . 2>&1 | Out-Null
        Get-ChildItem -Force | Where-Object { $_.Name -ne ".git" } |
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        Copy-Item "$site\*" -Destination "$work" -Recurse -Force

        # 3. Make this an orphan so gh-pages never carries repo history.
        git checkout --orphan gh-pages 2>&1 | Out-Null
        git rm -rq --ignore-unmatch . 2>&1 | Out-Null
        Copy-Item "$site\*" -Destination "$work" -Recurse -Force

        git add -A -- .
        git -c user.email="deploy@aurumsignals.local" -c user.name="deploy" commit -m "Deploy site $(Get-Date -Format s)" 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "git commit failed on gh-pages" }
    }
    finally {
        Pop-Location
    }

    git remote get-url origin 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) {
        git push --force origin "$work`:gh-pages" 2>&1 | Out-Null
        Write-Host ""
        Write-Host "Deployed to origin/gh-pages."
    }
    else {
        Write-Host ""
        Write-Host "No 'origin' remote configured; built local 'gh-pages' branch."
        Write-Host "  git remote add origin https://github.com/<user>/<repo>.git"
        Write-Host "  git push --force origin gh-pages"
    }
}
finally {
    if (Test-Path $work) { git worktree remove --force "$work" 2>&1 | Out-Null }
    Pop-Location
}

Write-Host ""
Write-Host "Live URL once Pages is enabled: https://<user>.github.io/<repo>/"
