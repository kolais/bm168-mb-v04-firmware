#!/usr/bin/env python3
"""Bench power supply (OWON ODP3033) over USB: read state, cycle CH1.

Deliberately narrow: it never sets voltage, current, limits or protection,
and it switches only CH1 (the head unit). Other channels keep their state.

  psu.py status             identity, measured V/I of all channels, output states
  psu.py cycle [SECONDS]    CH1 off, wait (default 5 s), CH1 on, then status
  psu.py off | on           CH1 output off or on only, then status
  psu.py local              return the front panel (leave remote mode)

The ODP3033 enumerates as USBTMC (class 0xfe/3) and accepts USBTMC
DEV_DEP_MSG_OUT packets, but answers with plain text without the USBTMC
header. SCPI other than IEEE 488.2 works only in remote mode
(SYSTem:REMote; front panel locked except KeyLock, long-press it to leave).
Per-channel output: CHANnel:OUTPut:ALL <ch1>,<ch2>,<ch3> (ODP programming
manual), so CH2/CH3 are written back with their current state.

Needs Homebrew libusb and pyusb in the project venv:
  .venv/bin/python tools/bench/psu.py status
"""
import argparse
import os
import sys
import time

import usb.backend.libusb1
import usb.core
import usb.util

VID, PID = 0x5345, 0x1235      # OWON ODP3033
LIBUSB = '/opt/homebrew/opt/libusb/lib/libusb-1.0.dylib'


class Odp:
    def __init__(self):
        backend = usb.backend.libusb1.get_backend(
            find_library=lambda _: LIBUSB if os.path.exists(LIBUSB) else None)
        self.dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend)
        if self.dev is None:
            sys.exit('OWON ODP3033 not found on USB')
        self.dev.set_configuration()
        intf = self.dev.get_active_configuration()[(0, 0)]

        def bulk(direction):
            return usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(
                e.bEndpointAddress) == direction and usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK)
        self.ep_out, self.ep_in = bulk(usb.util.ENDPOINT_OUT), bulk(usb.util.ENDPOINT_IN)
        self.tag = 1
        self._drain()
        self.write('SYST:REM')
        time.sleep(0.3)

    def _hdr(self, msg_id, size, attr):
        t = self.tag
        self.tag = self.tag % 255 + 1
        return bytes([msg_id, t, (~t) & 0xff, 0]) + size.to_bytes(4, 'little') + bytes([attr, 0, 0, 0])

    def _drain(self):
        for _ in range(4):
            try:
                self.ep_in.read(512, timeout=200)
            except usb.core.USBError:
                return

    def write(self, cmd):
        data = (cmd + '\n').encode()
        pkt = self._hdr(1, len(data), 1) + data
        self.ep_out.write(pkt + b'\0' * (-len(pkt) % 4), timeout=2000)

    def query(self, cmd):
        self.write(cmd)
        time.sleep(0.2)
        self.ep_out.write(self._hdr(2, 256, 0), timeout=2000)
        return bytes(self.ep_in.read(512, timeout=3000)).decode(errors='replace').strip()

    def outputs(self):
        states = [s.strip() for s in self.query('CHAN:OUTP:ALL?').split(',')]
        if len(states) != 3 or any(s not in ('0', '1') for s in states):
            sys.exit(f'unexpected output state reply: {states!r}')
        return states


def status(p):
    print('IDN       ', p.query('*IDN?'))
    print('outputs   ', p.query('CHAN:OUTP:ALL?'), '(CH1, CH2, CH3)')
    print('measured V', p.query('MEAS:VOLT:ALL?'))
    print('measured A', p.query('MEAS:CURR:ALL?'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('action', choices=('status', 'cycle', 'off', 'on', 'local'))
    ap.add_argument('seconds', nargs='?', type=float, default=5.0)
    a = ap.parse_args()
    p = Odp()
    if a.action == 'status':
        status(p)
    elif a.action == 'local':
        p.write('SYST:LOC')
        print('front panel returned to local mode')
    elif a.action in ('off', 'on'):
        _, ch2, ch3 = p.outputs()
        p.write(f'CHAN:OUTP:ALL {1 if a.action == "on" else 0},{ch2},{ch3}')
        time.sleep(1.0)
        print(f'CH1 {a.action}:', p.query('CHAN:OUTP:ALL?'))
        status(p)
    else:
        _, ch2, ch3 = p.outputs()
        p.write(f'CHAN:OUTP:ALL 0,{ch2},{ch3}')
        time.sleep(0.5)
        print('CH1 off:', p.query('CHAN:OUTP:ALL?'), f'- waiting {a.seconds:g} s')
        time.sleep(a.seconds)
        p.write(f'CHAN:OUTP:ALL 1,{ch2},{ch3}')
        time.sleep(1.0)
        print('CH1 on: ', p.query('CHAN:OUTP:ALL?'))
        status(p)


if __name__ == '__main__':
    main()
