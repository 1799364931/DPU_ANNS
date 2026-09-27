#!/bin/sh
anns_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export LD_LIBRARY_PATH="$anns_root/artifacts/toolchain/linker/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$anns_root/artifacts/toolchain/linker/usr/bin/aarch64-linux-gnu-ld" "$@"
