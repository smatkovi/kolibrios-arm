/* KolibriOS for ARM: the part of ktcc's <sys/ksys.h> (KolibriOS's C
 * library, programs/develop/ktcc/libc.obj/include/sys/ksys.h) that the
 * shell uses, its types and functions with svc #0x40 (lib/kos.h) instead
 * of x86's int 0x40 */
#ifndef _KSYS_H_
#define _KSYS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <kos.h>

#define KOSAPI static inline

typedef union {
    uint32_t val;
    struct {
        uint8_t sec;
        uint8_t min;
        uint8_t hour;
        uint8_t _zero;
    };
} ksys_time_t;

typedef union {
    uint32_t val;
    struct {
        uint8_t day;
        uint8_t month;
        uint16_t year;
    };
} ksys_date_t;

typedef union {
    uint32_t val;
    struct {
        int16_t y;
        int16_t x;
    };
} ksys_pos_t;

typedef struct {
    uint32_t attributes;
    uint32_t name_cp;
    ksys_time_t creation_time;
    ksys_date_t creation_date;
    ksys_time_t last_access_time;
    ksys_date_t last_access_date;
    ksys_time_t last_modification_time;
    ksys_date_t last_modification_date;
    uint64_t size;
    char name[0];
} ksys_bdfe_t;

/* Function 14: the screen's size */
KOSAPI ksys_pos_t _ksys_screen_size(void)
{
    ksys_pos_t size;
    size.val = kos_call(14, 0, 0, 0, 0, 0);
    return size;
}

/* Function 70.5: a file's or folder's information */
KOSAPI int _ksys_file_info(const char *name, ksys_bdfe_t *bdfe)
{
    struct kos_fileinfo fi = { 5, 0, 0, 0, (u32)bdfe, 0, name };
    return kos_call(70, (u32)&fi, 0, 0, 0, 0);
}
#define _ksys_file_get_info _ksys_file_info
#define _ksys_dir_info _ksys_file_info

/* Function 54: the clipboard (54.1's buffer to be freed with free()) */
KOSAPI int _ksys_clip_num(void) { return kos_call(54, 0, 0, 0, 0, 0); }
KOSAPI char *_ksys_clip_get(int n) { return (char *)kos_call(54, 1, n, 0, 0, 0); }
KOSAPI int _ksys_clip_set(int n, char *buffer) { return kos_call(54, 2, n, (u32)buffer, 0, 0); }
KOSAPI int _ksys_clip_pop(void) { return kos_call(54, 3, 0, 0, 0, 0); }
KOSAPI int _ksys_clip_unlock(void) { return kos_call(54, 4, 0, 0, 0, 0); }
KOSAPI void _ksys_free(void *p) { free(p); }
KOSAPI void _ksys_thread_yield(void) { kos_call(68, 1, 0, 0, 0, 0); }

#endif
