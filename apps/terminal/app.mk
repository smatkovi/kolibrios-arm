# terminal: KolibriOS's terminal on a serial port, translated to C from
# programs/system/terminal/terminal.asm; box_lib's widgets for its
# settings, the console for the session
CAPPS += terminal
SRC_terminal := apps/terminal/terminal.c
LIBS_terminal := boxlib console
MEM_terminal := 0x200000
