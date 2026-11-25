#!/usr/bin/env python3
"""BM168 bench console harness (UART through the Black Magic Probe).

Replaces the untracked per-run helpers used for R4-R7. It never issues NAND
erase/program, environment or vendor-update commands: U-Boot commands must
match an explicit allow-list, and shell commands are sent only as given by
the calling procedure. Every byte received is appended to an evidence log.

Port: --port, else $AUTOTOP_UART, else the Black Magic Probe's UART: the only
/dev/cu.usbmodem*3 port (its name changes on re-plugging; *1 is GDB). 115200 8N1.
Coordinate with the owner before running anything that touches the device.
"""
import argparse
import fcntl
import json
import os
import re
import select
import sys
import termios
import time
import zlib
from pathlib import Path

DEFAULT_PORT = '/dev/cu.usbmodem103'


def probe_uart():
    """The probe's UART port: its name changes on re-plugging, the suffix does not."""
    import glob
    ports = sorted(glob.glob('/dev/cu.usbmodem*3'))
    if os.path.exists(DEFAULT_PORT) or len(ports) != 1:
        return DEFAULT_PORT
    return ports[0]
UBOOT_PROMPT = re.compile(rb'(?:\r|\n)> $')
# While interrupting, U-Boot echoes the spaces we send, so the prompt is not last.
UBOOT_PROMPT_SEEN = re.compile(rb'(?:\r|\n)> ')
BOOT_BANNER = (b'ARK1680 SoC Booter', b'U-Boot 2012.10', b'Restarting system')
SHELL_LINE_LIMIT = 900  # stock and recovery shells truncate lines near 1 KiB
# Read-only inspection, USB loading and the two proven boot commands only.
UBOOT_ALLOWED = (
    re.compile(r'help( \w+)?'), re.compile(r'version'), re.compile(r'printenv( \w+)?'),
    re.compile(r'nand (info|bad)'), re.compile(r'nand read 0x[0-9a-f]+ 0x[0-9a-f]+ 0x[0-9a-f]+'),
    re.compile(r'usb (start|storage|info)'), re.compile(r'fat(ls|info) usb 0(:1)?( [\w/.]+)?'),
    re.compile(r'fatload usb 0:1 0x[0-9a-f]+ [A-Z0-9_]{1,8}(\.[A-Z0-9]{1,3})?( 0x[0-9a-f]+)?'),
    re.compile(r'crc32 0x[0-9a-f]+ 0x[0-9a-f]+'), re.compile(r'md(\.[bwl])? 0x[0-9a-f]+ 0x[0-9a-f]+'),
    re.compile(r'bootz 0x1000000'), re.compile(r'run nandboot'),
)
# Stock U-Boot global data, read-only: relocation address and RAM size words.
GD_ADDRESS, GD_RELOC, GD_RAM = 0x03ffff80, '03e1c000', '04000000'
LOAD_ADDRESS = 0x1000000


class ConsoleError(RuntimeError):
    pass


class Console:
    def __init__(self, log_path, port=None):
        self.port = port or os.environ.get('AUTOTOP_UART') or probe_uart()
        self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(self.fd)
            raise ConsoleError(f'{self.port} is in use by another harness') from None
        self.saved = termios.tcgetattr(self.fd)
        cfg = termios.tcgetattr(self.fd)
        cfg[0] = cfg[1] = cfg[3] = 0
        cfg[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        cfg[4] = cfg[5] = termios.B115200
        cfg[6][termios.VMIN] = cfg[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, cfg)
        # Discard stale input so a reopened port cannot replay old bytes.
        termios.tcflush(self.fd, termios.TCIFLUSH)
        log_path = Path(log_path)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log = log_path.open('ab', buffering=0)

    def close(self):
        termios.tcsetattr(self.fd, termios.TCSANOW, self.saved)
        os.close(self.fd)
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def send(self, text):
        data = text.encode() if isinstance(text, str) else text
        while data:
            if not select.select([], [self.fd], [], 2)[1]:
                raise ConsoleError('UART write timed out')
            data = data[os.write(self.fd, data):]

    def read(self, seconds, until=None):
        """Read for up to `seconds`; stop early when `until` (bytes or regex) matches."""
        end, data = time.monotonic() + seconds, b''
        while (left := end - time.monotonic()) > 0:
            if select.select([self.fd], [], [], min(0.2, left))[0]:
                chunk = os.read(self.fd, 65536)
                data += chunk
                self.log.write(chunk)
                if until is not None and (until.search(data) if hasattr(until, 'search') else until in data):
                    break
        return data

    def expect(self, seconds, until, what):
        data = self.read(seconds, until)
        if not (until.search(data) if hasattr(until, 'search') else until in data):
            raise ConsoleError(f'timed out waiting for {what}: {data[-600:]!r}')
        return data

    # Linux shells (stock, recovery)
    def shell(self, command, seconds=30):
        """Run one shell command; return (exit status, output). Never raises on status."""
        token = f'AUTOTOP_RC_{int(time.monotonic() * 1000) % 100000:05d}'
        line = f"{command}; printf '\\n{token}=%s\\n' \"$?\"\n"
        if len(line) > SHELL_LINE_LIMIT or '\n' in command:
            raise ConsoleError('shell command too long or multi-line for the target shell')
        self.send(line)
        marker = re.compile(rf'\r?\n{token}=(\d+)\r?\n'.encode())
        data = self.expect(seconds, marker, f'completion of {command[:40]!r}')
        return int(marker.search(data).group(1)), data

    def push(self, local, remote, chunk=700):
        """Copy a small local file to a RAM path on the target shell over UART, then verify SHA-256."""
        import base64, hashlib
        data = Path(local).read_bytes()
        if not re.fullmatch(r'/tmp/[A-Za-z0-9._-]+', remote):
            raise ConsoleError('push target must be a plain file under /tmp')
        encoded = base64.b64encode(data).decode()
        status, _ = self.shell(f': > {remote}.b64', 10)
        if status:
            raise ConsoleError('could not create transfer file')
        for i in range(0, len(encoded), chunk):
            status, _ = self.shell(f"printf '%s' '{encoded[i:i + chunk]}' >> {remote}.b64", 10)
            if status:
                raise ConsoleError(f'transfer chunk {i // chunk} failed')
        status, out = self.shell(f'OPENSSL_CONF=/dev/null openssl base64 -d -A -in {remote}.b64 -out {remote} && '
                                 f'OPENSSL_CONF=/dev/null openssl dgst -sha256 {remote}', 30)
        want = hashlib.sha256(data).hexdigest()
        if status or f'= {want}'.encode() not in out:
            raise ConsoleError(f'pushed file hash mismatch for {remote}')
        return want

    def push_segmented(self, local, remote, segment=65536, retries=3, chunk=700):
        """Like push(), for larger files: 64 KiB segments, each verified by
        SHA-256 on the target and resent on mismatch (single characters can be
        lost on the UART during long transfers)."""
        import base64, hashlib
        data = Path(local).read_bytes()
        if not re.fullmatch(r'/tmp/[A-Za-z0-9._-]+', remote):
            raise ConsoleError('push target must be a plain file under /tmp')
        status, _ = self.shell(f': > {remote}', 10)
        if status:
            raise ConsoleError('could not create target file')
        for n, off in enumerate(range(0, len(data), segment)):
            part = data[off:off + segment]
            enc = base64.b64encode(part).decode()
            want = hashlib.sha256(part).hexdigest()
            for attempt in range(retries):
                self.shell(f': > {remote}.seg.b64', 10)
                for i in range(0, len(enc), chunk):
                    self.shell(f"printf '%s' '{enc[i:i + chunk]}' >> {remote}.seg.b64", 10)
                status, out = self.shell(f'OPENSSL_CONF=/dev/null openssl base64 -d -A -in {remote}.seg.b64 -out {remote}.seg && '
                                         f'OPENSSL_CONF=/dev/null openssl dgst -sha256 {remote}.seg', 30)
                if not status and f'= {want}'.encode() in out:
                    break
            else:
                raise ConsoleError(f'segment {n} of {remote} failed {retries} times')
            status, _ = self.shell(f'/bin/busybox cat {remote}.seg >> {remote} && /bin/busybox rm -f {remote}.seg {remote}.seg.b64', 30)
            if status:
                raise ConsoleError(f'could not append segment {n}')
        want = hashlib.sha256(data).hexdigest()
        status, out = self.shell(f'OPENSSL_CONF=/dev/null openssl dgst -sha256 {remote}', 30)
        if status or f'= {want}'.encode() not in out:
            raise ConsoleError(f'pushed file hash mismatch for {remote}')
        return want

    # U-Boot
    def interrupt_uboot(self, seconds=40):
        """Wait for a boot banner, then send single spaces until the U-Boot prompt."""
        data, booting, end = b'', False, time.monotonic() + seconds
        while time.monotonic() < end:
            data += self.read(0.12)
            booting = booting or any(b in data for b in BOOT_BANNER)
            if booting and UBOOT_PROMPT_SEEN.search(data):
                break
            if booting:
                self.send(b' ')
        else:
            raise ConsoleError(f'no U-Boot prompt: {data[-600:]!r}')
        self.read(0.5)
        self.clear_uboot_line()
        return data

    def clear_uboot_line(self):
        """Discard echoed spaces or stray bytes, then require a clean prompt."""
        # Never send an empty line: U-Boot repeats the previous command on one.
        # Ctrl-U erases the line; a harmless 'version' then proves a clean prompt.
        # A stray byte after reset can still prefix the first command; that is
        # rejected as unknown and runs nothing, so retry once.
        for _ in range(2):
            self.send(b'\x15')
            self.read(0.5)
            self.send(b'version\n')
            data = self.expect(5, UBOOT_PROMPT, 'clean U-Boot prompt')
            if b'U-Boot 2012.10' in data and b'Unknown command' not in data:
                return data
        raise ConsoleError(f'U-Boot command line is not clean: {data[-300:]!r}')

    def uboot(self, command, seconds=10):
        if not any(p.fullmatch(command) for p in UBOOT_ALLOWED):
            raise ConsoleError(f'U-Boot command not on the read-only allow-list: {command!r}')
        self.send(command + '\n')
        data = self.expect(seconds, UBOOT_PROMPT, f'U-Boot {command!r}')
        if b'Unknown command' in data:
            raise ConsoleError(f'U-Boot rejected {command!r}')
        return data

    def verify_uboot_layout(self):
        data = self.uboot(f'md.l {GD_ADDRESS:#x} 0x20').decode(errors='replace').lower()
        row = re.search(r'03ffffb0: (\w{8}) (\w{8}) (\w{8})', data)
        if not row or row.group(2) != GD_RELOC or row.group(3) != GD_RAM:
            raise ConsoleError('U-Boot relocation/RAM layout differs from the recorded baseline')

    def uboot_load(self, name, size, crc32):
        """Load a short-name file from CARPLAY and verify byte count and RAM CRC."""
        self.verify_uboot_layout()
        if b'1 Storage Device(s) found' not in self.uboot('usb start', 20):
            raise ConsoleError('USB storage not found on CARPLAY')
        self.uboot('usb storage')
        load = self.uboot(f'fatload usb 0:1 {LOAD_ADDRESS:#x} {name}', 40)
        if f'{size} bytes read'.encode() not in load:
            raise ConsoleError(f'{name}: expected exactly {size} bytes')
        crc = self.uboot(f'crc32 {LOAD_ADDRESS:#x} {size:#x}')
        if f'==> {crc32}'.encode() not in crc.lower():
            raise ConsoleError(f'{name}: RAM CRC32 mismatch')
        return {'image': name, 'bytes': size, 'ram_crc32': crc32,
                'load_range': [f'{LOAD_ADDRESS:#010x}', f'{LOAD_ADDRESS + size:#010x}']}

    def boot_loaded_image(self):
        """Start the verified RAM image; output then belongs to the new kernel."""
        command = f'bootz {LOAD_ADDRESS:#x}'
        if not any(p.fullmatch(command) for p in UBOOT_ALLOWED):
            raise ConsoleError('bootz address not allowed')
        self.send(command + '\n')


def artifact_identity(path):
    data = Path(path).read_bytes()
    return len(data), f'{zlib.crc32(data):08x}'


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port')
    parser.add_argument('--evidence', type=Path, required=True, help='evidence directory under backups/')
    sub = parser.add_subparsers(dest='command', required=True)
    load = sub.add_parser('load', help='stock reboot, interrupt U-Boot, load and verify an image; stops before bootz')
    load.add_argument('--image', type=Path, required=True, help='local copy of the short-name file on the stick')
    load.add_argument('--from-uboot', action='store_true', help='already at the U-Boot prompt')
    boot = sub.add_parser('boot', help='bootz the verified image and wait for a ready marker')
    boot.add_argument('--ready', default='AUTOTOP_READONLY_RECOVERY_READY')
    run = sub.add_parser('shell', help='run one command on the current shell and report its status')
    run.add_argument('cmd')
    run.add_argument('--timeout', type=float, default=30)
    args = parser.parse_args()
    if not args.evidence.resolve().is_relative_to(Path('backups').resolve()):
        parser.error('evidence must be stored under the ignored backups/ directory')
    stamp = time.strftime('%Y%m%dT%H%M%S')
    with Console(args.evidence / f'{stamp}-{args.command}.log', args.port) as con:
        if args.command == 'load':
            size, crc = artifact_identity(args.image)
            if not args.from_uboot:
                status, _ = con.shell('sync', 20)
                if status:
                    raise ConsoleError('sync failed')
                con.send('reboot\n')
                con.interrupt_uboot()
            else:
                con.clear_uboot_line()
            record = con.uboot_load(args.image.name, size, crc)
            record['awaiting_drive_move_to_udisk'] = True
            (args.evidence / 'ram-load.json').write_text(json.dumps(record, indent=2) + '\n')
            print(json.dumps(record, indent=2))
            print('Verified in RAM; ask the owner to move the drive to U DISK, then run "boot".')
        elif args.command == 'boot':
            con.boot_loaded_image()
            con.expect(90, args.ready.encode(), args.ready)
            print(f'{args.ready} received')
        else:
            status, data = con.shell(args.cmd, args.timeout)
            sys.stdout.write(data.decode(errors='replace'))
            return status
    return 0


if __name__ == '__main__':
    sys.exit(main())
