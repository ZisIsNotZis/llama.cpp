<script lang="ts">
	// Live sequence dashboard (docs/dashboard/WEB.md).
	// Same UX as the ncurses TUI: HxW grid (shared grid_dims objective, auto ratio),
	// per-cell scroll-follow + selection + maximize, bottom input bar (raw completion
	// pinned to the selected cell via id_slot), kill, [ ] ratio tuning.
	import { onMount } from 'svelte';
	import { base } from '$app/paths';

	type Global = {
		model: string;
		alias: string;
		ctx: number;
		ctx_train: number;
		slots: number;
		kv_unified: boolean;
		flash_attn: string;
		kv_type: string;
		sleeping: boolean;
		busy: number;
		deferred: number;
		engine: number;
		prompt_tps: number;
		gen_tps: number;
		spec_acc: number;
		spec_prop_len: number;
		spec_acc_len: number;
		hit_rate: number;
		pp_ms: number;
		tg_ms: number;
		first_tok: number;
		req_s: number;
		requests: number;
		kv_gpu: number;
		kv_cpu: number;
		weights_gpu: number;
		ram_cache: number;
		rss: number;
		uptime: number;
		total_prompt: number;
		total_gen: number;
		total_cached: number;
		total_decode: number;
	};

	type Slot = {
		id: number;
		phase: string;
		loc: string;
		occupied: boolean;
		kv_used: number;
		n_ctx: number;
		n_prompt_cached: number;
		n_decoded: number;
		n_remain: number;
		pp_tps: number;
		tg_tps: number;
		pp5: number;
		tg5: number;
		queue_ms: number;
		task: number;
	};

	type Cell = {
		id: number;
		active: boolean;
		evicted: boolean;
		created: number;
		modified: number;
		hit: number;
		seq: number;
		text: string;
	};

	let es: EventSource | undefined;
	let connected = $state(false);
	let errorMsg = $state('');
	let global = $state<Global | null>(null);
	let slots = $state<Slot[]>([]);
	let cells = $state<Cell[]>([]);
	let refs = $state<Record<number, HTMLDivElement>>({});
	let pinned = $state<Record<number, boolean>>({});

	// TUI parity state
	let selected = $state(-1);
	let maximized = $state(-1);
	let ratio = $state(0.21); // auto ratio = usable rows/cols (computed on mount)
	let gridW = $state(0);
	let gridH = $state(0);
	let inputMode = $state(false);
	let input = $state('');
	let inputSlot = $state(-1);
	let statusMsg = $state('');

	function applySnapshot(s: { global: Global; slots: Slot[]; cells: Cell[] }) {
		global = s.global;
		slots = s.slots ?? [];
		cells = s.cells ?? [];
		if (selected < 0 && cells.length > 0) selected = cells[0].id;
	}

	function onDelta(id: number, seq: number, text: string) {
		const c = cells.find((x) => x.id === id);
		if (!c || !text) return;
		if (seq <= c.seq) return;
		c.seq = seq;
		c.text += text;
	}

	function onUpsert(id: number, evicted: boolean, active: boolean, seq: number, text: string) {
		const c = cells.find((x) => x.id === id);
		if (c) {
			c.evicted = evicted;
			c.active = active;
			if (seq > c.seq) c.seq = seq;
			if (text && text !== c.text) c.text = text;
		} else {
			cells = [
				...cells,
				{ id, active, evicted, created: 0, modified: 0, hit: -1, seq, text: text ?? '' }
			];
		}
	}

	function onRemove(id: number) {
		cells = cells.filter((x) => x.id !== id);
		if (selected === id) selected = cells[0]?.id ?? -1;
		if (maximized === id) maximized = -1;
	}

	onMount(() => {
		// auto ratio: square cells on the usable area
		ratio = Math.min(0.75, Math.max(0.12, (window.innerHeight - 220) / window.innerWidth));
		es = new EventSource(`${base}/dashboard`);
		es.addEventListener('snapshot', (e) => applySnapshot(JSON.parse(e.data)));
		es.addEventListener('status', (e) => {
			const d = JSON.parse(e.data);
			global = d.global;
			slots = d.slots ?? [];
		});
		es.addEventListener('delta', (e) => {
			const d = JSON.parse(e.data);
			onDelta(d.id, d.seq, d.text ?? '');
		});
		es.addEventListener('add', (e) => {
			const d = JSON.parse(e.data);
			onUpsert(d.id, false, true, d.seq, d.text ?? '');
		});
		es.addEventListener('cache', (e) => {
			const d = JSON.parse(e.data);
			onUpsert(d.id, true, false, d.seq, d.text ?? '');
		});
		es.addEventListener('remove', (e) => onRemove(JSON.parse(e.data).id));
		es.addEventListener('cell', (e) => {
			const d = JSON.parse(e.data);
			const c = cells.find((x) => x.id === d.id);
			if (c) {
				if (typeof d.seq === 'number') c.seq = d.seq;
				if (typeof d.text === 'string' && d.text) c.text = d.text;
			}
		});
		es.onopen = () => (connected = true);
		es.onerror = () => {
			connected = false;
			errorMsg = 'dashboard stream disconnected';
		};
		window.addEventListener('keydown', onKey);
		return () => {
			es?.close();
			window.removeEventListener('keydown', onKey);
		};
	});

	// ---- TUI parity: grid (shared grid_dims objective), selection, maximize, input, kill ----

	function gridDims(n: number, rows: number, cols: number, r: number): { H: number; W: number } {
		if (n <= 0) return { H: 1, W: 1 };
		const target = (r * cols) / rows;
		let best = Infinity;
		let W = 1;
		let H = 1;
		for (let w = 1; w <= n; w++) {
			const h = Math.ceil(n / w);
			const dev = Math.abs(Math.log(w / h) - Math.log(target));
			const empty = h * w - n;
			const score = dev + 0.3 * Math.log(1 + empty);
			if (score < best) {
				best = score;
				W = w;
				H = h;
			}
		}
		return { H, W };
	}

	const activeCells = $derived(cells.filter((c) => !c.evicted));
	const cachedCells = $derived(cells.filter((c) => c.evicted));
	// grid over the usable area; maximized cell takes the whole grid
	const grid = $derived(
		maximized >= 0
			? { W: 1, H: 1 }
			: gridDims(activeCells.length, Math.max(1, gridH), Math.max(1, gridW), ratio)
	);
	const gridRows = $derived(Math.max(grid.H, 1));
	const gridCols = $derived(Math.max(grid.W, 1));
	const visibleCells = $derived(
		maximized >= 0 ? activeCells.filter((c) => c.id === maximized) : activeCells
	);

	function slotFor(id: number): Slot | undefined {
		return slots.find((s) => s.id === id);
	}

	function onCellScroll(id: number, el: HTMLDivElement) {
		pinned[id] = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
	}

	$effect(() => {
		for (const c of [...cells]) {
			const el = refs[c.id];
			if (el && pinned[c.id] !== false) el.scrollTop = el.scrollHeight;
		}
	});

	function select(id: number) {
		selected = id;
		if (maximized >= 0 && maximized !== id) maximized = -1;
	}

	function toggleMax(id: number) {
		maximized = maximized === id ? -1 : id;
	}

	async function killCell(id: number) {
		try {
			await fetch(`${base}/abort`, {
				method: 'POST',
				headers: { 'Content-Type': 'application/json' },
				body: JSON.stringify({ slot_id: id })
			});
			statusMsg = `killed slot ${id}`;
			setTimeout(() => (statusMsg = ''), 4000);
		} catch {
			statusMsg = 'kill failed';
		}
	}

	async function submitInput() {
		if (!input.trim() || inputSlot < 0) {
			inputMode = false;
			input = '';
			return;
		}
		// raw completion pinned to the selected slot (no chat template)
		fetch(`${base}/completion`, {
			method: 'POST',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify({ prompt: input, id_slot: inputSlot, n_predict: 512, stream: false })
		}).catch(() => {});
		statusMsg = `fired completion to slot ${inputSlot}`;
		setTimeout(() => (statusMsg = ''), 4000);
		inputMode = false;
		input = '';
	}

	function startInput(id: number) {
		inputSlot = id;
		input = '';
		inputMode = true;
	}

	function onKey(e: KeyboardEvent) {
		// input mode captures text; Enter sends, Shift+Enter newline, Esc cancels
		if (inputMode) {
			if (e.key === 'Enter' && e.shiftKey) {
				input += '\n';
				e.preventDefault();
			} else if (e.key === 'Enter') {
				e.preventDefault();
				submitInput();
			} else if (e.key === 'Escape') {
				inputMode = false;
				input = '';
			} else if (e.key === 'Backspace') {
				input = input.slice(0, -1);
			} else if (e.key.length === 1) {
				input += e.key;
			}
			return;
		}
		const target = selected >= 0 ? selected : activeCells[0]?.id ?? -1;
		switch (e.key) {
			case 'i':
			case 'I':
				if (target >= 0) startInput(target);
				break;
			case 'k':
			case 'K':
				if (target >= 0) killCell(target);
				break;
			case '[':
				ratio = Math.max(0.08, ratio - 0.02);
				break;
			case ']':
				ratio = Math.min(1.0, ratio + 0.02);
				break;
			case 'ArrowLeft':
				selectCellRelative(-1);
				break;
			case 'ArrowRight':
				selectCellRelative(1);
				break;
			case 'Enter':
				if (target >= 0) toggleMax(target);
				break;
			default:
				break;
		}
	}

	function selectCellRelative(dx: number) {
		const ids = activeCells.map((c) => c.id);
		if (ids.length === 0) return;
		let pos = ids.indexOf(selected);
		if (pos < 0) pos = 0;
		selected = ids[(pos + dx + ids.length) % ids.length];
	}

	function fmtTime(t: number): string {
		if (!t) return '-';
		return new Date(t / 1000).toLocaleTimeString();
	}
	function fmtPct(h: number): string {
		if (h < 0) return '-';
		return `${Math.round(h * 100)}%`;
	}
	function fmtNum(v: number, d = 1): string {
		if (!v || v <= 0) return '-';
		return v.toFixed(d);
	}
	function fmtBytes(b: number): string {
		if (!b) return '-';
		const u = ['B', 'kB', 'MB', 'GB', 'TB'];
		let i = 0;
		let v = b;
		while (v >= 1024 && i < u.length - 1) {
			v /= 1024;
			i++;
		}
		return `${v.toFixed(1)}${u[i]}`;
	}
</script>

<svelte:head><title>Dashboard</title></svelte:head>

<div class="dash">
	<div class="bar">
		<strong>Sequence dashboard</strong>
		<span class={connected ? 'ok' : 'bad'}>{connected ? 'live' : 'offline'}</span>
		{#if errorMsg}<span class="err">{errorMsg}</span>{/if}
		<span class="counts">{activeCells.length} active / {cachedCells.length} cached</span>
	</div>

	{#if global}
		<div class="stats">
			<div class="row">
				<span class="k">model</span> {global.model} ({global.alias})
				<span class="k">ctx</span> {global.ctx}/{global.ctx_train}
				<span class="k">kv</span> {global.kv_unified ? 'unified' : 'split'} {global.kv_type}
				<span class="k">fa</span> {global.flash_attn}
				<span class="k">up</span> {global.uptime}s
			</div>
			<div class="row">
				<span class="k">busy</span> {global.busy}/{global.slots}
				<span class="k">queue</span> {global.deferred}
				<span class="k">engine</span> {Math.round(global.engine * 100)}%
				<span class="k">req</span> {fmtNum(global.req_s)}/s
				<span class="k">prompt</span> {fmtNum(global.prompt_tps, 0)}/s
				<span class="k">gen</span> {fmtNum(global.gen_tps, 0)}/s
				<span class="k">hit</span> {fmtPct(global.hit_rate)}
				{#if global.spec_acc >= 0}
					<span class="k">spec</span> {fmtPct(global.spec_acc)}
					<span class="k">len</span> {fmtNum(global.spec_acc_len)}/{fmtNum(global.spec_prop_len)}
				{/if}
			</div>
			<div class="row">
				<span class="k">pp</span> {fmtNum(global.pp_ms)}ms
				<span class="k">tg</span> {fmtNum(global.tg_ms)}ms
				<span class="k">ftok</span> {fmtNum(global.first_tok)}s
				<span class="k">kv gpu</span> {fmtBytes(global.kv_gpu)}
				<span class="k">kv cpu</span> {fmtBytes(global.kv_cpu)}
				<span class="k">weights</span> {fmtBytes(global.weights_gpu)}
				<span class="k">cache</span> {fmtBytes(global.ram_cache)}
				<span class="k">rss</span> {fmtBytes(global.rss)}
			</div>
			<div class="row">
				<span class="k">total</span> prompt {global.total_prompt} gen {global.total_gen}
				cached {global.total_cached} decode {global.total_decode}
				<span class="k">ratio</span> {ratio.toFixed(2)}
			</div>
		</div>
	{/if}

	{#if activeCells.length === 0 && cachedCells.length === 0}
		<p class="empty">No sequences yet. Send a request to the server.</p>
	{:else}
		<div
			class="grid"
			bind:clientWidth={gridW}
			bind:clientHeight={gridH}
			style={`grid-template-columns: repeat(${gridCols}, 1fr); grid-template-rows: repeat(${gridRows}, minmax(0, 1fr));`}
		>
			{#each visibleCells as c (c.id)}
				{@const s = slotFor(c.id)}
				{@const isSel = c.id === selected}
				<div
					class="cell"
					class:selected={isSel}
					class:maximized={c.id === maximized}
					onclick={(e) => {
						if (e.detail === 2) {
							toggleMax(c.id);
						} else {
							select(c.id);
						}
					}}
				>
					<div class="head">
						[SEQ {c.id}] {s ? s.phase : c.active ? 'ACTIVE' : 'IDLE'} loc:{s?.loc ?? '-'}
						kv:{s ? `${s.kv_used}/${s.n_ctx}` : '-'} cch:{s?.n_prompt_cached ?? '-'}
						pp:{fmtNum(s?.pp5)} tg:{fmtNum(s?.tg5)}
						hit:{fmtPct(c.hit)} created:{fmtTime(c.created)}
						{#if s && s.task >= 0}
							<button class="mini" onclick={(e) => { e.stopPropagation(); killCell(c.id); }}>kill</button>
						{/if}
					</div>
					<div
						class="text"
						bind:this={refs[c.id]}
						onscroll={(e) => onCellScroll(c.id, e.currentTarget as HTMLDivElement)}
					>{c.text}</div>
				</div>
			{/each}
		</div>

		{#if cachedCells.length > 0 && maximized < 0}
			<h2>Cached / evicted</h2>
			<div class="cached">
				{#each cachedCells as c (c.id)}
					<div class="cell">
						<div class="head">
							[CACHE {c.id}] hit:{fmtPct(c.hit)} created:{fmtTime(c.created)}
							mod:{fmtTime(c.modified)}
						</div>
						<div class="text" bind:this={refs[c.id]}>{c.text || '(empty)'}</div>
					</div>
				{/each}
			</div>
		{/if}
	{/if}

	<div class="inputbar">
		{#if inputMode}
			<span class="ib-label">[SEQ {inputSlot}]</span>
			<span class="ib-input">{input.replaceAll('\n', '\u21b5')}&#x2588;</span>
			<span class="ib-hint">Enter send - Shift+Enter newline - Esc cancel</span>
		{:else}
			<span class="ib-label">{statusMsg || 'idle - press i to input to selected cell, k to kill, [ ] ratio'}</span>
			<span class="ib-hint">click a cell to select, double-click to maximize</span>
		{/if}
	</div>
</div>

<style>
	.dash {
		display: flex;
		flex-direction: column;
		gap: 0.5rem;
		height: 100dvh;
		min-height: 0;
		padding: 0.5rem;
		box-sizing: border-box;
		overflow: hidden;
		color: var(--color-foreground);
	}
	.bar {
		display: flex;
		gap: 0.75rem;
		align-items: center;
		font-size: 0.85rem;
	}
	.bar .ok { color: var(--color-primary); }
	.bar .bad { color: var(--color-destructive); }
	.bar .err { color: var(--color-destructive); }
	.bar .counts { margin-left: auto; opacity: 0.7; }
	.stats {
		font-family: monospace;
		font-size: 0.78rem;
		background: var(--color-muted);
		color: var(--color-foreground);
		border: 1px solid var(--color-border);
		border-radius: 6px;
		padding: 0.4rem 0.6rem;
		display: flex;
		flex-direction: column;
		gap: 0.15rem;
		flex-shrink: 0;
	}
	.stats .row { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
	.stats .k { color: var(--color-primary); margin-left: 0.6rem; }
	.stats .k:first-child { margin-left: 0; }
	.empty { opacity: 0.6; }
	.grid {
		display: grid;
		gap: 0.5rem;
		flex: 1;
		min-height: 0;
		min-width: 0;
	}
	.cached {
		display: flex;
		flex-direction: column;
		gap: 0.5rem;
		max-height: 40%;
		min-height: 0;
		overflow-y: auto;
		flex-shrink: 0;
	}
	h2 { font-size: 0.85rem; margin: 0; }
	.cell {
		display: flex;
		flex-direction: column;
		border: 1px solid var(--color-border);
		border-radius: 6px;
		overflow: hidden;
		min-height: 0;
		min-width: 0;
	}
	.cell.selected {
		border-color: var(--color-primary);
		box-shadow: 0 0 0 1px var(--color-primary);
	}
	.head {
		font-size: 0.72rem;
		font-family: monospace;
		padding: 0.25rem 0.5rem;
		background: var(--color-muted);
		color: var(--color-muted-foreground);
		border-bottom: 1px solid var(--color-border);
		white-space: nowrap;
		overflow: hidden;
		text-overflow: ellipsis;
		flex-shrink: 0;
	}
	.mini {
		font-size: 0.65rem;
		margin-left: 0.4rem;
		padding: 0 0.3rem;
		border: 1px solid var(--color-border);
		border-radius: 3px;
		background: transparent;
		color: var(--color-destructive);
		cursor: pointer;
	}
	.text {
		flex: 1;
		min-height: 0;
		overflow-y: auto;
		white-space: pre-wrap;
		word-break: break-word;
		font-family: monospace;
		font-size: 0.8rem;
		line-height: 1.3;
		padding: 0.35rem 0.5rem;
		color: var(--color-foreground);
	}
	.inputbar {
		display: flex;
		gap: 0.5rem;
		align-items: center;
		flex-shrink: 0;
		font-family: monospace;
		font-size: 0.8rem;
		border: 1px solid var(--color-border);
		border-radius: 6px;
		padding: 0.35rem 0.6rem;
		background: var(--color-muted);
		color: var(--color-foreground);
	}
	.ib-label { color: var(--color-primary); }
	.ib-input { white-space: pre-wrap; word-break: break-all; }
	.ib-hint { margin-left: auto; opacity: 0.6; font-size: 0.72rem; }
</style>
