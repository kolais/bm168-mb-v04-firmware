#!/usr/bin/env python3
"""Reference model of the stock BM168 normal-partition NAND ECC.

Derived from saved raw/OOB captures (docs/nand-ecc-format.md):

* BCH over GF(2^14), primitive polynomial 0x4443, t = 7 (98 parity bits).
* Two 1,024-byte steps per 2,048-byte page; parity at OOB [3,16) and [16,29).
* Data bits enter the encoder least-significant bit first within each byte.
* The 98-bit remainder is packed LSB-first into 13 bytes; the final six bits
  are padding. The result is XORed with a constant chosen so that all-0xff
  data has all-0xff parity, making erased pages decode cleanly.
* Stock programmed pages keep OOB [0,3) and [29,64) at 0xff.

S-Loader uses a different 512-byte-step format and is not modelled.
This is a host verification tool; it never accesses a device.
"""
import argparse
import json
import sys
from pathlib import Path

M, T, POLY = 14, 7, 0x4443
PARITY_BITS = M * T
STEP, PAGE, OOB, PARITY_BYTES = 1024, 2048, 64, 13
PARITY_SLOTS = ((3, 16), (16, 29))
MASK = bytes.fromhex('cc8a7ae8f62c31529aa2d283ff')
REV8 = bytes(int(f'{b:08b}'[::-1], 2) for b in range(256))


def _generator():
    n = (1 << M) - 1
    exp, log, a = [0] * (2 * n), [0] * (n + 1), 1
    for i in range(n):
        exp[i] = exp[i + n] = a
        log[a] = i
        a <<= 1
        if a >> M & 1:
            a ^= POLY
    roots = set()
    for i in range(1, 2 * T + 1):
        j = i
        while True:
            roots.add(j)
            j = j * 2 % n
            if j == i:
                break
    g = [1]
    for r in roots:
        ng = [0] * (len(g) + 1)
        for k, c in enumerate(g):
            ng[k + 1] ^= c
            ng[k] ^= exp[log[c] + r] if c else 0
        g = ng
    if len(g) != PARITY_BITS + 1 or any(c not in (0, 1) for c in g):
        raise AssertionError('unexpected BCH generator')
    return sum(c << k for k, c in enumerate(g))


GENERATOR = _generator()


def parity(data):
    """Return the 13 stock parity bytes for one 1,024-byte step."""
    if len(data) != STEP:
        raise ValueError('ECC step must be 1,024 bytes')
    rem = int.from_bytes(bytes(data).translate(REV8), 'big') << PARITY_BITS
    while rem.bit_length() > PARITY_BITS:
        rem ^= GENERATOR << (rem.bit_length() - 1 - PARITY_BITS)
    packed = (rem << (PARITY_BYTES * 8 - PARITY_BITS)).to_bytes(PARITY_BYTES, 'big')
    return bytes(x ^ y for x, y in zip(packed.translate(REV8), MASK))


def page_oob(page):
    """Return the complete stock-format OOB for a 2,048-byte page."""
    if len(page) != PAGE:
        raise ValueError('page must be 2,048 bytes')
    return b'\xff' * 3 + parity(page[:STEP]) + parity(page[STEP:]) + b'\xff' * 35


def _single_bit_syndromes():
    base = parity(bytes(STEP))
    table = {}
    for k in range(STEP * 8):
        d = bytearray(STEP)
        d[k // 8] = 1 << (k % 8)
        table[bytes(x ^ y for x, y in zip(parity(bytes(d)), base))] = ('data', k)
    for k in range(PARITY_BYTES * 8):
        p = bytearray(PARITY_BYTES)
        p[k // 8] = 1 << (k % 8)
        table[bytes(p)] = ('parity', k)
    return table


def selftest():
    assert parity(b'\xff' * STEP) == b'\xff' * PARITY_BYTES
    assert parity(bytes(STEP)) == MASK
    assert page_oob(b'\xff' * PAGE) == b'\xff' * OOB


def verify(path, stride, mismatch_limit=20):
    """Compare model parity with an interleaved 2,048+64 raw capture."""
    data = Path(path).read_bytes()
    if len(data) % stride:
        raise ValueError(f'{path}: size is not a multiple of {stride}')
    result = {'file': str(path), 'pages': len(data) // stride, 'erased_pages': 0,
              'matching_steps': 0, 'single_bit_steps': 0, 'other_steps': 0,
              'nonstock_spare_pages': 0, 'examples': []}
    singles = None
    for n in range(result['pages']):
        pg = data[n * stride:(n + 1) * stride]
        main, spare = pg[:PAGE], pg[PAGE:PAGE + OOB]
        if pg == b'\xff' * stride:
            result['erased_pages'] += 1
            continue
        if spare[:3] != b'\xff' * 3 or spare[29:] != b'\xff' * 35:
            result['nonstock_spare_pages'] += 1
        for step, (lo, hi) in enumerate(PARITY_SLOTS):
            syndrome = bytes(x ^ y for x, y in
                             zip(parity(main[step * STEP:(step + 1) * STEP]), spare[lo:hi]))
            if not any(syndrome):
                result['matching_steps'] += 1
                continue
            singles = singles or _single_bit_syndromes()
            kind = singles.get(syndrome)
            result['single_bit_steps' if kind else 'other_steps'] += 1
            if len(result['examples']) < mismatch_limit:
                result['examples'].append({'page': n, 'step': step,
                                           'single_bit': list(kind) if kind else None})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('selftest', help='check fixed model properties')
    v = sub.add_parser('verify', help='check raw/OOB captures against the model')
    v.add_argument('captures', nargs='+', type=Path)
    v.add_argument('--stride', type=int, default=PAGE + OOB)
    e = sub.add_parser('encode', help='print stock OOB for each 2,048-byte page of a file')
    e.add_argument('image', type=Path)
    args = parser.parse_args()
    selftest()
    if args.command == 'selftest':
        print('nand_ecc selftest passed')
    elif args.command == 'verify':
        results = [verify(p, args.stride) for p in args.captures]
        json.dump(results, sys.stdout, indent=2)
        print()
        return 1 if any(r['other_steps'] for r in results) else 0
    else:
        image = args.image.read_bytes()
        if len(image) % PAGE:
            raise SystemExit('image is not page aligned')
        for n in range(len(image) // PAGE):
            print(f'{n * PAGE:08x} {page_oob(image[n * PAGE:(n + 1) * PAGE]).hex()}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
