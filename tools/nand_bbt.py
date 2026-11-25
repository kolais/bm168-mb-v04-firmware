#!/usr/bin/env python3
"""Decode stock BM168 flash bad-block tables from a raw main+OOB capture.

Observed stock placement (docs/nand-ecc-format.md#flash-bad-block-table):
pattern `Bbt0` (primary) or `1tbB` (mirror) at OOB byte 55, version at 59,
in the first page of one of the chip's last four eraseblocks. The table uses
Linux's 2-bit-per-block encoding, least-significant bit pair first, in the
page's main area: 0b11 good, 0b00 factory bad, 0b01 worn/marked bad and
0b10 reserved. This interpretation is checked against the capture; any
disagreement between the copies or with a marker map is reported, never
silently resolved. Host-only: it reads files and never accesses a device.
"""
import argparse
import json
import sys
from pathlib import Path

PAGE, OOB, ERASE, CHIP = 2048, 64, 131072, 134217728
BLOCKS = CHIP // ERASE
PATTERNS = {b'Bbt0': 'primary', b'1tbB': 'mirror'}
PATTERN_OFFSET, VERSION_OFFSET, TABLE_BLOCKS = 55, 59, 4
CODES = {3: 'good', 0: 'factory-bad', 1: 'worn', 2: 'reserved'}


def decode_table(main):
    states = []
    for block in range(BLOCKS):
        byte = main[block // 4]
        states.append(CODES[(byte >> (2 * (block % 4))) & 3])
    return states


def find_tables(capture, base):
    stride = PAGE + OOB
    if len(capture) % stride or base % ERASE:
        raise ValueError('capture must be whole pages starting at an eraseblock boundary')
    first_table_block = BLOCKS - TABLE_BLOCKS
    tables = []
    for n in range(len(capture) // stride):
        physical = base + n * PAGE
        if physical % ERASE:
            continue
        block = physical // ERASE
        page = capture[n * stride:(n + 1) * stride]
        oob = page[PAGE:]
        pattern = oob[PATTERN_OFFSET:PATTERN_OFFSET + 4]
        if pattern not in PATTERNS:
            continue
        if block < first_table_block:
            raise ValueError(f'table pattern outside the last {TABLE_BLOCKS} blocks at {physical:#x}')
        main = page[:PAGE]
        states = decode_table(main)
        tables.append({
            'copy': PATTERNS[pattern], 'block': block, 'physical_offset': physical,
            'version': oob[VERSION_OFFSET],
            'table_bytes_all_ff': main[:BLOCKS // 4] == b'\xff' * (BLOCKS // 4),
            'trailing_main_all_ff': main[BLOCKS // 4:] == b'\xff' * (PAGE - BLOCKS // 4),
            'oob_outside_pattern_all_ff': (oob[:PATTERN_OFFSET] + oob[VERSION_OFFSET + 1:]) ==
                                          b'\xff' * (OOB - (VERSION_OFFSET + 1 - PATTERN_OFFSET)),
            'non_good': {str(b): s for b, s in enumerate(states) if s != 'good'},
        })
    return tables


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('capture', type=Path, help='interleaved 2,048+64 raw capture')
    parser.add_argument('--base', type=lambda s: int(s, 0), required=True,
                        help='physical main-area offset of the first captured page')
    parser.add_argument('--marker-map', type=Path,
                        help='bad-block-map.json from a physical marker scan, to reconcile')
    args = parser.parse_args()
    tables = find_tables(args.capture.read_bytes(), args.base)
    report = {'capture': str(args.capture), 'base': args.base, 'tables': tables, 'problems': []}
    copies = {t['copy'] for t in tables}
    if copies != {'primary', 'mirror'} or len(tables) != 2:
        report['problems'].append(f'expected one primary and one mirror table, found {len(tables)}')
    if len({t['version'] for t in tables}) > 1:
        report['problems'].append('table versions differ')
    if len({json.dumps(t['non_good'], sort_keys=True) for t in tables}) > 1:
        report['problems'].append('table contents differ')
    for t in tables:
        if not t['trailing_main_all_ff'] or not t['oob_outside_pattern_all_ff']:
            report['problems'].append(f'unexpected non-ff bytes in {t["copy"]} table page')
    if args.marker_map and tables:
        markers = json.loads(args.marker_map.read_text())
        bad = {r['physical_offset'] // ERASE for r in markers['records']
               if r['page0_marker'] != 0xff or r['page1_marker'] != 0xff or r['memgetbadblock']}
        for t in tables:
            table_bad = {int(b) for b, s in t['non_good'].items() if s != 'reserved'}
            if bad != table_bad:
                report['problems'].append(f'marker map bad {sorted(bad)} differs from '
                                          f'{t["copy"]} table {sorted(table_bad)}')
        report['marker_map_bad_blocks'] = sorted(bad)
    json.dump(report, sys.stdout, indent=2)
    print()
    return 1 if report['problems'] else 0


if __name__ == '__main__':
    sys.exit(main())
