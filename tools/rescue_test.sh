#!/bin/sh
# Make small rescue containers and run recovery/plan-a/test-init-rescue.sh in
# the build container. Needs `make plana-b-state` to have built rescue-ui.
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
head -c 2000000 /dev/urandom > "$T/kernel"
head -c $((0x6d00000)) /dev/zero > "$T/rootfs"
python3 tools/rescue_image.py --kernel "$T/kernel" --rootfs "$T/rootfs" --out "$T/install.img" --note test > /dev/null
python3 tools/rescue_image.py --kernel "$T/kernel" --rootfs "$T/rootfs" --out "$T/plan.img" --note test --plan-only > /dev/null
{ printf 'AUTOTOP-RESCUE 9\n'; tail -c +18 "$T/install.img"; } > "$T/broken.img"
rm "$T/kernel" "$T/rootfs"
docker run --rm --platform linux/amd64 --network none \
    --mount "type=bind,src=$PWD,dst=/project,readonly" --mount "type=bind,src=$T,dst=/t,readonly" \
    --mount type=volume,src=${VOLUME:-autotop-bm168-work},dst=/work ${IMAGE:-autotop-bm168-build:1} \
    sh /project/recovery/plan-a/test-init-rescue.sh /work/recovery-sanitized/plana-b-state/stock-stage \
    /project/recovery/plan-a/init-rescue /project/dist/plana-b-state/rescue-ui /t
