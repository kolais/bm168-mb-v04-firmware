#!/bin/sh
# The system's two start-up scripts, tried with the unit's own shell (the
# stock ARM busybox under qemu-arm, in the build container):
#   dev-access   with stand-ins for the server and its key tool
#   state-mount  with a stand-in busybox whose flash and mount commands
#                fail on demand
# Nothing is mounted and no flash is touched.
#
#   test-system.sh STOCK_ROOT SCRIPTS_DIR
R=$1; P=$2
SH="qemu-arm -L $R $R/bin/busybox sh"
T=$(mktemp -d)
fails=0; n=0
want() { n=$((n + 1)); if grep -q -- "$1" $T/out; then :; else fails=$((fails + 1)); echo "FAIL $2: wanted '$1', got:"; sed 's/^/    /' $T/out; fi; }
wantrc() { n=$((n + 1)); [ $rc = $1 ] || { fails=$((fails + 1)); echo "FAIL $2: exit $rc, wanted $1"; }; }
ok() { n=$((n + 1)); if eval "$1"; then :; else fails=$((fails + 1)); echo "FAIL $2"; fi; }

# ---- dev-access ----
export DEV_ACCESS_TEST=1 B="qemu-arm -L $R $R/bin/busybox" SYS=$T/sys STATE=$T/state T
mkdir -p $SYS $STATE
cat > $SYS/dropbear <<'STUB'
#!/bin/sh
# stand-in: "runs" as a sleeping process whose pid goes into the -P file
while [ $# -gt 0 ]; do case $1 in -P) pid=$2; shift ;; -r) key=$2; shift ;; -c) forced=$2; shift ;; -p) shift ;; *) exit 1 ;; esac; shift; done
[ -f "$T/server-fails" ] && exit 1
[ -s "$key" ] && [ "$forced" = "$SYS/dev-shell" ] || exit 1
sleep 300 & echo $! > "$pid"
echo "$key" > "$T/served-with"
STUB
cat > $SYS/dropbearkey <<'STUB'
#!/bin/sh
while [ $# -gt 0 ]; do case $1 in -f) f=$2; shift ;; esac; shift; done
head -c 64 /dev/urandom > "$f"
STUB
printf '#!/bin/sh\n' > $SYS/dev-shell
chmod 755 $SYS/dropbear $SYS/dropbearkey $SYS/dev-shell
dev() { $SH $P/dev-access "$@" > $T/out 2>&1; rc=$?; }
alive() { [ -s $T/dropbear.pid ] && kill -0 "$(cat $T/dropbear.pid)" 2> /dev/null; }

dev status;  want 'available no' 'no key in the image'; want 'on no' 'off at first'
dev on;      want 'not available: this image has no key' 'on without a key'; wantrc 1 'on without a key'
dev start;   wantrc 0 'start when off'; ok '! alive' 'nothing runs without a key'
echo 'ssh-ed25519 AAAA test' > $SYS/authorized_keys
dev status;  want 'available yes' 'a key in the image'; want 'running no' 'not running yet'
dev start;   ok '! alive' 'start does nothing while it is off'
dev on;      want 'the server runs$' 'on'; wantrc 0 'on'
ok 'alive' 'the server runs after on'
ok '[ -f $STATE/autotop/dev-access ] && [ -s $STATE/autotop/dev-hostkey ]' 'the choice and the key are kept'
ok '[ "$(cat $T/served-with)" = $STATE/autotop/dev-hostkey ]' 'served with the kept key'
first=$(cat $T/dropbear.pid); key=$(cksum < $STATE/autotop/dev-hostkey)
dev on;      ok '[ "$(cat $T/dropbear.pid)" = "$first" ]' 'on twice: still the one server'
dev status;  want 'on yes' 'status on'; want 'running yes' 'status running'
kill "$first"; rm -f $T/dropbear.pid		# a restart of the unit
dev start;   want 'the server runs$' 'start after a restart'; ok 'alive' 'runs again'
ok '[ "$(cksum < $STATE/autotop/dev-hostkey)" = "$key" ]' 'the same key after a restart'
dev off;     want 'off' 'off'; ok '! alive' 'the server ended'
ok '[ ! -f $STATE/autotop/dev-access ] && [ -s $STATE/autotop/dev-hostkey ]' 'off: the choice is gone, the key stays'
dev start;   ok '! alive' 'start after off does nothing'
touch $T/server-fails
dev on;      want 'the server did not start' 'a server that fails'; wantrc 1 'a server that fails'
rm -f $T/server-fails; dev off
export DEV_ACCESS_NO_STATE=1; rm -rf $STATE/autotop	# a unit whose partition is not there
dev on;      want 'no state partition: on for this session only' 'without the state partition'
want 'with a key for this session only' 'a session key'; ok 'alive' 'runs without the state partition'
ok '[ "$(cat $T/served-with)" = $T/dev-hostkey ] && [ ! -e $STATE/autotop ]' 'the session key is in RAM; nothing written to the partition'
dev off;     ok '! alive' 'off without the state partition'
unset DEV_ACCESS_NO_STATE
dev bogus;   wantrc 1 'an unknown word'

# ---- state-mount ----
C=$T/c; mkdir -p $C; export C
cat > $T/bb <<'STUB'
#!/bin/sh
# stand-in busybox: the real text tools; flash and mount commands counted, failing on demand
a=$1; shift
echo "$a $*" >> $C/log
count() { k=$(cat $C/$1.n 2> /dev/null || echo 0); echo $((k + 1)) > $C/$1.n; f=$(cat $C/$1.fails 2> /dev/null || echo 0); [ $k -ge $f ]; }
# Like the kernel: a partition is attached once, detached only when asked rightly (busybox 1.25:
# "ubidetach -d N CTRL_DEV"), and neither erased nor attached again while it is attached.
case $a in
sed | mkdir | cut) exec $a "$@" ;;
ubiattach) [ ! -f $C/attached ] && count attach && : > $C/attached ;;
ubidetach) [ "$*" = '-d 1 /dev/ubi_ctrl' ] && rm -f $C/attached ;;
mount) case "$*" in *remount*) exit 0 ;; esac; [ -f $C/attached ] && count mount ;;
flash_eraseall) [ ! -f $C/attached ] && count erase ;;
ubimkvol) [ -f $C/attached ] && count mkvol ;;
*) exit 0 ;;
esac
STUB
# sys/ubi-attach's stand-in: the attach that makes anew, with stock's reserve
mkdir -p $T/sm-sys
cat > $T/sm-sys/ubi-attach <<'STUB'
#!/bin/sh
echo "ubi-attach $*" >> $C/log
[ ! -f $C/attached ] && [ "$3" = 2 ] && [ ! -f $C/anew-fails ] && : > $C/attached
STUB
chmod 755 $T/bb $T/sm-sys/ubi-attach
printf 'dev:    size   erasesize  name\nmtd6: 06d00000 00020000 "rootfs"\nmtd7: 00600000 00020000 "userdata"\n' > $T/mtd
sm() {	# sm ATTACH_FAILS MOUNT_FAILS ERASE_FAILS
    rm -rf $C/* $T/st; echo $1 > $C/attach.fails; echo $2 > $C/mount.fails; echo $3 > $C/erase.fails; : > $T/window
    [ -n "${ANEW_FAILS:-}" ] && : > $C/anew-fails
    B=$T/bb SYS=$T/sm-sys STATE=$T/st MTD=$T/mtd STATE_RETRY_S=0 $SH $P/state-mount $T/window > $T/out 2>&1; rc=$?
}
erased() { grep -q '^flash_eraseall' $C/log; }

sm 0 0 0;  want 'userdata attached (ubi1), mounted read-only' 'a good partition'; wantrc 0 'a good partition'
ok '! erased' 'a good partition is not erased'; ok 'grep -q "^arm 7260000 600000$" $T/window' 'the window is armed'
ok 'grep -q "^ubiattach -m 7 -d 1" $C/log' 'the partition is found by its name'
sm 1 0 0;  want 'at the second attempt' 'one failed attach'; wantrc 0 'one failed attach'; ok '! erased' 'one failure erases nothing'
sm 0 1 0;  want 'at the second attempt' 'one failed mount'; ok '! erased' 'one failed mount erases nothing'
sm 2 0 0;  want 'could not be read, twice: making it anew' 'attach fails twice'; want 'userdata made anew, mounted read-only' 'made anew'
wantrc 0 'made anew'; ok 'erased && grep -q "^flash_eraseall /dev/mtd7" $C/log' 'the right partition is erased'
ok 'grep -q "^ubi-attach 7 1 2$" $C/log' "attached anew with stock's reserve for bad blocks (2 in 1,024), not the kernel's 20"
ok 'grep -q "^ubimkvol /dev/ubi1 -N userdata -m" $C/log && [ -s $T/st/autotop/made-anew ]' 'a volume, a filesystem and a note'
ok 'grep -q "^arm" $T/window' 'still armed after making anew'
sm 0 2 0;  want 'userdata made anew' 'mount fails twice'; wantrc 0 'mount fails twice'
ok 'erased && [ "$(grep -c "^ubidetach -d 1 /dev/ubi_ctrl" $C/log)" -ge 2 ]' 'an attached partition is detached before each new attempt and before the erase'
sm 2 0 1;  want 'NOT mounted and could not be made anew' 'the erase fails'; wantrc 1 'the erase fails'
ok 'grep -q "^disarm$" $T/window' 'disarmed when nothing could be done'
sm 9 0 0;  want 'userdata made anew' 'a partition of garbage: the ordinary attach never works, the one after the erase does'; wantrc 0 'garbage'
ANEW_FAILS=1 sm 2 0 0;  want 'could not be made anew' 'the attach after the erase fails too'; wantrc 1 'nothing works'
ok 'grep -q "^disarm$" $T/window' 'disarmed then'
printf 'mtd6: 06d00000 00020000 "rootfs"\n' > $T/mtd
sm 0 0 0;  want 'NOT mounted (no partition or no window)' 'no userdata partition'; wantrc 1 'no userdata partition'
ok '[ ! -s $T/window ] && ! grep -q ubiattach $C/log 2> /dev/null' 'nothing armed or attached without the partition'

kill $(cat $T/dropbear.pid 2> /dev/null) 2> /dev/null
rm -rf $T
[ $fails = 0 ] && echo "system scripts: $n checks passed" || echo "system scripts: $fails of $n FAILED"
[ $fails = 0 ]
