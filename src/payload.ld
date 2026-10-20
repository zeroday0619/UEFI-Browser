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
  .tdata ALIGN(4096) : { __thread_template_start = .; *(.tdata .tdata.*) __thread_template_data_end = .; } :data :tls
  .tbss : { *(.tbss .tbss.*) . = ALIGN(4096); __thread_template_end = .; } :data :tls
  .bss : { *(.bss .bss.*) *(COMMON) } :data
  /DISCARD/ : { *(.eh_frame*) *(.gcc_except_table*) *(.comment) *(.note*) }
}
