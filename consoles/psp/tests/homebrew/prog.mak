# One program of the PSP corpus through PSPSDK's build.mak (XBA-224): `make -f prog.mak TARGET=<prog>` in a copy of
# its sources (the Makefile next to this file does it). -G0: no small-data section (PSPSDK's usual flag).
OBJS = $(patsubst %.c,%.o,$(wildcard *.c))
CFLAGS = -O2 -G0 -Wall -Wextra -Werror
BUILD_PRX = 1
EXTRA_TARGETS = EBOOT.PBP
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
