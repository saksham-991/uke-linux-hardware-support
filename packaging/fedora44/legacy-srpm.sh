#!/usr/bin/env bash
set -Eeuo pipefail

component=${1:?component required}
destination=${2:?absolute output directory required}
case $component in
    uke-boot|uke-core-meta|uke-platform-runtime) ;;
    *) echo "Unsupported Fedora 44 source component: $component" >&2; exit 2;;
esac

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
# Last Fedora 44 hardware snapshot before the upstream 1.8 boot integration
# combined the touch daemon with the boot package. Keep TM touch in its
# separate, tested package until both runtime paths have been qualified.
source_commit=9f6ad347d4b45e52096022f9da3aeb677a888b8e
if ! git -C "$repo" cat-file -e "$source_commit^{commit}" 2>/dev/null; then
    git -C "$repo" fetch --depth=1 origin "$source_commit" || git -C "$repo" fetch origin legacy-1.5.2 || true
fi
git -C "$repo" -c "safe.directory=$repo" cat-file -e "$source_commit^{commit}"
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
paths=("$component")
if [[ $component == uke-boot ]]; then
    paths+=(uke-desktop-integration packaging/families)
fi
git -C "$repo" -c "safe.directory=$repo" archive "$source_commit" "${paths[@]}" | tar -xf - -C "$tmp"
make -C "$tmp/$component" FEDORA44_LEGACY=0 srpm outdir="$destination"
