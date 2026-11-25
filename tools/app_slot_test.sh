#!/bin/sh
# Make test bundles with throw-away keys and run recovery/plan-a/test-app-slot.sh
# in the build container, where the unit's own busybox and openssl run under
# qemu-arm. Needs one image build to have been made (the stock root in the
# builder's work volume).
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/tree/bin" "$T/bundles"
printf '#!/bin/sh\necho launcher\n' > "$T/tree/bin/launcherd"
for f in plana-up.sh state-save carplay-run; do printf '#!/bin/sh\necho %s\n' $f > "$T/tree/$f"; done
head -c 20000 /dev/urandom > "$T/tree/carplay-icon.png"
openssl genrsa -out "$T/test.key" 2048 2> /dev/null
openssl rsa -in "$T/test.key" -pubout -out "$T/bundles/test.pub" 2> /dev/null
openssl genrsa -out "$T/other.key" 2048 2> /dev/null
for v in 2 3 5; do
    python3 tools/app_bundle.py --tree "$T/tree" --version $v --key "$T/test.key" --check "$T/bundles/test.pub" \
        --out "$T/bundles/v$v.bundle" > /dev/null
done
python3 tools/app_bundle.py --tree "$T/tree" --version 9 --system 9 --key "$T/test.key" --out "$T/bundles/sys9.bundle" > /dev/null
python3 tools/app_bundle.py --tree "$T/tree" --version 4 --key "$T/other.key" --out "$T/bundles/other.bundle" > /dev/null
python3 - "$T/bundles" <<'PY'
import sys
d = bytearray(open(sys.argv[1] + '/v3.bundle', 'rb').read())
d[2560 + 50] ^= 1      # one bit of app.zip (manifest and signature are the first four 512-byte blocks)
open(sys.argv[1] + '/broken.bundle', 'wb').write(d)
PY
docker run --rm --platform linux/amd64 --network none \
    --mount "type=bind,src=$PWD,dst=/project,readonly" --mount "type=bind,src=$T,dst=/t,readonly" \
    --mount type=volume,src=${VOLUME:-autotop-bm168-work},dst=/work ${IMAGE:-autotop-bm168-build:1} \
    sh /project/recovery/plan-a/test-app-slot.sh /work/recovery-sanitized/plana-b-state/stock-stage \
    /project/recovery/plan-a/app-slot /t/bundles
