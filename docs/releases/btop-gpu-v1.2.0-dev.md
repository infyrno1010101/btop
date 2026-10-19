# btop-gpu 1.2.0 development notes

This development release extends local inference monitoring without generating
requests or guessing which process owns a server.

- Adds `runtime: "ollama"` connections. The adapter polls only Ollama's
  read-only `GET /api/ps` endpoint and reports loaded models and model VRAM.
- Ollama token speed is explicitly `N/A`: Ollama exposes token counts only in
  inference responses, not as passive server-wide counters.
- Adds an optional positive integer `pid` to each `ai_servers_file` entry.
- A mapped connection reports the configured PID, detected workload process
  count, summed per-process GPU utilization, and summed driver-reported VRAM.
- A mapped worker PID resolves to its detected workload root. Missing PIDs stay
  visible as missing and do not stop endpoint polling.
- Monitoring never infers ownership from a port or process name and never sends
  generation, chat, embedding, or model-load requests.

The development build identifies itself as `1.4.7-gpu1.2.0-dev`.
