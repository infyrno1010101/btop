---
name: GPU sorting bug
about: Report a problem with NVIDIA per-process GPU or GPU-memory sorting
title: "[GPU sorting] "
labels: bug
assignees: ""
---

**Describe the problem**

Explain what happened and what you expected instead.

**Sort mode**

- [ ] `gpu`
- [ ] `gpu memory`
- [ ] Both

**Environment**

- btop version (`btop --version`):
- Distribution and version:
- Kernel (`uname -r`):
- GPU model:
- NVIDIA driver version:
- Native host, container, virtual machine, or sandbox:

**Driver checks**

Does `nvidia-smi` work for the same user that runs btop?

Paste the relevant rows from `nvidia-smi pmon -c 1`. Remove unrelated or sensitive process information.

```text

```

**Reproduction steps**

1.
2.
3.

**Additional context**

Include relevant btop log messages, configuration details, or screenshots. Do not include secrets or unrelated command-line arguments.
