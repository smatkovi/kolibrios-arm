# threadtest: threads (51), IPC (60), named shared memory (68.22, 68.23)
# and libraries (68.19) checked from a program; its lines go to the debug
# board ("threadtest:"), tests/threads runs it on both kernels
CAPPS += threadtest
SRC_threadtest := apps/threadtest/threadtest.c
# libini: loaded by 68.19 through the import table (DLL=0: linked in)
LIBS_threadtest := libini
MEM_threadtest := 0x200000
CFLAGS_threadtest := -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
