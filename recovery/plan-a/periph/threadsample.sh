#!/bin/sh
# Poor man's profiler for the RAM spike (no perf): sample the current
# system call (/proc/TID/syscall: number and arguments, or "running") and
# the kernel wait point (/proc/TID/wchan) of threads whose name matches a
# pattern, about every 50 ms.
#   threadsample.sh SECS PATTERN... > out
# Output lines: "S <name> <syscall-nr|running> <wchan>"; analyse on the host.
B=/bin/busybox
secs=$1; shift
tids=
for t in /proc/[0-9]*/task/[0-9]*; do
    n=$(cat "$t/comm" 2>/dev/null) || continue
    for p in "$@"; do
        case "$n" in $p) tids="$tids ${t##*/}:$n" ;; esac
    done
done
echo "threads:$tids"
end=$(( $($B date +%s) + secs ))
while [ "$($B date +%s)" -lt "$end" ]; do
    for e in $tids; do
        id=${e%%:*}
        read -r sc rest < /proc/$id/syscall 2>/dev/null || continue
        echo "S ${e#*:} $sc $(cat /proc/$id/wchan 2>/dev/null)"
    done
    $B usleep 50000
done
echo done
