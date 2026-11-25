#!/bin/sh
# Wi-Fi CPU profile for the RAM spike (no perf or profiling in the kernel).
#   wifiprof.sh SECS OUT
# Writes OUT.before/OUT.after (CPU, interrupts, wlan0 counters, per-thread
# times of the driver threads) and OUT.stacks: kernel stacks of the busy
# threads sampled about 5 times a second. Run it while traffic flows.
B=/bin/busybox
secs=$1
out=$2

pids() {
    for d in /proc/[0-9]*; do
        read -r c < "$d/comm" 2>/dev/null || continue
        case "$c" in
            RTW*|ksdioirqd*|ksoftirqd*|kworker*|wifiload|irq/*) echo "${d#/proc/}:$c" ;;
        esac
    done
}

snap() {
    $B head -1 /proc/stat
    $B grep -E "dw-mci|timer" /proc/interrupts
    $B grep wlan0 /proc/net/dev
    for e in $(pids); do
        p=${e%%:*}
        set -- $(cat /proc/$p/stat 2>/dev/null)
        echo "thread ${e#*:} pid=$p utime=${14} stime=${15}"
    done
}

snap > "$out.before"
: > "$out.stacks"
end=$(( $($B date +%s) + secs ))
while [ "$($B date +%s)" -lt "$end" ]; do
    for e in $(pids); do
        p=${e%%:*}
        read -r st < /proc/$p/stat 2>/dev/null || continue
        case "$st" in
            *") R "*|*") D "*)
                echo "== ${e#*:}" >> "$out.stacks"
                $B head -6 /proc/$p/stack >> "$out.stacks" 2>/dev/null ;;
        esac
    done
    $B usleep 200000
done
snap > "$out.after"
echo "wifiprof done"
