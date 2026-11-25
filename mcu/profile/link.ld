/* Patch code in erased flash page 53 (0x0801a800-0x0801afff). The stock
 * application ends at 0x0801a3ff; the firmware's own settings pages are
 * 0x0801f000 and 0x0801f800. No data or bss: state lives at a fixed SRAM
 * address (profile.c). */
MEMORY { FLASH (rx) : ORIGIN = 0x0801a800, LENGTH = 0x800 }
ENTRY(profile_hook)
SECTIONS
{
  .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) } > FLASH
  /DISCARD/ : { *(.data*) *(.bss*) *(.ARM.*) *(.comment) }
}
