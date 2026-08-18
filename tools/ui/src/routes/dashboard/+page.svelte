<script lang="ts">
	// Live sequence dashboard (docs/dashboard/WEB.md).
	// Connects to the server's /dashboard SSE stream.
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
	// reactive array (deep-proxied by Svelte 5); Map mutation is NOT reactive
	let cells = $state<Cell[]>([]);
	let refs = $state<Record<number, HTMLDivElement>>({});
	let pinned = $state<Record<number, boolean>>({});

	function applySnapshot(s: { global: Global; slots: Slot[]; cells: Cell[] }) {
		global = s.global;
		slots = s.slots ?? [];
		cells = s.cells ?? [];
	}

	function onDelta(id: number, seq: number, text: string) {
		const c = cells.find((x) => x.id === id);
		if (!c || !text) return;
		if (seq <= c.seq) return; // dedup by token seq
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
	}

	onMount(() => {
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
		return () => es?.close();
	});

	function onCellScroll(id: number, el: HTMLDivElement) {
		pinned[id] = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
	}

	// terminal-emulator scroll-follow: keep pinned cells at the bottom on updates
	$effect(() => {
		for (const c of [...cells]) {
			const el = refs[c.id];
			if (el && pinned[c.id] !== false) el.scrollTop = el.scrollHeight;
		}
	});

	const activeCells = $derived(cells.filter((c) => !c.evicted));
	const cachedCells = $derived(cells.filter((c) => c.evicted));
	const gridCols = $derived(
		activeCells.length === 0
			? 1
			: Math.ceil(activeCells.length / Math.floor(Math.sqrt(activeCells.length)))
	);
	const gridRows = $derived(
		activeCells.length === 0 ? 0 : Math.ceil(activeCells.length / gridCols)
	);

	function slotFor(id: number): Slot | undefined {
		return slots.find((s) => s.id === id);
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
				<span class="k">spec</span> {fmtPct(global.spec_acc)}
				<span class="k">hit</span> {fmtPct(global.hit_rate)}
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
			</div>
		</div>
	{/if}

	{#if activeCells.length === 0 && cachedCells.length === 0}
		<p class="empty">No sequences yet. Send a request to the server.</p>
	{:else}
		<div class="grid" style={`grid-template-columns: repeat(${gridCols}, 1fr); grid-template-rows: repeat(${Math.max(gridRows, 1)}, minmax(0, 1fr));`}>
			{#each activeCells as c (c.id)}
				{@const s = slotFor(c.id)}
				<div class="cell">
					<div class="head">
						[SEQ {c.id}] {s ? s.phase : c.active ? 'ACTIVE' : 'IDLE'} loc:{s?.loc ?? '-'}
						kv:{s ? `${s.kv_used}/${s.n_ctx}` : '-'} cch:{s?.n_prompt_cached ?? '-'}
						pp:{fmtNum(s?.pp_tps)} tg:{fmtNum(s?.tg_tps)} pp5:{fmtNum(s?.pp5)} tg5:{fmtNum(s?.tg5)}
						hit:{fmtPct(c.hit)} created:{fmtTime(c.created)}
					</div>
					<div
						class="text"
						bind:this={refs[c.id]}
						onscroll={(e) => onCellScroll(c.id, e.currentTarget as HTMLDivElement)}
					>{c.text}</div>
				</div>
			{/each}
		</div>

		{#if cachedCells.length > 0}
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
</style>
