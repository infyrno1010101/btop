# Remaining work

The 1.1 series contains GPU process sorting, workload grouping, optional local
llama.cpp/vLLM/SGLang inference adapters, multi-server navigation, and histories.

## Validation

- Real vLLM and SGLang deployments, including multi-worker exporters and auth.
  No such server was available for this release; mock tests cover both adapters.
- GPU hardware smoke tests on RHEL 8, 9, and 10. Rocky container builds verify
  compatibility of the packages and their userspace dependencies.

## Optional future features

- Ollama inference telemetry: first establish a passive source for token timing.
  Monitoring must not create inference requests merely to obtain statistics.
- Remote HTTP/TLS endpoints: verified certificates, explicit credentials, bounded
  name resolution, and handling of endpoint changes without blocking rendering.
- Explicit connection-to-process association and per-server GPU/VRAM totals.
  Avoid guessing ownership from a shared port or attributing unrelated jobs.

These items are not advertised as implemented in 1.1.0.
