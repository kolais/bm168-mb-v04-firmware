#!/bin/sh
# init-rescue, run by the unit's own shell (the stock ARM busybox under
# qemu-arm in the build container) against rescue containers made by
# tools/rescue_image.py. rescue-ui's `mem` is the real ARM tool; its screens
# are recorded instead of drawn; nand-restore is a stand-in that says what
# the real one says in each case (its own behaviour has its fixture suite in
# tools/recovery.py). Nothing is mounted and nothing is written.
#
#   test-init-rescue.sh STOCK_ROOT INIT_RESCUE RESCUE_UI DIR
#
# DIR holds install.img and plan.img (the same software, "mode install" and
# "mode plan") and broken.img (install.img with its first line changed).
R=$1; INIT=$2; UI=$3; DIR=$4
X=$(mktemp -d)
fails=0; n=0
cat > $X/ui <<SH
#!/bin/sh
case "\$1" in
show) shift; echo "\$*" >> $X/screens ;;
mem) exec qemu-arm $UI "\$@" ;;
esac
exit 0
SH
cat > $X/nand <<SH
#!/bin/sh
echo "\$*" >> $X/calls
qemu-arm $R/bin/busybox cp "\$4" $X/manifest.seen 2> /dev/null || cp "\$4" $X/manifest.seen
i=0
case "\$1-\$STUB" in
plan-stop) echo 'STOP image block 3 hash differs from manifest and more words follow here'; exit 1 ;;
plan-same) while [ \$i -lt 904 ]; do echo "PLAN block \$i same=1 bad=0 marker0=ff marker1=ff"; i=\$((i + 1)); done ;;
plan-*) while [ \$i -lt 904 ]; do [ \$i -lt 300 ] && s=1 || s=0; echo "PLAN block \$i same=\$s bad=0 marker0=ff marker1=ff"; i=\$((i + 1)); done ;;
execute-fail) while [ \$i -lt 10 ]; do echo "VERIFIED block=\$i ecc_corrected=0"; i=\$((i + 1)); done; echo 'STOP verify raw page differs at block 10'; exit 1 ;;
execute-*) while [ \$i -lt 904 ]; do echo "VERIFIED block=\$i ecc_corrected=0"; i=\$((i + 1)); done; echo 'RESULT PASS blocks=904 written_from=0 programmed_pages=43108 (writer disarmed)' ;;
esac
[ "\$1" = plan ] && echo 'PLAN_OK blocks=904 (no erase or program issued)'
exit 0
SH
chmod 755 $X/ui $X/nand
printf 'mtd13: 00600000 00020000 "userdata"\nmtd14: 07100000 00020000 "autotop-write-window"\n' > $X/mtd
run() {    # run STUB [SOURCE]
    rm -rf $X/t $X/screens $X/calls $X/manifest.seen
    mkdir $X/t
    echo 'window=0x160000+0x7100000 armed=0 erases=0 programs=0 rejects=0' > $X/writer
    STUB=$1 RESCUE_SRC=$2 RESCUE_TEST=1 RESCUE_PACE=0 B="qemu-arm -L $R $R/bin/busybox" U=$X/ui N=$X/nand W=$X/writer \
        MTD=$X/mtd T=$X/t qemu-arm -L $R $R/bin/busybox sh $INIT > $X/out 2>&1
    rc=$?
    touch $X/screens $X/calls
}
want() { n=$((n + 1)); grep -q -- "$1" "$X/$2" || { fails=$((fails + 1)); echo "FAIL $3: wanted '$1' in $2:"; sed 's/^/    /' "$X/$2" | tail -8; }; }
wantnot() { n=$((n + 1)); if grep -q -- "$1" "$X/$2"; then fails=$((fails + 1)); echo "FAIL $3: '$1' in $2"; fi; }

run differ;                      want 'AUTOTOP_RESCUE_FAILED: No software on the stick' out 'nothing in RAM, no stick'
want 'Rescue failed No software on the stick' screens 'the screen says so'; wantnot . calls 'the writer is not asked'
run differ $DIR/broken.img;      want 'AUTOTOP_RESCUE_FAILED: No software on the stick' out 'not a container'

run differ $DIR/plan.img;        want 'AUTOTOP_RESCUE_PLAN_ONLY blocks=904 same=300' out 'a check-only container'
want '604 of 904 blocks would change' screens 'check only: the count'; wantnot '^execute' calls 'check only: no write'
want "^plan /dev/mtd14 $DIR/plan.img@73728 $X/t/rescue.man\$" calls 'the plan is asked with the window, the image inside the container and the manifest'
n=$((n + 1)); [ "$(sha256sum < $X/manifest.seen | cut -c1-64)" = "$(sed -n 's/^manifest_sha256 //p' $DIR/plan.img | head -n 1 | tr -d '\0')" ] ||
    { fails=$((fails + 1)); echo 'FAIL the manifest handed over is not the container'"'"'s'; }
want 'armed=0' writer 'check only: never armed'

run same $DIR/install.img;       want AUTOTOP_RESCUE_SAME out 'the flash already holds it'
want 'This software is already installed' screens 'already installed: the screen'; wantnot '^execute' calls 'already installed: no write'

run differ $DIR/install.img;     want 'AUTOTOP_RESCUE_DONE RESULT PASS blocks=904' out 'an install'
want '^arm 0x160000 0x7100000$' writer 'the window is armed as the container says'
want "^execute /dev/mtd14 $DIR/install.img@73728 $X/t/rescue.man\$" calls 'the write is asked'
want '^100 Done Remove the stick' screens 'done: the screen'
want 'Checking the software' screens 'the check is announced first'
n=$((n + 1)); [ $rc = 0 ] || { fails=$((fails + 1)); echo "FAIL an install ends with exit $rc"; }

run stop $DIR/install.img;       want 'AUTOTOP_RESCUE_FAILED: image block 3 hash differs from manifest and more words follow here$' out 'the check stops: the console has the whole reason'
want 'Rescue failed image block 3 hash differs from manife Nothing more' screens 'the screen has one line of it'
wantnot '^arm' writer 'a stopped check never arms'; wantnot '^execute' calls 'a stopped check never writes'

run fail $DIR/install.img;       want 'AUTOTOP_RESCUE_FAILED: verify raw page differs at block 10' out 'the write stops'
want 'Rescue failed verify raw page differs at block 10' screens 'the write stops: the screen'
wantnot 'AUTOTOP_RESCUE_DONE' out 'a stopped write is not called done'

echo "$n checks, $fails failed"
rm -rf $X
[ $fails = 0 ]
