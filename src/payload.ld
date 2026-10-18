ENTRY(engine_entry)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); tls PT_TLS FLAGS(4); }
SECTIONS {
  . = 0x10000000;
  __ImageBase = .;
  .text : { *(.text .text.*) } :text
  .rodata : { *(.rodata .rodata.*) } :text
  . = ALIGN(4096);
  .data : { *(.data .data.*) *(.got .got.*) } :data
  .init_array : { __init_array_start = .; KEEP(*(.init_array .init_array.*)) __init_array_end = .; } :data
  .tdata : { *(.tdata .tdata.*) } :data :tls
  .tbss : { *(.tbss .tbss.*) } :data :tls
  .bss : { *(.bss .bss.*) *(COMMON) } :data
  /DISCARD/ : { *(.eh_frame*) *(.gcc_except_table*) *(.comment) *(.note*) }
}
