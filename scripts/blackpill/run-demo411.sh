#!/bin/sh
# Run demo411 using the QEMU and firmware builds in this worktree.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
firmware=${1:-"$repo_dir/build/demo411/DemoRTOSProject.elf"}
if [ "$#" -gt 0 ]; then shift; fi
qemu="$repo_dir/build/qemu-system-arm"
if [ ! -x "$qemu" ] || [ ! -f "$firmware" ]; then
    echo "Missing QEMU binary or firmware; see docs/system/arm/blackpill.rst." >&2
    exit 1
fi
exec "$qemu" -M blackpill-f411ce -kernel "$firmware" \
    -display none -monitor none -serial stdio "$@"
