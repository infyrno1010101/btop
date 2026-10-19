# btop-gpu 1.1.0

Adds AI workload monitoring alongside the existing NVIDIA process sorting.

- Recognizes llama.cpp, Ollama, vLLM, SGLang, and common PyTorch launchers.
- **Shift+W** groups AI jobs and their same-user workers; `ai:` filters the list.
- **Shift+A** toggles a dedicated inference card with a compact fallback for
  short terminals. Wide cards include throughput and queue history.
- Up to eight local server connections, each with separate credentials and
  history. **[** / **]** switches the selected server; the JSON file reloads
  automatically in the background.
- llama.cpp: live slot-based token rates and active-request progress, with
  clearly labeled fallback averages when live information is unavailable.
- vLLM: rates calculated from generated-token counter deltas, request gauges,
  and cumulative prompt/output totals.
- SGLang: explicitly labeled exporter throughput, request gauges, and
  completed-request token totals. Completed tokens never substitute for its
  in-flight throughput gauge.
- Poll failures, token resets, and changed delta-series membership produce
  unavailable samples instead of misleading rate spikes. Polling never submits
  inference requests, discovers servers, or restarts a model server.

## Compatibility and installation

GPU sorting still uses NVIDIA NVML directly; inference monitoring also works
in a CPU-only build. Server connections are optional and default to disabled.
Existing btop configuration remains usable. The fork is identified by
`1.4.7-gpu1.1.0` in `btop --version`.

Packages replace the existing btop executable without installing user
configuration. Close btop and restart it after upgrading. The connection example
is installed under `/usr/share/btop/examples/ai-servers.json`.

See [the guide](../btop-gpu.md) for requirements, key-file configuration,
shortcuts, and metric semantics. The bundled nlohmann/json MIT license is
installed with the program.

## Validation and remaining limits

The GPU and CPU-only builds have 58 automated tests, plus terminal checks for
small layouts, graphs, shortcuts, authentication, reload, and mixed runtimes.
The vLLM and SGLang adapters were tested with mock exporters; real deployments
have not been validated. Rocky Linux package build results are recorded in the
release build artifacts. Containers validate packaging and userspace startup;
GPU hardware behavior on RHEL still needs a suitable host.

Connections currently accept IPv4 loopback HTTP only. Remote hosts/TLS,
Ollama inference telemetry, and mapping a connection to its processes/GPU
allocations remain future work. Workload recognition does not itself provide
Ollama token rates. SGLang gauge freshness depends on the server version.
