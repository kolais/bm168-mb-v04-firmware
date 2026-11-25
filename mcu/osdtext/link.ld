/* The page writer in free RAM: the firmware's own use ends below 0x20003400
 * (stack top 0x20004a90); the profile hook's state is at 0x2000f000.
 * 0x2000e000 code, 0x2000e400 one page of data, stack below 0x2000eff0. */
MEMORY { RAM (rwx) : ORIGIN = 0x2000e000, LENGTH = 0x400 }
ENTRY(entry)
SECTIONS
{
  .text : { KEEP(*(.text.entry)) KEEP(*(.text.park)) *(.text*) *(.rodata*) } > RAM
  /DISCARD/ : { *(.data*) *(.bss*) *(.ARM.*) *(.comment) }
}
