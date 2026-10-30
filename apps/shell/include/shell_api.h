/* KolibriOS for ARM: ktcc's <shell_api.h> (programs started by the shell
 * that talk to it through named shared memory "PID-SHELL"): the
 * commands and the buffer's layout, for the shell's
 * modules/module_program_console.c (apps/threadtest's "shellapi" is
 * such a program) */
#ifndef _SHELL_API_H_
#define _SHELL_API_H_

#include <sys/ksys.h>

enum SHELL_API_COMMANDS {
    SHELL_OK = 0,
    SHELL_EXIT = 1,
    SHELL_PUTC = 2,
    SHELL_PUTS = 3,
    SHELL_GETC = 4,
    SHELL_GETS = 5,
    SHELL_CLS = 6,
    SHELL_PID = 7,
    SHELL_PING = 8
};

#define SHELL_SHM_MAX 1024 * 16

#pragma pack(push, 1)
struct return_value {
    size_t written_bytes;
    char *str;
};

struct shell_shm_buffer {
    uint8_t cmd;
    char data[SHELL_SHM_MAX - 1 - sizeof(struct return_value)];
    struct return_value return_value;
};
#pragma pack(pop)

#endif
