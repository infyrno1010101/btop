# Arch package

This recipe builds the `btop-gpu-sort-git` pacman package from the exact tested source commit recorded in `_commit` inside `PKGBUILD`.

## Build

```bash
makepkg --cleanbuild --clean
```

## Install

```bash
sudo pacman -U ./btop-gpu-sort-git-*.pkg.tar.zst
```

The package provides and conflicts with `btop`, allowing pacman to replace the repository package while retaining the normal btop executable name and user configuration.

Generated source checkouts, package archives, and build directories are excluded from Git. See the [btop-gpu guide](../../docs/btop-gpu.md) for runtime requirements, verification, updates, troubleshooting, and rollback.
