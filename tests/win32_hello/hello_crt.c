/* The hello world through mingw's own CRT startup (crt2.o) and msvcrt (XBA-233): what a real console program
 * imports before main runs. Built by tools/win32/build_hello.sh. */
#include <stdio.h>

int main(int argc, char **argv)
{
    printf("Hello, world! (argc=%d, through the CRT)\n", argc);
    return argv[0] ? 0 : 4;
}
