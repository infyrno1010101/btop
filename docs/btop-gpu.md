# btop-gpu

`btop-gpu` is a Linux-focused fork of [btop](https://github.com/aristocratos/btop) that adds NVIDIA GPU activity to the process list. It keeps the `btop` executable name and existing configuration format, so it works as a direct replacement for an ordinary GPU-enabled btop installation.

## Added process sort modes

| Sort mode | Value | Process-list display |
| --- | --- | --- |
| `gpu` | NVIDIA SM utilization attributed to each process | Replaces the numeric `Cpu%` value with `Gpu%` while selected |
| `gpu memory` | NVIDIA framebuffer memory attributed to each process | Replaces the memory value with `GpuB` while selected |

The modes are added to the process sort menu only when their corresponding NVML functions are present. Selecting either mode activates GPU collection even when no GPU panel is visible. Values from multiple NVIDIA GPUs are combined for each PID. Process-tree parents include the values of their descendants when the tree is collapsed.

The implementation calls the NVIDIA Management Library directly. It does not spawn or parse `nvidia-smi`, and it does not require a background service or plugin host.

## Requirements

### Runtime

- Linux. The per-process additions are currently implemented in the Linux NVIDIA backend.
- An NVIDIA GPU supported by the installed NVIDIA driver.
- An official NVIDIA proprietary or open kernel-module driver.
- `libnvidia-ml.so`, normally installed as part of the NVIDIA userspace driver package.
- Permission to access the NVIDIA device nodes and NVML. A normal desktop user with a working `nvidia-smi` installation usually already has this.
- A btop build with GPU support enabled. The included Arch package and the default x86_64 Linux build enable it.

Existing upstream GPU panels retain their NVIDIA, AMD ROCm SMI, and Intel support. The two new **per-process** sort modes currently receive data from NVIDIA NVML only.

### Build

- A C++23 compiler supported by btop: GCC 14 or newer, or Clang 19 or newer.
- CMake and a build tool, or GNU Make plus the standard tools documented by upstream btop.
- Git when building the Arch package recipe.
- The normal btop development dependencies for the target distribution.

## Arch, Garuda, and Arch-derived distributions

Clone the fork and check out the feature branch:

```bash
git clone https://github.com/infyrno1010101/btop.git
cd btop
git switch gpu-process-sorting
```

Build the pacman package:

```bash
cd package/arch
makepkg --cleanbuild --clean
```

Install it with pacman:

```bash
sudo pacman -U ./btop-gpu-sort-git-*.pkg.tar.zst
```

The package declares that it provides and conflicts with `btop`. Pacman will ask to replace the repository `btop` package. Configuration under `~/.config/btop` is preserved.

To rebuild after the pinned source commit or package release changes:

```bash
git pull --ff-only
cd package/arch
makepkg --cleanbuild --clean --force
sudo pacman -U ./btop-gpu-sort-git-*.pkg.tar.zst
```

## Red Hat Enterprise Linux and compatible distributions

An RPM recipe is included for x86_64 systems. It is continuously built on
Rocky Linux 8, 9, and 10, which exercise the corresponding RHEL package interfaces.

- RHEL 10 uses the distribution GCC 14 compiler.
- RHEL 8 and 9 use GCC Toolset 14 from AppStream.
- RHEL 8 statically links the GCC C++ runtime because its system `libstdc++`
  predates the C++23 features required by current btop. NVML remains dynamic.

Install the build dependencies and create the RPM by following
[`package/rpm/README.md`](../package/rpm/README.md). From an already prepared
build host, the complete build command is:

```bash
./package/rpm/build-rpm.sh
```

Install the resulting package with DNF:

```bash
sudo dnf install ./package/rpm/rpmbuild/RPMS/x86_64/btop-gpu-*.rpm
```

The RPM uses the standard `btop` executable and declares the package metadata
needed for DNF to replace a distribution `btop` installation. The NVIDIA driver
and NVML remain runtime requirements. They are not hard-coded as an RPM
dependency because the exact driver package name depends on the configured
NVIDIA or distribution repository.

## Build and install manually

Using CMake:

```bash
cmake -S . -B build \
  -DBTOP_GPU=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel
sudo cmake --install build
```

Using the upstream Makefile:

```bash
make -j"$(nproc)" GPU_SUPPORT=true
sudo make install GPU_SUPPORT=true
```

Package-manager installation is recommended because pacman can verify, upgrade, and remove every installed file.

## Usage

Start the installed program normally:

```bash
btop
```

Open the process options menu and cycle **Sorting** to one of these values:

- `gpu` for per-process SM activity.
- `gpu memory` for per-process VRAM allocation.

The setting is saved as `proc_sorting` in the normal btop configuration. It can also be set directly:

```ini
proc_sorting = "gpu"
```

or:

```ini
proc_sorting = "gpu memory"
```

## Verification

Confirm that the fork is installed:

```bash
btop --version
```

The version should include the fork commit, such as `1.4.7+a9681f2`, and report `-DBTOP_GPU=ON` or `GPU_SUPPORT=true`.

Confirm that the driver and NVML can enumerate processes:

```bash
nvidia-smi
nvidia-smi pmon -c 1
```

On Arch-based systems, verify the installed package:

```bash
pacman -Q btop-gpu-sort-git
sudo pacman -Qkk btop-gpu-sort-git
```

On RHEL-compatible systems, verify the installed package:

```bash
rpm -q btop-gpu
rpm -V btop-gpu
```

## Troubleshooting

### The GPU sort choices are missing

1. Check `btop --version` and confirm GPU support is enabled.
2. Run `nvidia-smi` as the same user that runs btop.
3. Confirm that the dynamic linker can find NVML:

   ```bash
   ldconfig -p | grep libnvidia-ml
   ```

4. Avoid static builds. NVML is loaded dynamically at runtime.
5. Restart btop after installing or upgrading the NVIDIA driver.

### GPU activity is zero

NVML reports utilization over sampling intervals. An idle process can own VRAM while reporting zero SM utilization. Generate GPU activity, wait for another btop update interval, and compare with `nvidia-smi pmon -c 1`.

### btop works in a terminal but not in a container or sandbox

The environment must expose the NVIDIA device nodes and driver libraries. For containers, use the NVIDIA Container Toolkit or an equivalent configuration. Sandboxes that hide `/dev/nvidia*` cannot collect live NVIDIA statistics even when the host driver is installed correctly.

### GPU memory differs from application estimates

The value is NVML's process framebuffer-memory accounting. Shared graphics allocations, driver overhead, unified memory, and allocation timing can differ from values reported inside an application.

## Remove or return to upstream btop

On Arch-based systems:

```bash
sudo pacman -S btop
```

Pacman will replace `btop-gpu-sort-git` because the packages conflict. To remove btop entirely instead:

```bash
sudo pacman -Rns btop-gpu-sort-git
```

On RHEL-compatible systems, return to the distribution package with:

```bash
sudo dnf swap btop-gpu btop
```

## Current scope

- NVIDIA per-process utilization and VRAM collection through NVML.
- Flat and tree process sorting, including reverse sorting.
- Multi-GPU aggregation by PID.
- Conditional menu entries based on available driver functions.
- Standard upstream GPU panels and behavior remain available.

Per-process AMD and Intel collection is not implemented yet. The shared process-stat structure is backend-neutral so those collectors can be added without redesigning the process list.

## Development and reporting

Use the [GPU sorting bug report](https://github.com/infyrno1010101/btop/issues/new?template=gpu_sorting_bug.md) for fork-specific problems. Include the fork version, driver version, GPU model, selected sort mode, relevant `nvidia-smi` output, and whether the problem reproduces outside a container or sandbox. Remove unrelated or sensitive process arguments before posting logs.

The feature implementation was AI-assisted and is disclosed here in accordance with the upstream contribution policy. It was compiled with GPU support enabled and disabled, passed the upstream test suite in both configurations, and was validated against live NVML process data on an NVIDIA GeForce RTX 3090.

## Upstream and license

This repository is a fork of [aristocratos/btop](https://github.com/aristocratos/btop). The original project and this fork are distributed under the Apache License 2.0; see [LICENSE](../LICENSE).

## AI workload awareness

The process command column and detail view annotate recognized commands with a
runtime and role, for example `[llama.cpp serving]`. Press `f` or `/` and enter
`ai:` to show recognized AI runtimes. Use `ai:vllm`, `ai:serving`, or
`ai:example.gguf` to narrow that list. Clear the filter with Delete.
On Linux, workers inherit their runtime from a recognized launcher through
same-user process ancestry. Their labels identify the launcher PID, and the root
label reports the number of live processes in the job.

Initial signatures cover llama.cpp (`llama-server`, `llama-cli`), Ollama
(`serve`, `runner`), vLLM (`serve` and API server Python modules), SGLang
(`python -m sglang.launch_server`), and PyTorch distributed launchers
(`torchrun`, `python -m torch.distributed.run`). Detection uses the executable
and launch arguments, not arbitrary occurrences of framework names. It works
without GPU support, network requests, extra packages, or a background service.

These are command-line hints, not verified job identities. Generic Python scripts,
renamed binaries, wrapper commands, and some worker processes remain unclassified.
A distributed launcher is labeled `distributed`, not assumed to be training.
Command lines are subject to the collector's existing length and caching limits.
The optional server adapters below provide inference and model telemetry. Ollama
loaded-model metadata is collected from its read-only API; training progress is
not collected.

### AI workload view (Linux)

Press **Shift+W** (`W`) in the process panel to enable the AI workload tree.
It applies the `ai:` filter, turns on tree aggregation, and labels job roots
with their runtime and process count. Select a root and press `-` to collapse
its workers or `+` to expand them. Press `W` again to restore the previous
filter, tree, and aggregation settings. The preset is session-only and restores
those settings before saving configuration on normal exit.

Each job is the topmost recognized AI process in a same-user parent chain.
Nested launchers join that job, while unrelated launchers stay separate. Workers
are inferred from ancestry, not positively identified as AI computation: helpers
and data loaders are included. Exited processes are excluded from member counts;
missing parents, reparented workers, containers, and permission boundaries can
leave a job incomplete. Membership is rebuilt on every process snapshot.

Expanded parent rows already include their descendants. Do not add parent and
child rows together. RAM totals sum process RSS, so shared memory can appear in
multiple processes. VRAM totals sum the driver-reported per-PID allocations;
shared allocations are not deduplicated. GPU activity is the sum of per-process
samples, not a normalized percentage of a job's capacity, and can exceed 100%
across devices. This view does not yet group jobs by scheduler or container ID.

### Optional llama.cpp inference metrics (Linux)

The llama.cpp adapter displays a dedicated inference panel below the process
list when an endpoint is configured:

```text
AI inference [A]
Server: 127.0.0.1:8080
Live: 42.5 tok/s (gen)
Active: 2 | Queue: 1
Output: 180 | Prompt: 240
Avg(total): 39.2 tok/s
```

Press **Shift+A** (`A`) or click the panel title to switch between the panel
and the compact status row. This choice is saved as `ai_panel` and can also be
changed under **Options → Processes → ai_panel**. It does not disable polling.
The panel leaves process actions and navigation on the outer bottom border.
It automatically uses compact mode when fewer than 14 rows remain for the
process list and inference panel combined (after process details). This keeps
normally at least four process rows visible when the full panel is shown;
pause/follow banners reserve one additional row.
The server endpoint identifies the source: these metrics are independent of
the selected process and the AI workload tree. No GPU or VRAM figures are
attributed to that endpoint yet.

#### Throughput and queue history

When the process panel is at least 56 columns wide, the unused right side of
the inference card shows a two-row **tokens/sec history** and a one-row
**queue-depth strip**. Narrower panels retain the full-width numeric display.
No extra key is needed; `A` still toggles the complete card and compact row.

- Each column represents one completed server poll, newest at the right,
  approximately every two seconds. A server retains its last 120 polls in
  memory (roughly four to five minutes); the graph shows the most recent
  columns that fit. Histories start empty when btop starts.
- The two graphs scale independently to their visible peaks, labeled
  `tok/s peak` and `Queue peak`. Heights are relative to those scales, not
  GPU utilization percentages. A changing peak can rescale older columns.
- A dot marks a measured zero, while `?` marks unavailable data or a live-rate
  warm-up sample. A timeout or new request does not become a false zero or
  an interpolated bridge. llama.cpp history uses slot deltas; vLLM uses
  exported-counter deltas. Their fallback averages never enter the graph.
  SGLang history uses its explicitly labeled exporter throughput window.
- Every server keeps a separate history while it is in the connection list,
  including when another server is selected or the card is hidden. Switching
  with `[` and `]` or resizing does not reset it. Changing a server's endpoint,
  runtime, key-file path, or live setting clears its history. Removing a connection
  discards its history. For delta-based rates, a detected restart retains
  older columns with a warm-up gap. SGLang uses the current exporter gauge.
- True TTY mode uses ASCII graph characters. The number of graph samples grows
  only on background polls, independently of UI redraw frequency.

Start a llama.cpp server with its `--metrics` option enabled, then set this
in btop's configuration file (or Options → Processes):

```ini
ai_llama_endpoint = "127.0.0.1:8080"
```

`localhost:8080` is also accepted and connects directly to IPv4 loopback.
The default empty string disables polling. The adapter never scans ports,
reads command-line credentials, restarts a server, or sends inference requests.
An optional key file is read only when explicitly configured.
It performs `GET /metrics` and, by default, `GET /slots` every two seconds on a
background thread. Each request has a 250 ms total network deadline and a
256 KiB response limit (up to 500 ms for both requests). It adds no external
HTTP dependency. Disabling or changing the endpoint, key-file path, or live
setting discards old samples and rate baselines.

#### Live generation and request progress

`ai_llama_live = true` (the default) enables slot polling on the explicitly
configured endpoint, using the same API-key file. Change it under
**Options → Processes → ai_llama_live**, or set it to `false` in the config
to poll only `/metrics`. No extra keybind is needed.

- `live` uses token-counter deltas for active slots divided by the elapsed
  wall-clock time between successful polls. During prompt ingestion it uses
  `n_prompt_tokens_processed` and labels the rate `(prompt)`. Once output tokens
  advance it uses `next_token.n_decoded` and labels the rate `(gen)`. This is
  observed throughput during the polling interval, including stalls; it is not
  the server's cumulative average or an exact per-token timing measurement.
- `out` counts tokens generated by currently active requests. It resets when
  those requests leave their slots. It is separate from the `/metrics`
  cumulative token total.
- `active` counts processing slots, including requests still evaluating prompts.
  `queue` comes from the separate `/metrics` request, so these are nearby
  observations rather than an atomic server snapshot.
- `prompt` sums the reported `n_prompt_tokens_processed` for active requests.
  It counts processed prompt tokens, not a completion percentage or context
  occupancy; cached tokens need not be included. Missing counts show `N/A`.
- Two samples of the same slot and task IDs are needed for speed. Initial
  samples and changes in active membership show `N/A (sampling)`. Counter
  decreases, server start-time changes (when exported), failed slot requests,
  and sampling gaps longer than ten seconds reset the baseline. Idle slots
  show `live 0.0` and `out 0`. Brief requests completed between polls can be
  missed; the cumulative metrics still record them.
- Slot data must include stable `id`, `id_task`, `is_processing`, and
  `next_token.n_decoded` fields. Both the object and one-element array forms
  of `next_token` are supported. Slot polling is limited to 256 slots and
  JSON nesting depth 32. Prompts and generated text are never retained or
  displayed, even if the server includes them in its response.

If `/slots` is disabled, inaccessible, malformed, or unsupported, btop shows
`live N/A` with a reason, clears live counts, and preserves available `/metrics`
averages. Some server versions require `--slots`; do not restart an active
server solely to change this setting. See the
[llama.cpp slot API](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md#get-slots-returns-the-current-slots-processing-state).

#### Exporter averages and fallback

With live polling disabled or unavailable, the status row uses exporter values:

- `gen(avg)` is llama.cpp's exported `llamacpp:predicted_tokens_seconds` gauge,
  its reported average generation throughput. It is not a btop-computed live
  token rate, time to first token, or per-request speed.
- When that gauge is zero or missing, `gen(total)` uses
  `llamacpp:tokens_predicted_total / llamacpp:tokens_predicted_seconds_total`.
  This is the average over accumulated generation work since the server's
  counters began, not the current request's speed. Some llama.cpp versions
  export zero gauges between completions, even during active generation.
  If the counters cannot establish a speed, btop shows `N/A` instead of zero.
- `tokens` is the server's cumulative generated-token counter. Depending on
  llama.cpp's version, counters may update only when a request finishes or is
  cancelled, so an active request can generate tokens before this count changes.
- In this fallback view, `active` and `queue` are `llamacpp:requests_processing` and
  `llamacpp:requests_deferred` from the same scrape.
- Missing fields display `N/A`. Failed polls clear numeric values and show a
  status such as timeout, unreachable, authentication required, or HTTP 404.
- Metrics belong to the explicitly configured server, not automatically to a
  displayed PID or job. The endpoint stays visible to make that distinction clear.
- In compact mode, pause/follow banners take precedence over the metrics row.
  The dedicated inference panel remains visible while pausing or following
  the process list. If needed, widen the window to see long status messages.

This adapter supports up to eight HTTP endpoints on IPv4 loopback, optionally
with separate file-based Bearer authentication. TLS, remote hosts, model
metadata, and llama.cpp router mode are not supported yet. A server must enable
`--metrics` independently; authentication alone does not enable that endpoint.
HTTP 501 is shown as `metrics disabled (enable --metrics)`.

#### Multiple server connections

The existing `ai_llama_endpoint` and `ai_llama_api_key_file` settings continue
to work for one server. For multiple connections, create a JSON file:

```json
[
  {
    "name": "Primary",
    "runtime": "llama.cpp",
    "endpoint": "127.0.0.1:8000",
    "api_key_file": "/path/to/primary-api-key",
    "pid": 12345,
    "live": true
  },
  {
    "name": "Worker",
    "endpoint": "127.0.0.1:8001",
    "api_key_file": "/path/to/worker-api-key",
    "live": true
  }
]
```

Point btop to that file under **Options → Processes → ai_servers_file** or
in the btop config:

```ini
ai_servers_file = "/absolute/path/to/ai-servers.json"
```

A [connections example](examples/ai-servers.json) is included in the repository.
Replace the ports, key-file paths, and optional PIDs with your servers' actual settings.
Omit `api_key_file` or set it to an empty string for an unauthenticated server.
Credentials are never inherited from another connection or from the legacy
single-server key setting. Put only paths in this JSON file; inline API keys
are rejected.

- Press **[** for the previous server or **]** for the next. The panel title
  shows the selected connection, for example `AI inference 2/3 [A]`, and the
  server line shows its endpoint and optional name. The panel's bottom border
  also has clickable previous/next buttons. The same keys work in compact mode,
  whose status row identifies the connection by index, name, and endpoint.
- All configured servers are polled concurrently, including those not currently
  displayed. Each has its own credentials, samples, and rate baseline. Changing
  the displayed server does not reset live rates. There is no combined token
  rate: each card describes only the selected endpoint.
- One slow or failed server does not block the others. Each server retains the
  existing 250 ms deadline per HTTP request. The connection list is checked
  every two seconds on a background thread; loading or editing it does not
  perform file or network I/O on the drawing thread.
- File edits are reloaded automatically. Unchanged endpoints preserve their
  histories, and reordering the file keeps the same endpoint selected. Changing
  its runtime, key-file path or live setting resets that endpoint's baseline. Removing
  the selected server selects the first remaining connection. Selection itself
  is session-only; a fresh session begins at the first connection.
- A nonempty `ai_servers_file` overrides the single-server endpoint and key-file
  settings. An invalid or unreadable file clears connections and displays an
  error; it never silently falls back to other credentials. An empty JSON array
  disables all connections. Clearing the `ai_servers_file` setting restores
  the legacy single-server settings.
- At most eight unique loopback ports are accepted. `localhost` is normalized
  to `127.0.0.1`; aliases of the same port cannot be configured twice. Optional
  names must be unique printable ASCII strings of 1–24 characters. `live`
  defaults to the global `ai_llama_live` setting, with per-connection overrides.
  Key-file paths and the connections-file path must be absolute. Unknown fields,
  oversized files (more than 64 KiB), and deeply nested JSON are rejected.
- An optional positive integer `pid` explicitly associates a connection with a
  process. btop aggregates that process and members of its detected workload job,
  including per-process GPU utilization and VRAM reported by the active GPU
  backend. If the PID exits or is reused outside that job, the panel reports it as
  missing until the connections file is updated. No process is guessed from a
  port, name, or endpoint. GPU percentages are summed process samples and can
  exceed 100% across devices; shared VRAM allocations are not deduplicated.

No servers are discovered, started, or restarted automatically. Configure only
the endpoints you want btop to poll. Server telemetry remains independent of
process mapping; a missing or stale PID does not stop endpoint polling.

#### vLLM connections (Linux)

Set `"runtime": "vllm"` on a connection to read vLLM's `/metrics` endpoint.
Omitting `runtime` selects `"llama.cpp"`; `"sglang"` and `"ollama"` are also
supported as described below. Other runtime names are rejected.
The legacy `ai_llama_endpoint` setting continues to select llama.cpp only.
For example, a mixed connections file can contain:

```json
[
  {"name": "llama", "endpoint": "127.0.0.1:8000"},
  {"name": "vLLM", "runtime": "vllm", "endpoint": "127.0.0.1:8001"}
]
```

Configure `ai_servers_file` with this file's absolute path, then press **[** or
**]** to select a server. No vLLM package is needed on the monitoring host.
The server must expose an accessible Prometheus `/metrics` endpoint; this
adapter never requests `/slots` or submits inference requests. Authentication,
loopback restrictions, deadlines, and background reload work as above.

The panel shows **Rate**, running requests, waiting requests, cumulative output
tokens, and cumulative prompt tokens. It reads `vllm:generation_tokens_total`,
`vllm:prompt_tokens_total`, `vllm:num_requests_running`, and
`vllm:num_requests_waiting`, as documented in the
[official vLLM metrics guide](https://docs.vllm.ai/en/latest/design/metrics/).
Distinct labeled engine/model series are summed within the selected endpoint;
duplicate series or invalid counts reject the scrape. Missing metrics show
`N/A`, rather than zero. Prompt and output totals describe the server's
exported counters, rather than an individual request.

**Rate** is the change in generated tokens divided by elapsed poll time.
It needs two successful readings and reflects the exporter's update cadence;
batched counter updates can make short intervals uneven. `live: false`
disables rate calculation while keeping totals and request gauges. There is
no server-lifetime average substituted for an unavailable rate. The history
graph uses these same rates and queue samples.

Failed scrapes, a decrease in any engine's counter, changed generation-series
labels/membership, a changed process-start timestamp (when exported), and gaps
longer than ten seconds require a fresh rate baseline. A restart that supplies
no start marker and overtakes its old counter between polls cannot be detected.
Changing the configured runtime also clears the connection's baseline and
history. The adapter has mock exporter and terminal integration coverage;
a live vLLM deployment has not been validated here.

#### SGLang connections (Linux)

Add `"runtime": "sglang"` to an entry in `ai_servers_file`, for example:

```json
{"name": "SGLang", "runtime": "sglang", "endpoint": "127.0.0.1:30000"}
```

The file must contain a JSON array of entries. Enable metrics on the SGLang
server with `--enable-metrics`. No SGLang installation is needed for btop.
Use **[** / **]** to select the connection and **Shift+A** to toggle the card.
Bearer key files, loopback restrictions, polling deadlines, and background
configuration reload apply to this adapter too. It polls `/metrics` only.

**Gen(exporter)** and its history use `sglang:gen_throughput`, the server's
reported throughput window. Running and waiting requests come from
`sglang:num_running_reqs` and `sglang:num_queue_reqs`. Output and prompt totals
use `sglang:generation_tokens_total` and `sglang:prompt_tokens_total`.
Distinct labeled series are summed within that endpoint; invalid values and
duplicate series reject the scrape. Missing values show `N/A`; an exported
zero remains zero. `live: false` disables the rate display and rate history,
while retaining totals and request counts.

SGLang's completed-request totals can stay unchanged during generation, so
btop never uses their deltas as a fallback throughput. The gauge's update
cadence and handling of stale windows depend on the server version. A
successful scrape confirms what the exporter reports, rather than establishing
an independent per-token timing measurement. Failed scrapes leave graph gaps.
See the [SGLang metrics implementation](https://github.com/sgl-project/sglang/blob/main/python/sglang/srt/observability/metrics_collector.py).
This adapter has mock exporter and terminal coverage; a live SGLang deployment
has not been validated here.

#### Ollama connections (Linux)

Set `"runtime": "ollama"` to poll Ollama's read-only `GET /api/ps` endpoint:

```json
{
  "name": "Local Ollama",
  "runtime": "ollama",
  "endpoint": "127.0.0.1:11434",
  "pid": 12345
}
```

The panel reports the number of loaded models, the first model name, and the
sum of `size_vram` across loaded models. It ignores model digests and unrelated
response fields. Polling performs only `GET /api/ps`; it never calls generation,
chat, embedding, or model-load endpoints.

Ollama includes `eval_count`, `eval_duration`, and prompt counts in the final
response to an inference request, as described in its
[official usage documentation](https://github.com/ollama/ollama/blob/main/docs/api/usage.mdx).
It does not expose those values as passive server-wide counters through `/api/ps`,
and its [official metrics endpoint request](https://github.com/ollama/ollama/issues/3144)
remains unresolved. btop therefore displays `Tokens: N/A (Ollama has no
passive API)` instead of creating a request, intercepting a user's response, or
estimating token speed from GPU activity. The optional `pid` association still
provides process count, GPU utilization, and VRAM for the Ollama server and its
detected runner children.

#### API-key file

For an API-key-protected server, point btop at an existing key file:

```ini
ai_llama_api_key_file = "/path/to/private/api-key"
```

Store a single Bearer token in that file (an optional trailing newline is
accepted), with owner-only permissions such as `chmod 600`. Use an absolute
path; shell expansions such as `~` or `$HOME` are not performed. Leave the
setting empty for unauthenticated endpoints. Never put the key itself in btop's
configuration or command arguments.

The polling thread reloads the file before each scrape, so replacing a temporary
key takes effect on the next poll. The token is sent only as an Authorization
header to the explicitly configured loopback endpoint. It is not included in
status rows, logs, response summaries, or saved btop configuration. Empty,
invalid, oversized, unreadable, or non-regular key files stop that scrape instead
of silently falling back to an unauthenticated request. Maximum file size is
4096 bytes; tokens use Bearer-token characters and cannot contain internal
whitespace or header control characters. The file is not read while the endpoint
is disabled. The 250 ms deadline applies to network I/O; file reads also occur
on the background thread.
