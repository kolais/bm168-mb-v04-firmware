#!/bin/sh
# Wait until wlan0 receives more than 300 KB in one second, then profile.
#   wifiprof-wait.sh SECS OUT
B=/bin/busybox
rx() { $B sed -n "s/.*wlan0: *\([0-9]*\).*/\1/p" /proc/net/dev; }
a=$(rx)
while :; do
    $B sleep 1
    b=$(rx)
    [ $((b - a)) -gt 300000 ] && break
    a=$b
done
exec /tmp/wifiprof.sh "$1" "$2"
