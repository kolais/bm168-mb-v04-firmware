/* RAM layout of the CAN recorder (docs/mcu-can-sniffer.md).
 * Stock firmware: data/bss below 0x20003400, stack top 0x20004a90.
 * 0x20005000  vector table copy (512 B, VTOR alignment), written by sniff.py
 * 0x20005200  code
 * 0x20005700  header
 * 0x20005800  table, 1024 x 24 B = 24 KiB, ends 0x2000b800 (inside 48 KiB) */
MEMORY
{
  CODE  (rx) : ORIGIN = 0x20005200, LENGTH = 0x500
  HDR   (rw) : ORIGIN = 0x20005700, LENGTH = 0x100
  TABLE (rw) : ORIGIN = 0x20005800, LENGTH = 0x6000
}
ENTRY(sniff_can0_rx0)
SECTIONS
{
  .text : { *(.text*) *(.rodata*) } > CODE
  .sniff_hdr (NOLOAD) : { *(.sniff_hdr) } > HDR
  .sniff_table (NOLOAD) : { *(.sniff_table) } > TABLE
  /DISCARD/ : { *(.data*) *(.bss*) *(.ARM.*) *(.comment) }
}
