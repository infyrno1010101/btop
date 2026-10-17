#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "${script_dir}/../.." && pwd)
spec_file="${script_dir}/btop-gpu.spec"
version=$(awk '$1 == "Version:" { print $2; exit }' "${spec_file}")
topdir=${RPM_TOPDIR:-"${repo_dir}/package/rpm/rpmbuild"}
source_name="btop-gpu-v${version}.tar.gz"
archive_root="btop-btop-gpu-v${version}/"

mkdir -p "${topdir}"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
git -C "${repo_dir}" archive \
    --format=tar.gz \
    --prefix="${archive_root}" \
    --output="${topdir}/SOURCES/${source_name}" \
    HEAD
cp "${spec_file}" "${topdir}/SPECS/btop-gpu.spec"

rpmbuild -ba \
    --define "_topdir ${topdir}" \
    "${topdir}/SPECS/btop-gpu.spec"

printf 'RPM packages are in %s/RPMS\n' "${topdir}"
printf 'Source RPM packages are in %s/SRPMS\n' "${topdir}"
