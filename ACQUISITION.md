# Getting the first 10 users ($0)

Aurum Signals' moat is not hype — it is a **verifiable track record**. Every
acquisition channel below is chosen to amplify that honesty advantage and cost
nothing.

## Why this wins vs. incumbents

Almost every signal channel sells on screenshots and pasted profit claims that
cannot be checked. We publish every signal, win and loss, into a hash chain
anyone can re-verify from the hosted raw data. That is a durable, defensible
differentiator and the core of every message we send.

## The growth loop (built, ready)

1. Engine emits a signal with entry/stop/target/lot + reason.
2. It is appended to the hash-chained journal.
3. `tools/update_site.ps1` regenerates the site: landing (`index.html`) +
   ledger (`track.html`) + shareable `proof-summary.svg` + raw evidence.
4. The "proof card" (SVG) + one-line verified summary is designed to be shared —
   each share is a self-contained, trustworthy ad.

Deploy the site so the loop is public: `.\tools\deploy_gh_pages.ps1`.

## Launch sequence (first 10 users)

1. **Go live on Telegram** (needs a bot token + channel from the user via
   @BotFather — the one external credential). Put `channel="telegram"`,
   `token`, `chat_id` in the forward `config.json`. Post each signal + a link
   to `track.html`. Anchor the `chain_head` in the channel so the public can
   verify nothing was edited retroactively.
2. **Seed with credibility, not hype.** First week: post signals + the ledger
   link, verbatim. Let the record speak. Losses posted openly build more trust
   than wins.
3. **Direct outreach** to gold/silver/trading Telegram groups and Reddit
   (r/gold, r/silverbugs, r/Daytrading, r/Forex) — offer the *verifiable
   ledger*, not "join my channel". One account, ~10 quality replies a day.
4. **SEO long-tail** on the landing page: "verifiable gold signals", "transparent
   silver signals". Low competition; the ledger is real content that ranks.
5. **Measure**: channel joins, track.html visits, proof-card shares, and forward
   results. Kill what does not convert; invest where it does.

## First revenue (later, optional)

Telegram **Stars** (in-app payments, no payout infra) for a premium tier — only
after the public record has proven out. The record is the reason anyone pays.

## Honesty guardrails (non-negotiable)

- Never claim guaranteed profit; every message carries the not-financial-advice
  framing.
- Losses are published exactly like wins.
- Stop-first resolution on same-bar conflicts; entry at signal close.
- The chain_head anchor makes retroactive edits impossible without detection.

## Blockers

- **Telegram delivery**: blocked until the user provides a bot token + channel
  ID (external credential; cannot be created programmatically).
- **Public site**: blocked until the user creates a GitHub repo + adds the
  `origin` remote (free; then `deploy_gh_pages.ps1` publishes it).
