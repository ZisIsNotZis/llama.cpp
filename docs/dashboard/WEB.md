# Web dashboard - Design + implementation detail

Status: APPROVED BASELINE (draft)

Date: 2026-02-14

This is the detailed design + implementation for the web dashboard consumer of the shared producer (see DESIGN.md sections 5-7 for the sequence model and event producer). It is a tab in the built-in web UI, **on by default, no enable/disable switch**.

## 1. Goals

- A live per-sequence monitor in the browser: full scrollable text per sequence, immediate token push (no polling), terminal-emulator scroll-follow, per-sequence created/last-modified/cache-hit-rate.
- Show active sequences AND cached/evicted sequences (flat list, static text).
- Uses the same shared sequence event producer as the TUI; no duplicated server logic.

## 2. Non-goals

- No interactivity for now (no resume/clear/pause; read-only).
- No graphs, no historical plots.
- No long polling.

## 3. UX

- **Active sequences**: laid out in an HxW grid, `H = floor(sqrt(np))`, `W = ceil(np/H)` where `np` = number of parallel slots (e.g. 4 -> 2x2, 8 -> 2x4, 16 -> 4x4, 2 -> 1x2).
- **Each cell**: header (`id`, state, phase, created, modified, hit rate) + a scrollable text area.
- **Scroll-follow** (terminal-emulator style): if the user is at/near the bottom, keep pinned to the bottom as tokens stream in; if they scroll up, detach (with a "jump to bottom" affordance).
- **Cached/evicted sequences**: a separate flat list below the grid (or side panel). Each item = metadata + full static text in a scrollable area. They never grow, so no scroll-follow.
- **Full text**: the client receives full text for every cell on connect, but does NOT render it all into the DOM at once. Rendering is **virtualized/lazy**: items render as the user scrolls to their position (a virtualized list, no "load more" button).

## 4. Protocol (single SSE)

- One dedicated SSE endpoint, e.g. `GET /dashboard` (alias `/v1/sequences/stream`). One persistent connection; no per-cell connections, no polling.
- Auth: same mechanism as the rest of the web UI (no auth by default).
- Event flow:
  1. `event: snapshot` - full state: all cells (active + idle) with full text + seq, all cached entries with full text + metadata, global stats.
  2. `event: delta` - `{ id, seq, text }`: new tokens for one active cell (batched per decode step).
  3. `event: cell` - state/metadata change (phase, kv, speeds, hit rate, created/modified) for one cell.
  4. `event: cell-add` / `event: cell-remove` - a sequence was created/died (or moved to cache).
  5. `event: cache` - the cached list changed (entry added/removed).
- **No token loss between snapshot and first delta** (DESIGN.md section 6): the handler subscribes to the shared log first, then reads the snapshot under the same mutex. Per-cell `seq` allows the client to detect a gap and re-sync that cell.

## 5. Server-side plumbing

- Consume the **shared event producer** (DESIGN.md section 6): per-sequence append-only token log + state events.
- Extend the sequence model on the server:
  - `server_slot`: `t_created`, `t_modified`, per-sequence `hit_rate` (from `stats.n_prompt_cached`).
  - `server_prompt_cache_state`: carry `t_created`, `t_modified`, `hit_rate` (already has `id_slot`); these travel with the entry and are restored on load; cleared when the entry is pruned (sequence fully dies).
- New SSE handler (`server-routes`/`server-http`): on connect, subscribe -> snapshot -> stream events.
- The `stream:false` question (BTW4): decode always happens in real-time in `update_slots()` regardless of any request's `stream` flag; `stream:false` only changes delivery to the requesting client. The dashboard reads the engine's per-slot stream, so it sees live tokens either way. No extra decode, no double decode.
- The snapshot/delta race (BTW5): subscribe-before-snapshot under one mutex + per-cell seq.

## 6. Frontend (SvelteKit, built-in UI)

- New route: `tools/ui/src/routes/dashboard/+page.svelte`; add a Dashboard entry to the UI nav.
- SSE client: browser `EventSource` (built-in).
- Stores: `cells` (map id -> cell: text log, seq, metadata), `cached` (list), `global` (stats). `delta` appends text by `id` at `seq`; on seq gap, re-sync that cell.
- Virtualized rendering: use a virtualized-list library for the cells and for each cell's text (e.g. `@tanstack/virtual-core` or `svelte-windowed`) - the "modern wheel" for lazy render-on-scroll; no "load more" button.
- Grid: CSS grid with HxW derived from `np` (from snapshot). Responsive fallback (stack on narrow screens) - backlog.
- Scroll-follow: on `scroll` of a cell, if at bottom keep pinned; `delta` appends and auto-scrolls only while pinned; show a "jump to bottom" chip when detached.

## 7. Build / integration

- The web UI is built by the existing UI build step (`scripts/ui-assets.cmake`, `tools/ui/`); adding a route is picked up by the SvelteKit build. No new server link deps; SSE uses the existing HTTP plumbing.
- Server: add the `/dashboard` SSE handler + the shared producer wiring; no new endpoint flags (on by default).

## 8. Validation

- Open the UI with the server running; the Dashboard tab shows cells appearing as requests start, text appends live (both `stream:true` and `stream:false` requests), scroll-follow works, cached sequences appear in the flat list after eviction, created/modified/hit-rate show per cell.
- Rapid token generation: verify no dropped text between connect and first delta (re-sync path).
- Many slots: verify the HxW grid and virtualized rendering stay responsive.

## 9. Risks and notes

- Full text for 32+ cells on connect can be large; virtualized rendering keeps DOM small, but the initial SSE payload is one-time - acceptable (per decision, no "load more").
- The dashboard is on by default; it must not block the server or consume significant resources when no browser is connected (producer only emits when there are subscribers; or emit is cheap regardless).

## Change log

| Date | Change | Approved by |
|---|---|---|
| 2026-02-14 | Initial baseline: web dashboard detail (SSE, on by default, HxW grid, lazy/virtualized text, scroll-follow, shared producer). | owner |
| 2026-02-14 | Server-side foundation implemented: shared `dash::feed` producer (snapshot + seq'd deltas + evicted cells), sequence metadata (`server_slot` t_created/t_modified/hit_rate; `server_prompt_cache_state` carries them), engine publish per update_slots, SSE endpoint `GET /dashboard` (subscribe -> snapshot -> deltas, resync on ring gap), first-cut UI route `tools/ui/src/routes/dashboard/+page.svelte`. | owner |
| 2026-02-14 | Verified end-to-end with Qwen3.5-0.8B (CPU): `/dashboard` streams snapshot + per-token deltas live (80 deltas during one request, works with `stream:false`); cache-hit rate shown (0.73 on a cache hit); epoch timestamps correct; frontend builds and is embedded in the UI bundle. Sequence timestamps use `system_clock` epoch; `hit_rate` persists on the slot across release. | owner |
| 2026-02-14 | Fixes after browser testing: (1) `/dashboard` now sends `global` + per-slot `slots` status in the snapshot and as periodic `status` events (~1 s), so the dashboard shows the same content as `--tui` ([SERVER]/[RUN]/[MEMORY]/[THROUGHPUT]/[GPU]/[SYSTEM]/[TOTAL] + per-[SEQ] phase/kv/speeds). (2) Frontend switched from a non-reactive `Map` to a reactive array (Svelte 5 `Map` mutation does not trigger updates - this caused "does not refresh / does not grow"). (3) Sidebar Dashboard entry added (`ROUTES.DASHBOARD`, `ui.constants.ts`). | owner |
| 2026-02-14 | Fixes after browser testing: header/stat colors use the app's shadcn theme tokens (was black-on-black); grid gets bounded rows (`repeat(H, minmax(0,1fr))`) + `100dvh` root so each cell has its own scrollbar (was a global scrollbar). | owner |
| 2026-02-14 | Fix: KV memory shows USED bytes, not the reserved buffer. `llama_memory_breakdown` reports the full reserved KV allocation (huge with a big `-c`), which alarmed users (e.g. "kv cpu 359.8GB"). Now `kv_gpu`/`kv_cpu` in the dashboard JSON are used = reserved x (used cells / reserved cells); `kv_gpu_res`/`kv_cpu_res` carry the reservation. | owner |
