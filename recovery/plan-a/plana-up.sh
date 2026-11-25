#!/bin/sh
# Plan A projection session on the RAM spike (4.19): stock userspace in a
# chroot on the read-only stock squashfs, with the 4.19 drivers proven one by
# one in the spikes (docs/plan-a-*.md). Nothing touches NAND.
#
#   plana-up.sh base   mount the chroot with writable RAM copies of /data
#                      (from DATA.TAR) and /etc/goc, load the drivers, create
#                      the device nodes stock's libraries expect
#   plana-up.sh run    MCU proxy, then what stock's /etc/profile starts:
#                      D-Bus session bus, gocsdk, Launcher -qws
#
# Environment: SQF (default STOCK6.SQF) and DATA (default DATA.TAR) on
# /mnt/usb; MCU_ALLOW = hex prefixes of Launcher->MCU frames the proxy may
# forward (default: stock's two read requests and the backlight frame).
# LAUNCHER=autotop runs the new Launcher (launcherd) instead of stock's GUI.
# AP_SSID / AP_PSK replace stock's access point name and passphrase.
set -u
B=/bin/busybox
R=/mnt/stock
A=$R/opt/autotop
SQF=${SQF:-STOCK6.SQF}
DATA=${DATA:-DATA.TAR}
# Owner-approved frames (2026-11-18): status 01 and version 02 requests,
# backlight 61 xx, source switch 04 xx, status detect 0A xx, and the AV
# switch 4C xx that moves the screen from the car's input to the SoC on a
# cold boot (the MCU reports DE 00 01 until Launcher sends 4C 00 00,
# 4C 13 03). Still dropped: volume 07 xx and everything else.
# Narrowed 2026-11-19 after the GD32 disassembly: of the 4C family only
# 4C 00 xx (picture source: 00 ARM, 01 car, 02-04 camera inputs) and
# 4C 13 03 (version query to the APIX-board MCU) pass. 4C 10/11/12 start
# firmware updates of the TW8836 or the APIX-board MCU, and 4C 20/21 write
# the GD32's stored settings to its flash; Launcher has never sent them.
MCU_ALLOW=${MCU_ALLOW:-"aa55000301 aa55000302 aa55000361 aa55000304 aa5500040a aa5500044c00 aa5500044c1303"}

say() { echo "plana: $*"; }
die() { echo "plana: FAIL: $*"; exit 1; }
mounted() { $B grep -q " $1 " /proc/mounts; }

base() {
    mounted $R || $B mount -t squashfs -o loop,ro /mnt/usb/$SQF $R || die "mount $SQF"
    [ -d $A/modules ] || die "$SQF has no /opt/autotop/modules (wrong image?)"
    mounted $R/proc || $B mount -t proc proc $R/proc
    mounted $R/sys || $B mount -t sysfs sysfs $R/sys
    for d in tmp var media; do
        mounted $R/$d || $B mount -t tmpfs -o mode=1777 tmpfs $R/$d
    done
    $B mkdir -p $R/var/run $R/var/lib/lockdown $R/var/lib/misc $R/tmp/dev

    # Writable state in RAM: /data from the backup (settings, CarPlay
    # keychain), /etc/goc from the image (Bluetooth pairing list).
    if ! mounted $R/data; then
        $B mount -t tmpfs tmpfs $R/data
        [ -f /mnt/usb/$DATA ] && $B tar -x -f /mnt/usb/$DATA -C $R/data || say "no $DATA: empty /data"
    fi
    # /etc in RAM (2.8 MB): Launcher writes /etc/linkmode and gocsdk its
    # pairing list under /etc/goc; the squashfs is read-only. A fresh copy
    # of stock's pairing list (GOC, taken on stock just before the session,
    # see docs/plan-a-peripheral-spike.md) replaces the image's old one, so
    # the phone finds the keys it currently knows.
    if ! mounted $R/etc; then
        $B rm -rf /tmp/etc-seed
        $B cp -a $R/etc /tmp/etc-seed
        $B mount -t tmpfs tmpfs $R/etc
        $B cp -a /tmp/etc-seed/. $R/etc/
        $B rm -rf /tmp/etc-seed
        [ -f /mnt/usb/${GOC:-GOC.TAR} ] && $B tar -x -f /mnt/usb/${GOC:-GOC.TAR} -C $R/etc/goc \
            && say "pairing list from ${GOC:-GOC.TAR}"
        # Access point name and passphrase (PLANA.CFG AP_SSID, AP_PSK), in
        # the RAM copy of /etc only. carplay asks the running hostapd for
        # them (/var/run/hostapd/wlan0) and gives them to the phone over
        # iAP2 at every connection, so phones need no set-up.
        H=$R/etc/hostapd/hostapd.conf
        [ -n "${AP_SSID:-}" ] && $B sed -i "s/^ssid=.*/ssid=$AP_SSID/" $H && say "access point name set"
        [ -n "${AP_PSK:-}" ] && [ ${#AP_PSK} -ge 8 ] && $B sed -i "s/^wpa_passphrase=.*/wpa_passphrase=$AP_PSK/" $H \
            && say "access point passphrase set"
    fi

    # Saved state (state-save, on the userdata partition): /data and the
    # pairing list as they were when last saved replace the session's
    # copies above. An archive that does not read through is passed over,
    # and the one before it is tried.
    if [ ! -e /tmp/state-loaded ]; then
        : > /tmp/state-loaded
        for t in /mnt/state/autotop/state.tar /mnt/state/autotop/state.old; do
            [ -f $t ] && $B tar -t -f $t > /dev/null 2>&1 || continue
            $B rm -rf $R/data/* $R/data/.[!.]*
            $B tar -x -f $t -C $R && say "saved state loaded (${t##*/})" && break
            say "saved state ${t##*/} did not unpack"
        done
    fi

    # Drivers, in the order the spikes proved them.
    M=$A/modules
    ins() { $B grep -q "^$2 " /proc/modules || $B insmod $M/$1 ${3:-} || die "insmod $1"; }
    ins galcore.ko galcore "registerMemBase=0xe0f00000 irqLine=48 contiguousSize=0x800000 physSize=0x80000000 powerManagement=0"
    ins arkvdec.ko arkvdec
    ins memalloc_stock.ko memalloc_stock
    ins ark_display_compat.ko ark_display_compat screen_id=1
    for m in snd-soc-ark1668-i2s ark1668-sddac-codec ark1668-sdadc-codec snd-soc-ark-bd37033 \
             snd-soc-simple-card-utils snd-soc-simple-card; do
        ins $m.ko $(echo $m | $B sed "s/-/_/g")
    done
    ins 8821cs.ko 8821cs
    # Touch last, then wait for its input device: goodix finishes its set-up
    # asynchronously after insmod (firmware lookup, interrupt request on the
    # level-only GPIO controller). With the L2 cache on, that overlapping the
    # sound drivers' probe locked the system up on 6 of 7 boots (2026-11-19);
    # loaded last it came up cleanly on 4 of 4.
    ins goodix.ko goodix
    n=0
    while [ ! -e /dev/input/event0 ] && [ $n -lt 50 ]; do
        $B usleep 100000
        n=$((n + 1))
    done
    [ -e /dev/input/event0 ] || say "warning: no touch input device after 5 s"

    # /dev: a copy of the spike's nodes, made after the drivers above have
    # created theirs (touch, GPU, decoder, sound), so ttyS2 can be the MCU
    # proxy's pty.
    if ! mounted $R/dev; then
        $B mount -t tmpfs -o mode=755 tmpfs $R/dev
        $B cp -a /dev/. $R/dev/
        $B rm -f $R/dev/ttyS2
        mounted /dev/pts || { $B mkdir -p /dev/pts; $B mount -t devpts devpts /dev/pts; }
        $B mkdir -p $R/dev/pts $R/dev/shm
        $B mount --bind /dev/pts $R/dev/pts
    fi

    # Nodes stock's libmfc opens (its own load scripts stop at their 3.4
    # insmod): /tmp/dev/hx170 = the decoder misc device, /tmp/dev/memalloc =
    # the stock-layout pool.
    v=$($B cat /sys/class/misc/vdec/dev) || die "no vdec misc device"
    ma=$($B sed -n 's/^ *\([0-9]*\) memalloc_stock$/\1/p' /proc/devices)
    [ -n "$ma" ] || die "no memalloc_stock major"
    $B rm -f $R/tmp/dev/hx170 $R/tmp/dev/memalloc
    $B mknod -m 666 $R/tmp/dev/hx170 c ${v%:*} ${v#*:}
    $B mknod -m 666 $R/tmp/dev/memalloc c $ma 0
    for n in fb0 fb4 galcore vdec ark_display input/event0 snd/controlC0 snd/pcmC0D0p snd/pcmC0D1c ttyHS1; do
        [ -e $R/dev/$n ] || die "chroot /dev/$n missing"
    done
    [ -e /sys/class/net/wlan0 ] || say "warning: no wlan0"
    say "base ready"
}

run() {
    mounted $R/dev || die "run 'base' first"
    $B ps | $B grep -q "[m]cuproxy" || {
        # shellcheck disable=SC2086
        $A/bin/mcuproxy /tmp/mcu-pty /tmp/mcu.log $MCU_ALLOW &
        $B usleep 300000
        S=$($B readlink /tmp/mcu-pty) || die "mcuproxy did not create its pty"
        $B rm -f $R/dev/ttyS2
        : > $R/dev/ttyS2
        $B mount --bind $S $R/dev/ttyS2 || die "bind $S onto ttyS2"
    }
    if [ "${LAUNCHER:-stock}" = autotop ]; then
        # New Launcher (docs/launcher-design.md): the start-up jobs stock
        # Launcher's GUI did, without it: D-Bus, gocsdk, the access point and
        # DHCP (stock /etc/hostapd.sh), the stock audio service; then
        # launcherd outside the chroot. carplay starts on its first D-Bus call.
        $B chroot $R /bin/sh -c '
            grep "^export" /etc/profile > /tmp/profile-env
            . /tmp/profile-env
            eval `dbus-launch --auto-syntax`
            echo "$DBUS_SESSION_BUS_ADDRESS" > /tmp/dbus-address
            gocsdk > /tmp/gocsdk.log 2>&1 &
            sh /etc/hostapd.sh > /tmp/hostapd.log 2>&1
            Launcher -t -audio > /tmp/audio.log 2>&1 &
        ' > $R/tmp/plana-run.log 2>&1 < /dev/null
        $B usleep 2000000
        # The launcher is started again if it ends, and what it last said is
        # kept beside (it crashed once in the car, 2026-11-25, and left
        # CarPlay running with no way back to Home). Five ends within a
        # minute and it is left alone: something is wrong that a restart
        # does not mend.
        (
            n=0; t0=$($B cut -d. -f1 /proc/uptime)
            while :; do
                $A/bin/launcherd > /tmp/launcherd.log 2>&1 < /dev/null
                rc=$?
                $B cp /tmp/launcherd.log /tmp/launcherd-ended.log
                now=$($B cut -d. -f1 /proc/uptime)
                echo "launcherd ended with $rc at $now s" >> /tmp/launcherd-ends.log
                [ $((now - t0)) -gt 60 ] && { n=0; t0=$now; }
                n=$((n + 1))
                [ $n -ge 5 ] && { echo 'five ends within a minute: not started again' >> /tmp/launcherd-ends.log; break; }
                $B sleep 1
            done
        ) > /dev/null 2>&1 < /dev/null &
        say "run started (new Launcher): logs /tmp/launcherd.log, $R/tmp/gocsdk.log, /tmp/mcu.log"
        return
    fi
    # Stock /etc/profile: exports, D-Bus session bus, gocsdk, Launcher.
    $B chroot $R /bin/sh -c '
        grep "^export" /etc/profile > /tmp/profile-env
        . /tmp/profile-env
        eval `dbus-launch --auto-syntax`
        echo "$DBUS_SESSION_BUS_ADDRESS" > /tmp/dbus-address
        gocsdk > /tmp/gocsdk.log 2>&1 &
        Launcher -qws > /tmp/launcher.log 2>&1 &
    ' > $R/tmp/plana-run.log 2>&1 < /dev/null   # daemons must not hold the caller's output
    $B usleep 2000000
    say "run started: logs $R/tmp/launcher.log, $R/tmp/gocsdk.log, /tmp/mcu.log"
}

case "${1:-}" in
    base) base ;;
    run) run ;;
    *) echo "usage: plana-up.sh base|run"; exit 2 ;;
esac
