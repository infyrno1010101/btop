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
