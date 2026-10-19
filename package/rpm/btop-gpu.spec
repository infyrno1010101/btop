Name:           btop-gpu
Version:        1.0.2
Release:        1%{?dist}
Summary:        Resource monitor with NVIDIA per-process GPU sorting

%global debug_package %{nil}

License:        Apache-2.0 AND MIT
URL:            https://github.com/infyrno1010101/btop
Source0:        %{url}/archive/refs/tags/btop-gpu-v%{version}.tar.gz

ExclusiveArch:  x86_64
BuildRequires:  cmake >= 3.25
BuildRequires:  make
%if 0%{?rhel} == 8
BuildRequires:  gcc-toolset-14-gcc-c++
%else
%if 0%{?rhel} == 9
BuildRequires:  gcc-toolset-14-gcc-c++
%else
BuildRequires:  gcc-c++ >= 14
%endif
%endif

Provides:       btop = 1.4.7
Conflicts:      btop
Obsoletes:      btop < 1.4.8

%description
btop-gpu is a btop fork that adds NVIDIA per-process GPU utilization and
framebuffer-memory sorting. It uses NVML from the installed NVIDIA driver at
runtime and keeps the btop command and configuration format.

%prep
%autosetup -n btop-btop-gpu-v%{version}

%build
%if 0%{?rhel} == 8
source /opt/rh/gcc-toolset-14/enable
linker_flags="-static-libgcc -static-libstdc++"
%else
%if 0%{?rhel} == 9
source /opt/rh/gcc-toolset-14/enable
linker_flags=""
%else
linker_flags=""
%endif
%endif
cmake -S . -B build-rpm \
    -DBTOP_GPU=ON \
    -DBUILD_TESTING=OFF \
    -DBTOP_LTO=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS="${linker_flags}" \
    -DCMAKE_INSTALL_PREFIX=%{_prefix}
cmake --build build-rpm --parallel %{?_smp_build_ncpus}

%install
DESTDIR=%{buildroot} cmake --install build-rpm
install -Dpm0644 LICENSE %{buildroot}%{_licensedir}/%{name}/LICENSE
install -Dpm0644 docs/btop-gpu.md %{buildroot}%{_docdir}/%{name}/README.md

%files
%license %{_licensedir}/%{name}/LICENSE
%doc %{_docdir}/%{name}/README.md
%{_bindir}/btop
%{_datadir}/applications/btop.desktop
%{_datadir}/icons/hicolor/48x48/apps/btop.png
%{_datadir}/icons/hicolor/scalable/apps/btop.svg
%{_datadir}/btop/

%changelog
* Sat Oct 17 2026 Infyrno <infyrno@users.noreply.github.com> - 1.0.2-1
- Add a RHEL 8 build using GCC Toolset 14.
- Link the GCC runtime statically on RHEL 8 for compatibility with its system
  libstdc++ while retaining dynamic NVML loading.

* Sat Oct 17 2026 Infyrno <infyrno@users.noreply.github.com> - 1.0.1-1
- Add RHEL 9 and RHEL 10 packaging for btop-gpu.
- Add NVIDIA per-process utilization and VRAM sorting.
