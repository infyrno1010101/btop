# Remaining work

The 1.1 series contains GPU process sorting, workload grouping, optional local
llama.cpp/vLLM/SGLang inference adapters, multi-server navigation, and histories.
The 1.2 development branch adds passive Ollama loaded-model/VRAM telemetry and
explicit per-connection PID association with process, GPU, and VRAM totals.

## Validation

- Real vLLM and SGLang deployments, including multi-worker exporters and auth.
  No such server was available for this release; mock tests cover both adapters.
- GPU hardware smoke tests on RHEL 8, 9, and 10. Rocky container builds verify
  compatibility of the packages and their userspace dependencies.

## Optional future features

- Ollama token timing if its upstream API adds passive server-wide counters.
  Current support deliberately reports token rates as unavailable.
- Remote HTTP/TLS endpoints: verified certificates, explicit credentials, bounded
  name resolution, and handling of endpoint changes without blocking rendering.

These items are not advertised as implemented in 1.1.0.
