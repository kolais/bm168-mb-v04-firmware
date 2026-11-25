#!/bin/sh
# The developer shell for real, in the build container: the server built for
# the unit (ARM, under qemu-arm) on the loopback, started by dev-access's own
# command line, and Dropbear's client built for the container from the same
# source. A key that is in authorized_keys gets in; another does not; a
# command comes back with its output and its status; a file goes there and
# comes back whole; and no session is a login shell (a login shell would run
# /etc/profile, which on the unit is stock's start-up).
#
#   test-dev-login.sh UNIT_DROPBEARMULTI DROPBEAR_SOURCE WORK SCRIPTS_DIR
M=$1; SRC=$2; W=$3; P=$4
T=$(mktemp -d); PORT=2222; fails=0; n=0
ok() { n=$((n + 1)); if [ "$1" = 0 ]; then :; else fails=$((fails + 1)); echo "FAIL $2"; fi; }
host=$W/dropbear-host
if [ ! -x $host/dbclient ] || [ "$(cat $host/.from 2> /dev/null)" != "$(cat $SRC/.source-sha256)" ]; then
    rm -rf $host; cp -r $SRC $host
    (cd $host && ./configure --disable-zlib --disable-syslog --disable-lastlog --disable-utmp --disable-utmpx \
        --disable-wtmp --disable-wtmpx --disable-pam --enable-bundled-libtom > /dev/null 2>&1 &&
        make -j8 PROGRAMS='dbclient scp' > /dev/null 2>&1) || { echo 'FAIL the client did not build'; exit 1; }
    cat $SRC/.source-sha256 > $host/.from
fi
S=$T/sys; mkdir -p $S
cp $M $S/dropbearmulti
for name in dropbear dropbearkey scp; do ln -s dropbearmulti $S/$name; done
cp $P/dev-shell $S/dev-shell
qemu-arm $S/dropbearkey -t ed25519 -f $T/hostkey > /dev/null 2>&1
qemu-arm $S/dropbearkey -t ed25519 -f $T/mine > /dev/null 2>&1
qemu-arm $S/dropbearkey -t ed25519 -f $T/other > /dev/null 2>&1
# The key list where dev-access puts it on the unit: root's home (there, a home made in RAM).
mkdir -p /root/.ssh; chmod 700 /root /root/.ssh
qemu-arm $S/dropbearkey -y -f $T/mine | grep '^ssh-ed25519' > /root/.ssh/authorized_keys
chmod 600 /root/.ssh/authorized_keys
# A profile that would give a login shell away. dev-shell's own PATH names the unit's directories;
# the container's tools are in /usr/bin and /bin, which it has too.
echo 'echo LOGIN-SHELL-PROFILE-RAN' > /etc/profile
# The far end of scp is found by a session's PATH, in our own directory first (on the unit the
# server's own scp; here the container's build of it, since a session's programs run natively).
mkdir -p /opt/autotop/sys; cp $host/scp /opt/autotop/sys/scp
qemu-arm $S/dropbear -r $T/hostkey -c $S/dev-shell -p 127.0.0.1:$PORT -P $T/pid -F > $T/server.log 2>&1 &
sleep 2
C="$host/dbclient -y -y -p $PORT -i $T/mine root@127.0.0.1"
out=$($C 'echo one; echo two' 2> $T/err); ok "$([ "$out" = "one
two" ]; echo $?)" 'a command comes back with its output'
$C 'exit 7' > /dev/null 2>&1; ok "$([ $? = 7 ]; echo $?)" "a command's status comes back"
$C 'echo $PATH' 2> /dev/null | grep -q '^/opt/autotop/sys:/opt/autotop/bin:'; ok $? "our tools come first in a session's PATH"
out=$(echo 'echo typed-in; exit' | $C -T 2> /dev/null)	# -T: this test has no terminal to offer
echo "$out" | grep -q typed-in; ok $? 'a session without a command is a shell'
echo "$out" | grep -q LOGIN-SHELL-PROFILE-RAN; ok "$([ $? != 0 ]; echo $?)" 'that shell is not a login shell: /etc/profile did not run'
$C 'echo x' 2> /dev/null | grep -q LOGIN-SHELL-PROFILE-RAN; ok "$([ $? != 0 ]; echo $?)" 'nor is a command'
$host/dbclient -y -y -p $PORT -i $T/other root@127.0.0.1 'echo in' < /dev/null > $T/o 2>&1
ok "$([ $? != 0 ] && ! grep -q '^in$' $T/o; echo $?)" 'a key that is not in the list does not get in'
head -c 300000 /dev/urandom > $T/file
$host/scp -S $host/dbclient -P $PORT -i $T/mine -o StrictHostKeyChecking=no $T/file root@127.0.0.1:$T/there > $T/scp.log 2>&1
cmp -s $T/file $T/there; ok $? 'a file goes to the unit whole (scp)'
$host/scp -S $host/dbclient -P $PORT -i $T/mine -o StrictHostKeyChecking=no root@127.0.0.1:$T/there $T/back >> $T/scp.log 2>&1
cmp -s $T/file $T/back; ok $? 'and comes back whole'
grep -q 'Pubkey auth succeeded' $T/server.log; ok $? 'the server says it was the key'
grep -qi 'password' $T/server.log; ok "$([ $? != 0 ]; echo $?)" 'no password was ever asked or tried'
kill $(cat $T/pid 2> /dev/null) 2> /dev/null
[ $fails = 0 ] || { echo '--- server log'; tail -20 $T/server.log; echo '--- client'; cat $T/err $T/scp.log 2> /dev/null | tail -10; }
rm -rf $T
[ $fails = 0 ] && echo "developer shell: $n checks passed" || echo "developer shell: $fails of $n FAILED"
[ $fails = 0 ]
