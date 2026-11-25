#!/bin/sh
# app-slot, tried with the unit's own busybox and openssl (the stock ARM
# binaries, under qemu-arm in the build container): every path of select,
# check, install and confirm, against bundles made by tools/app_bundle.py
# with a throw-away key. Nothing is mounted (APP_SLOT_TEST).
#
#   test-app-slot.sh STOCK_ROOT APP_SLOT BUNDLES_DIR
#
# BUNDLES_DIR holds test.pub and v2.bundle v3.bundle v5.bundle sys9.bundle
# (v2, v3, v5 for system 1; sys9 = version 9 that needs system 9),
# other.bundle (version 4, signed by another key) and broken.bundle (v3 with
# one byte of app.zip changed).
R=$1; SLOT=$2; BUN=$3
T=$(mktemp -d)
export APP_SLOT_TEST=1 B="qemu-arm -L $R $R/bin/busybox" O="qemu-arm -L $R $R/usr/bin/openssl"
export SYS=$T/sys STATE=$T/state W=$T/ram
mkdir -p $SYS $STATE $W
cp $BUN/test.pub $SYS/update-key.pem
echo 1 > $SYS/system-level
echo 1 > $SYS/app-version
fails=0; n=0
run() { $B sh $SLOT "$@" > $T/out 2>&1; rc=$?; }
want() { n=$((n + 1)); if grep -q -- "$1" $T/out; then :; else fails=$((fails + 1)); echo "FAIL $2: wanted '$1', got:"; sed 's/^/    /' $T/out; fi; }
wantrc() { n=$((n + 1)); [ $rc = $1 ] || { fails=$((fails + 1)); echo "FAIL $2: exit $rc, wanted $1"; }; }
ctl() { n=$((n + 1)); [ "$(tr '\n' ' ' < $STATE/autotop/app.ctl)" = "$1 " ] || { fails=$((fails + 1)); echo "FAIL $2: control file is '$(tr '\n' ' ' < $STATE/autotop/app.ctl)', wanted '$1'"; }; }
start() { run select; }

start;                       want "the image's own, version 1" 'nothing kept: the image'
run check $BUN/v2.bundle;    want '^ok 2$' 'check a good bundle'; wantrc 0 'check'
run check $BUN/other.bundle; want 'the signature is not ours' 'another key'; wantrc 1 'another key'
run check $BUN/broken.bundle; want 'the files are not the signed ones' 'changed files'
run check $BUN/sys9.bundle;  want 'it needs system 9, this is 1' 'system too old'
run check $SLOT;             want 'not a bundle' 'not a bundle at all'

run install $BUN/v2.bundle;  want 'version 2 kept: it runs from the next start' 'first install'; wantrc 0 'first install'
ctl 'active - trial a tries 2' 'after the first install'
run status;                  want '^running builtin 1$' 'still the image until the next start'
start;                       want 'the kept bundle, version 2, on trial (1 more)' 'the trial starts'
ctl 'active - trial a tries 1' 'one start used'
[ -x $W/tree/bin/launcherd ] || { fails=$((fails + 1)); echo 'FAIL the trial is unpacked'; }
WATCH=0 run watch;           want 'the trial has not confirmed in 0 s: restarting' 'the watch restarts an unconfirmed trial'
run confirm;                 want 'version 2 confirmed' 'confirm'
WATCH=0 run watch;           wantrc 0 'the watch leaves a confirmed bundle alone'
n=$((n + 1)); [ -s $T/out ] && { fails=$((fails + 1)); echo 'FAIL the watch said something about a confirmed bundle'; }
ctl 'active a trial - tries 0' 'confirmed'
start;                       want '^app: the kept bundle, version 2$' 'the confirmed bundle'

run install $BUN/v2.bundle;  want 'refused: version 2, the unit runs 2' 'the same again'; wantrc 1 'the same again'
ctl 'active a trial - tries 0' 'a refused install leaves what is kept'
run install $BUN/broken.bundle; want 'refused: the files are not the signed ones' 'a damaged upload'
start;                       want '^app: the kept bundle, version 2$' 'a refused install changes nothing'
run install $BUN/v3.bundle;  want 'version 3 kept' 'second install: in place of the first'
ctl 'active - trial a tries 2' 'second install'
n=$((n + 1)); [ "$(ls $STATE/autotop | grep -c bundle)" = 1 ] || { fails=$((fails + 1)); echo 'FAIL more than one bundle is kept'; }
start;                       want 'the kept bundle, version 3, on trial (1 more)' 'trial of 3'
start;                       want 'the kept bundle, version 3, on trial (0 more)' 'second start without a confirm'
start;                       want 'the kept bundle never confirmed: dropped' 'third start: dropped'
want "the image's own, version 1" 'back on the image'
ctl 'active - trial - tries 0' 'the trial is gone'
run confirm;                 wantrc 0 'confirm with no trial does nothing'

run install $BUN/v5.bundle;  want 'version 5 kept' 'third install'
start; run confirm;          ctl 'active a trial - tries 0' '5 confirmed'
# the kept bundle goes bad on the flash: the image's own copy
printf 'x' | dd of=$STATE/autotop/app-a.bundle bs=1 seek=3000 conv=notrunc 2> /dev/null
start;                       want 'the kept bundle is not taken' 'a damaged bundle'; want "the image's own, version 1" 'falls to the image'
# a newer image: its own app is newer than what is kept
cp $BUN/v5.bundle $STATE/autotop/app-a.bundle; echo 7 > $SYS/app-version
start;                       want 'the kept bundle is not taken: version 5, the image has 7' 'a stale bundle'
run install $BUN/v5.bundle;  want 'refused: version 5, the unit runs 7' 'older than the image'
# a cut during an install, after the kept bundle went and before the new one was whole
echo 1 > $SYS/app-version; rm $STATE/autotop/app-a.bundle; cp $BUN/v2.bundle $STATE/autotop/app-a.new
printf 'active -\ntrial -\ntries 0\n' > $STATE/autotop/app.ctl
start;                       want "the image's own, version 1" 'a cut mid-install: the image runs'
run install $BUN/v3.bundle;  want 'version 3 kept' 'and the next install goes through'
n=$((n + 1)); [ ! -e $STATE/autotop/app-a.new ] || { fails=$((fails + 1)); echo 'FAIL the left-over is still there'; }
# no control file, no state at all
rm -rf $STATE/autotop; start; want "the image's own, version 1" 'no control file'

echo "$n checks, $fails failed"
rm -rf $T
[ $fails = 0 ]
