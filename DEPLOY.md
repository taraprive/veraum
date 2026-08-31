# Deploying Aurum Signals (free static hosting)

The product ships as a **fully static `$0` site** in `site/`:

- `index.html` — landing page (CTA + value prop)
- `track.html` — the verifiable hash-chained track record (the trust moat)
- `trackrecord.json` + `proof-summary.svg` — machine-readable chain + share card
- the raw signal/bars evidence is copied alongside so anyone can re-verify

`site/` is a **generated** directory (gitignored in `main`). It is rebuilt from
the live signal journal by `tools/update_site.ps1` (which also re-verifies the
hash chain). Host it on any static host for free.

## Quick start (GitHub Pages — recommended)

1. Create a GitHub repo and configure the remote once:

       git remote add origin https://github.com/<user>/<repo>.git

2. In GitHub: **Settings > Pages > Source = "Deploy from a branch" > "gh-pages" / "(root)"**.

3. Deploy (rebuilds the site first, then publishes):

       .\tools\deploy_gh_pages.ps1

   Live URL: `https://<user>.github.io/<repo>/`

The deploy script uses `git worktree` so it never touches your working tree; it
publishes an orphan `gh-pages` branch that contains only the site snapshots.

## Alternatives (also $0)

- **Cloudflare Pages**: free static hosting, generous limits, global CDN. Push
  `site/` (or connect the repo) and set the build output to `site/`. Cloudflare
  offers a nicer build pipeline if you prefer a managed flow.
- **Netlify / Vercel**: drag-drop `site/` or connect the repo; free tiers apply.

## Rebuilding the site

The site is derived; after the forward journal changes, regenerate before deploy:

    .\tools\update_site.ps1

This regenerates all pages AND runs the independent chain verifier
(`tools/verify_trackrecord.py`). Deploy only when it reports `OK`.

## Notes

- Keep `site/` out of `main` (already gitignored). GitHub Pages reads the
  `gh-pages` branch, so the derived output never pollutes source history.
- When the Telegram channel/bot goes live, set `TELEGRAM_URL` in
  `tools/track_record.py` and regenerate so the landing CTA activates.
