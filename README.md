# llama.cpp

![llama](https://raw.githubusercontent.com/ggml-org/llama.brand/refs/heads/master/cover/llama-cpp/cover-llama-cpp-dark.svg)

<div align="center">

<b>LLM inference in C/C++</b>

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Release](https://img.shields.io/github/v/release/ggml-org/llama.cpp)](https://github.com/ggml-org/llama.cpp/releases)
[![Server](https://github.com/ggml-org/llama.cpp/actions/workflows/server.yml/badge.svg)](https://github.com/ggml-org/llama.cpp/actions/workflows/server.yml)
[![Docker](https://github.com/ggml-org/llama.cpp/actions/workflows/docker.yml/badge.svg)](https://github.com/ggml-org/llama.cpp/actions/workflows/docker.yml)
[![Winget](https://github.com/ggml-org/llama.cpp/actions/workflows/winget.yml/badge.svg)](https://github.com/ggml-org/llama.cpp/actions/workflows/winget.yml)

[manifesto](https://github.com/ggml-org/llama.cpp/discussions/205) / [ggml](https://github.com/ggml-org/ggml) / [ops](https://github.com/ggml-org/llama.cpp/blob/master/docs/ops.md) / [maintainer PRs](https://github.com/ggml-org/llama.cpp/issues?q=is%3Apr%20is%3Aopen%20draft%3AFalse%20(author%3Argerganov%20OR%20author%3AKitaitiMakoto%20OR%20author%3Adanbev%20OR%20author%3Aaldehir%20OR%20author%3Amax-krasnyansky%20OR%20author%3ACISC%20OR%20author%3Aggerganov%20OR%20author%3Aam17an%20OR%20author%3Abartowski1182%20OR%20author%3Ahipudding%20OR%20author%3AServeurpersoCom%20OR%20author%3Apwilkin%20OR%20author%3Areeselevine%20OR%20author%3Angxson%20OR%20author%3Ajeffbolznv%20OR%20author%3A0cc4m%20OR%20author%3Aangt%20OR%20author%3AIMbackK%20OR%20author%3Aarthw%20OR%20author%3AJohannesGaessler%20OR%20author%3AORippler%20OR%20author%3Aruixiang63%20OR%20author%3Axctan%20OR%20author%3Aallozaur%20OR%20author%3Ayomaytk%20OR%20author%3Aaendk%20OR%20author%3Agaugarg-nv%20OR%20author%3Ataronaeo%20OR%20author%3Aforforever73%20OR%20author%3Alhez%20OR%20author%3Anetrunnereve%20OR%20author%3Afairydreaming)%20sort%3Aupdated-desc) / [compile times](https://github.com/ggml-org/llama.cpp-dev/blob/master/README-compile-times.md) / [lib llama API](https://github.com/ggml-org/llama.cpp/issues/9289) / [llama-server REST API](https://github.com/ggml-org/llama.cpp/issues/9291)

</div>

## This fork: three ways to watch your server live

> This fork's differentiator from upstream llama.cpp: live per-sequence monitoring built into `llama-server`. Three UIs - a pipe-safe plain-text stream (`--printui N`), a real interactive ncurses terminal UI (`--tui`), and a Dashboard tab in the built-in web UI - all read the **same shared producer** (`dash::feed` + a status snapshot), so they show identical information, just in different shells. Docs: [design](docs/dashboard/DESIGN.md) / [printui](docs/dashboard/IMPLEMENTATION.md) / [web](docs/dashboard/WEB.md).

### 1. Plain-text stream: `--printui N`

**What it is** - a timely-output plain-text frame written to stdout every second, exactly `N` lines. No ncurses, no terminal control: it is safe to pipe into a file, `less -R`, or any log collector.

**Use case** - headless / SSH / cron / CI monitoring; keeping a rolling server log; scripting around live status (grep the tagged lines); terminals without a TTY.

**Usage**

```sh
llama-server -m model.gguf --printui 30                # 30-line frame every second on stdout
llama-server -m model.gguf --printui 30 2>&1 | less -R # or pipe it anywhere
llama-server -m model.gguf --printui 0                 # off (default)
```

**Example frame** (model identity; run + throughput + speculative; pp/tg latency + used KV/weights/cache/rss; lifetime totals; GPU/CPU utilization; then one tagged block per sequence with a live tail):

```text
[SERVER] qwen35 0.8B Q5_K - Medium (app)  ctx 4096/262144  split  KV f16/f16  FA auto  up 2.3h
[RUN] busy 2/4  queue 1  engine 62%  req 0.8/s  prompt 312/s  gen 45/s  hit 68%
[MEM] pp 3ms  tg 22ms  ftok 0.41s  kv gpu 0  kv cpu 24MB  weights 580MB  cache 0  rss 1.2GB
[TOTAL] prompt 120  gen 45  cached 80  decode 51
[GPU] SM 63%  mem 41%  pwr 245W  t 68C  pclk 1.9G  mclk 1.6G  pcie 80M/s
[SYSTEM] cpu 320%  ioR 45M/s  ioW 1M/s  req q 0.3s pp 1.2s dec 8.4s
[SEQ 0] PF G kv:##...... len:2048/8192 cch:900 pp:312 tg:-  q:0.3s dec:136 rem:500 t:100
Once upon a time in a land far away, there lived a brave knight who...
[SEQ 1] DEC G kv:######## len:8123/8192 cch:- pp:- tg:45  q:- dec:8123 rem:- t:101
```

### 2. Interactive terminal UI: `--tui`

**What it is** - a box-drawing (ncurses) dashboard on the alternate screen: a grid of per-sequence cells with live token push, markdown + code highlighting, and full mouse/keyboard control. Resize-aware and smooth (only changed cells are redrawn).

**Use case** - interactive terminal sessions where you want to watch and drive many conversations at once: scroll each sequence, maximize one, type a raw completion straight into a sequence, kill a runaway generation, save a transcript - all without leaving the terminal.

**Usage**

```sh
llama-server -m model.gguf --tui                    # interactive terminal dashboard (needs a TTY)
llama-server -m model.gguf --tui --tui-ratio 0.30   # fixed cell aspect (default: auto = terminal aspect)
```

**Example screen** (global header, then a grid of boxed sequences, then the status bar):

```text
┌ qwen35 0.8B Q5_K - Medium (app)  ctx 4096/262144  split  KV f16/f16  FA auto ┐
│ busy 2/4  queue 1  engine 62%  req 0.8/s  prompt 312/s  gen 45/s  hit 68%   │
│ pp 3ms  tg 22ms  ftok 0.41s  kv gpu 0  kv cpu 24MB  weights 580MB  rss 1.2GB  │
│ total prompt 120  gen 45  cached 80  decode 51  ratio 0.21                    │
┌─────────────────────────────────┐┌────────────────────────────────┐
│ SEQ 2 DEC  kv:300/1024 pp:- tg:45││ SEQ 3 PF   kv:20/1024 pp:312 tg:-│
│ The knight rode onward through  ││ The user asks about the server  │
│ the misty valley, and...        ││ status while the prompt loads... │
└─────────────────────────────────┘└────────────────────────────────┘
llama-server  [q] detach  [L/R] select  [↑/↓] scroll  [[/]] ratio  [i] input  [k] kill  [s] save
```

**Controls**

| Key / mouse | Action |
|---|---|
| mouse wheel | scroll the cell under the cursor (pinned at the bottom auto-follows; scroll up holds) |
| click / double-click | select / maximize + restore a sequence |
| `Left`/`Right` | move selection |
| `Up`/`Down` | scroll the selected cell |
| `Enter` | maximize / restore |
| `[` / `]` | tune the grid cell aspect ratio live (shown in the header) |
| `i` | open the bottom input bar: type a raw completion, `Enter` sends it to that sequence, `Shift+Enter` newline, `Esc` cancels |
| `k` | abort the selected sequence's running completion (ends cleanly like end-of-stream, client gets the partial text) |
| `s` | save the selected sequence's text to `yymmddhhmmss.txt` |
| `q` | detach the TUI (server keeps running) |
| `Ctrl-C` | stop the server |

### 3. Web dashboard (built-in web UI)

**What it is** - a Dashboard tab in the built-in web UI, **on by default** (no switch). It is the same UI/UX as the ncurses TUI, rendered in the browser: same global header, same HxW grid (same `grid_dims` objective, auto ratio), same per-sequence text with live push and scroll-follow, same bottom input bar, per-cell kill, and `[`/`]` ratio tuning. Cached/evicted sequences appear in a flat list.

**Use case** - when a browser is more convenient than a terminal: check on the server from anywhere, drive it from the phone/desktop, and keep the exact same mental model as `--tui`.

**Usage**

```sh
llama-server -m model.gguf                # web UI (with dashboard tab) is on by default
# open http://127.0.0.1:8080/dashboard
```

**Example** - the dashboard mirrors the terminal screens above: header rows (model / run+throughput / memory / totals+ratio), then a grid of `[SEQ n]` cells each with a scrollable live text area, a `kill` button while a sequence is generating, and the bottom input bar for firing raw completions into the selected sequence.

### Notes

- `--printui` and `--tui` both write stdout, so they cannot be used together (startup error). The web dashboard has no such restriction.
- All three share one producer: the same sequence feed and status snapshot drive printui, the ncurses TUI, and the SSE-backed web dashboard, so the numbers always agree.

## Quick start

A few options to get `llama.cpp` installed on your machine:

- Visit https://llama.app and follow the instructions
- Run with Docker - see our [Docker documentation](docs/docker.md)
- Download pre-built binaries from the [releases page](https://github.com/ggml-org/llama.cpp/releases)
- Build from source by cloning this repository - check out [our build guide](docs/build.md)

Once installed:

```sh
# Download and run a model directly from Hugging Face
llama cli -hf ggml-org/Qwen3.5-0.8B-GGUF

# Launch OpenAI-compatible API server
llama serve -hf ggml-org/Qwen3.5-0.8B-GGUF
```

<table align="center">
    <tr>
        <td align="center" width=50%>
            <img width="1310" height="888" alt="VLM session with `llama cli`" src="https://github.com/user-attachments/assets/88726b48-1713-48aa-a525-95a02e78afc4" />
            <i>VLM session with <b>llama cli</b></i>
        </td>
        <td align="center">
            <img width="1392" height="958" alt="Built-in web UI against `llama serve` running Qwen 3.6" src="https://github.com/user-attachments/assets/b402f972-2e32-4def-8771-8d849f08cf2e" />
            <i>Built-in web UI against <b>llama serve</b></i>
        </td>
    </tr>
<table>

## Description

The main goal of `llama.cpp` is to enable LLM (and VLM) inference with minimal setup and state-of-the-art performance on
a wide range of hardware - locally and in the cloud.

- Plain C/C++ implementation without any dependencies
- Apple silicon is a first-class citizen - optimized via ARM NEON, Accelerate and Metal frameworks
- AVX, AVX2, AVX512 and AMX support for x86 architectures
- RVV, ZVFH, ZFH, ZICBOP and ZIHINTPAUSE support for RISC-V architectures
- 1.5-bit, 2-bit, 3-bit, 4-bit, 5-bit, 6-bit, and 8-bit integer quantization for faster inference and reduced memory use
- Custom CUDA kernels for running LLMs on NVIDIA GPUs (support for AMD GPUs via HIP and Moore Threads GPUs via MUSA)
- Vulkan and SYCL backend support
- CPU+GPU hybrid inference to partially accelerate models larger than the total VRAM capacity
- Three live server monitors for `llama-server`, all sharing one producer: a pipe-safe plain-text stream (`--printui N`, N = lines per frame, 0 = off) with tagged per-sequence blocks plus memory/throughput/GPU utilization; an interactive ncurses terminal dashboard (`--tui`) with mouse/keyboard, per-sequence scroll + maximize, a raw-completion input bar, and `k` to abort a generation; and a Dashboard tab in the built-in web UI that mirrors the ncurses TUI.

The `llama.cpp` project is build on top of the [ggml](https://github.com/ggml-org/ggml) library.

## Supported backends

| Backend | Target devices |
| --- | --- |
| [BLAS](docs/build.md#blas-build) | All |
| [BLIS](docs/backend/BLIS.md) | All |
| [CANN](docs/build.md#cann) | Ascend NPU |
| [CUDA](docs/build.md#cuda) | Nvidia GPU |
| [HIP](docs/build.md#hip) | AMD GPU |
| [Hexagon [In Progress]](docs/backend/snapdragon/README.md) | Snapdragon |
| [IBM zDNN](docs/backend/zDNN.md) | IBM Z & LinuxONE |
| [MUSA](docs/build.md#musa) | Moore Threads GPU |
| [Metal](docs/build.md#metal-build) | Apple Silicon |
| [OpenCL](docs/backend/OPENCL.md) | Adreno GPU |
| [OpenVINO [In Progress]](docs/backend/OPENVINO.md) | Intel CPUs, GPUs, and NPUs |
| [RPC](https://github.com/ggml-org/llama.cpp/tree/master/tools/rpc) | All |
| [SYCL](docs/backend/SYCL.md) | Intel GPU |
| [VirtGPU](docs/backend/VirtGPU.md) | VirtGPU APIR |
| [Vulkan](docs/build.md#vulkan) | GPU |
| [WebGPU](docs/build.md#webgpu) | All |
| [ZenDNN](docs/build.md#zendnn) | AMD CPU |

## Documentation

#### Tools

- [cli](tools/cli/README.md)
- [completion](tools/completion/README.md)
- [server](tools/server/README.md)
- [GBNF grammars](grammars/README.md)

#### Development

- [How to build](docs/build.md)
- [Running on Docker](docs/docker.md)
- [Build on Android](docs/android.md)
- [Multi-GPU usage](docs/multi-gpu.md)
- [Performance troubleshooting](docs/development/token_generation_performance_tips.md)
- [GGML tips & tricks](https://github.com/ggml-org/llama.cpp/wiki/GGML-Tips-&-Tricks)
- [XCFramework](docs/xcframework.md)
- [Completions](docs/completions.md)
- [Models](docs/models.md)
- [Release process](docs/release.md)

## Contributing

- Contributors can open PRs
- Collaborators will be invited based on contributions
- Maintainers can push to branches in the `llama.cpp` repo and merge PRs into the `master` branch
- Any help with managing issues, PRs and projects is very appreciated!
- Read the [CONTRIBUTING.md](CONTRIBUTING.md) for more information

## Acknowledgements

- [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) - Single-header HTTP server, used by `llama-server` - MIT license
- [stb-image](https://github.com/nothings/stb) - Single-header image format decoder, used by multimodal subsystem - Public domain
- [nlohmann/json](https://github.com/nlohmann/json) - Single-header JSON library, used by various tools/examples - MIT License
- [miniaudio.h](https://github.com/mackron/miniaudio) - Single-header audio format decoder, used by multimodal subsystem - Public domain
- [subprocess.h](https://github.com/sheredom/subprocess.h) - Single-header process launching solution for C and C++ - Public domain
