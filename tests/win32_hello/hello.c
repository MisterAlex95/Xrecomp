/* A Win32 console hello world of our own (XBA-233): the first Windows PE the x86 pipeline recompiles.
 *
 * No C runtime: the entry point is ours (-nostartfiles -e _mainCRTStartup), so the import table is three kernel32
 * functions and the recompiled program needs no CRT startup HLE. hello_crt.c is the same program through mingw's
 * own CRT startup and msvcrt's printf. Built by tools/win32/build_hello.sh. */
#include <windows.h>

static const char message[] = "Hello, world! (a Win32 PE recompiled by XRecomp)\n";

void __cdecl mainCRTStartup(void)
{
    DWORD written = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    BOOL ok = WriteFile(out, message, sizeof message - 1, &written, NULL);
    ExitProcess(ok && written == sizeof message - 1 ? 0 : 3);
}
