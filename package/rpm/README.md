# RHEL-compatible RPM package

The spec builds `btop-gpu`, a drop-in replacement for the distribution `btop`
package. It installs the normal `/usr/bin/btop` command and keeps existing user
configuration in `~/.config/btop`.

Supported build targets:

- RHEL 10 and compatible distributions on x86_64, using the system GCC 14
  toolchain.
- RHEL 8 and 9 and compatible distributions on x86_64, using GCC Toolset 14.

The RHEL 8 build links `libstdc++` and `libgcc` into the executable because the
system C++ runtime predates the C++23 features used by current btop. Glibc and
NVML remain dynamically linked, so NVIDIA driver discovery continues to work.

## Install build dependencies

RHEL 10, Rocky Linux 10, or AlmaLinux 10:

```bash
sudo dnf install cmake gcc-c++ git make rpm-build
```

RHEL 8 or 9, Rocky Linux 8 or 9, or AlmaLinux 8 or 9:

```bash
sudo dnf install cmake gcc-toolset-14-gcc-c++ git make rpm-build
```

On subscribed RHEL systems, enable the standard BaseOS and AppStream
repositories if `dnf` cannot find one of these packages.

## Build and install

From the repository root:

```bash
./package/rpm/build-rpm.sh
sudo dnf install ./package/rpm/rpmbuild/RPMS/x86_64/btop-gpu-*.rpm
```

DNF replaces an installed `btop` package because this RPM provides, conflicts
with, and obsoletes the corresponding upstream package. User configuration is
not part of either RPM and remains in place.

The NVIDIA driver is a runtime requirement for the added sort modes rather than
an RPM dependency. NVIDIA publishes different driver package names in its own
repository and in Red Hat-derived repositories. Verify the runtime with:

```bash
nvidia-smi
ldconfig -p | grep libnvidia-ml
btop --version
```

See the [btop-gpu guide](../../docs/btop-gpu.md) for usage and troubleshooting.
