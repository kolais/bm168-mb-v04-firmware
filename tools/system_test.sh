#!/bin/sh
# Run recovery/plan-a/test-system.sh in the build container, where the
# unit's own busybox runs under qemu-arm. Needs one image build to have been
# made (the stock root in the builder's work volume).
set -e
cd "$(dirname "$0")/.."
docker run --rm --platform linux/amd64 --network none \
    --mount "type=bind,src=$PWD,dst=/project,readonly" \
    --mount type=volume,src=${VOLUME:-autotop-bm168-work},dst=/work ${IMAGE:-autotop-bm168-build:1} \
    sh /project/recovery/plan-a/test-system.sh /work/recovery-sanitized/plana-b-state/stock-stage /project/recovery/plan-a
docker run --rm --platform linux/amd64 --network none \
    --mount "type=bind,src=$PWD,dst=/project,readonly" \
    --mount type=volume,src=${VOLUME:-autotop-bm168-work},dst=/work ${IMAGE:-autotop-bm168-build:1} \
    sh -c 'python3 -c "import sys; sys.path.insert(0, \"/project/tools\"); from pathlib import Path; import build; build.dropbear(Path(\"/work\"))" > /dev/null &&
        sh /project/recovery/plan-a/test-dev-login.sh /work/dropbear-build/dropbearmulti /work/dropbear /work /project/recovery/plan-a'
