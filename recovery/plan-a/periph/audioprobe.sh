#!/bin/sh
# Read-only audio register snapshot for stock 3.4 and the RAM spike.
#   audioprobe.sh TAG [CARD]  prints sysregs, both I2S blocks, the DMA channels,
#                        audio interrupts and ALSA stream state, each line
#                        prefixed with TAG. Never reads an I2S data register
#                        (0x80 pops the FIFO). Needs devmem (stock or busybox).
tag=$1
card=${2:-0}
B=; [ -x /bin/busybox ] && B=/bin/busybox   # the spike shell has few applets
dm() { $B devmem "$1"; }
range() { # base first last step
    o=$2
    while [ $((o)) -le $(($3)) ]; do
        a=$(printf 0x%08x $(($1 + o)))
        echo "$tag $a $(dm $a)"
        o=$((o + $4))
    done
}
range 0xe4900000 0x000 0x1fc 4     # system registers: clocks, NCO, pads
range 0xe4000000 0x00 0x1c 4       # I2S0 + SDDAC (playback)
range 0xe8200000 0x00 0x1c 4       # I2S1 + SARADC (capture)
ch=0
while [ $ch -lt 8 ]; do            # DMA channels: SAR DAR LLP CTL CFG
    b=$((0xe0000000 + ch * 0x58))
    for o in 0x00 0x08 0x10 0x18 0x1c 0x40 0x44; do
        a=$(printf 0x%08x $((b + o)))
        echo "$tag dma$ch $a $(dm $a)"
    done
    ch=$((ch + 1))
done
for o in 0x398 0x3a0; do
    a=$(printf 0x%08x $((0xe0000000 + o)))
    echo "$tag dmactl $a $(dm $a)"
done
$B grep -i -E "dma|i2s|audio|sddac" /proc/interrupts | $B sed "s/^/$tag irq /"
for s in /proc/asound/card$card/pcm*/sub0; do
    for f in hw_params status; do
        $B sed "s|^|$tag ${s#/proc/asound/card$card/} $f |" "$s/$f" 2>/dev/null
    done
done
echo "$tag done"
