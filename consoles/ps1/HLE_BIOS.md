# PS1 HLE BIOS (P-5 HLE, XBA-163)

The PS1 core runs a program without any BIOS image: the kernel and the A / B / C function tables are written in C
(`runtime/ps1_hle.c`, design notes in `runtime/ps1_hle.h`). OpenBIOS LLE (`tools/consoles/build_ps1_bios.sh`) stays
the other path, and the reference: under both, our homebrew gives the same result block, checksums included.

## Choosing it

| Where | LLE (OpenBIOS image) | HLE |
|---|---|---|
| `ps1_create(bios, size, ...)` | a 512 KB image | `bios = NULL` |
| `ps1_run` | `--bios PATH`, `$RECOMP_PS1_BIOS`, else `build/ps1-bios/openbios.bin` when it exists | `--bios hle`; the default when no image was built |
| `ps1_play`, the app (`ps1_runtime`) | `--bios` / `RECOMP_PS1_BIOS` / the APK asset `ps1-bios/openbios.bin` / `build/ps1-bios/openbios.bin` | `hle` in any of those; the default when none of them exists |

`ps1_bios_hle(m)` tells which one runs; the runtime logs `BIOS HLE (no image found)` or `BIOS HLE (asked for)`.

## How it works

- **Boot.** Kernel RAM (0x0000..0xFFFF) is filled the way the retail BIOS and OpenBIOS fill it, then the PS-X EXE
  starts at once (SR = 0x40000000, interrupts off, as OpenBIOS hands over). With a disc and no EXE, `SYSTEM.CNF` is
  read from ISO 9660 (`BOOT`, its argument at 0x180, `TCB`, `EVENT`, `STACK`), else `PSX.EXE`. No shell, no logo.
- **Calls.** 0xA0 / 0xB0 / 0xC0 hold a dispatch trap: the entry the RAM table (A0 at 0x200, C0 at 0x674, B0 at 0x874)
  holds for `t1` runs. Each entry points to a 4-byte stub with a trap word (opcode 3Fh, reserved on the R3000A), which
  the interpreter hands to the C function when it runs from kernel RAM in HLE mode; the function returns to `ra`. A
  program that patches an entry (ps1-tests hook A(40h)), calls an entry it read (`GetB0Table`) or moves one gets the
  hardware behaviour. Lifted code (title packs) reaches the BIOS the same way: kernel RAM is never lifted, so its
  jumps to 0xA0 / 0xB0 / 0xC0 land in the interpreter, and `ps1_hle_rc_*` check packs in lockstep under HLE.
- **Exceptions.** The vector at 0x80 is `lui k0 / addiu k0 / jr k0` (the halfword at 0x86 is 0x275A, the retail
  value games check). The handler is MIPS in kernel RAM, as in OpenBIOS: a trap saves the registers into the current
  TCB (`[[0x108]]`, registers at +8, EPC +0x88, HI / LO, SR +0x94, CAUSE +0x98; an interrupt on a GTE command resumes
  after it), the four SysEnqIntRP priority chains are walked natively (verifier, then handler), then a trap longjmps
  to the exception jmp_buf: the default one returns from the exception, B(19h) sets the program's own (libpsn00b and
  Psy-Q's interrupt dispatchers). Built-in chain entries, as OpenBIOS enqueues them: the syscall handler (priority 0:
  Enter / ExitCriticalSection, ChangeThreadSubFunction; another exception calls A(40h) through the RAM table, which
  halts unless the program hooked it), the root counter handlers (priority 1: RCnt events `F200000x`, VBlank / timer
  IRQs acknowledged until ChangeClearRCnt(n, 0)), the pad / card VBlank handler (priority 2, StartPad / StartCard;
  ChangeClearPad) and the IRQ event verifier (priority 3: `F00000xx` events, SetIrqAutoAck).
- **Events, threads.** EvCBs (0x1C bytes) and TCBs (0xC0 bytes) live in kernel RAM behind the table of tables at
  0x100 (16 events and 4 threads by default; SYSTEM.CNF or SetConf change that). Event callbacks run as guest code
  from C (the interpreter runs them to their return, interrupts held back as in the handler). WaitEvent on an enabled
  event re-runs its stub until an interrupt delivers it.
- **Devices.** `cdrom:` reads ISO 9660 straight from the disc image (paths, `cd`, `firstfile` patterns); `bu00:` /
  `bu10:` work on the memory card images in the card's own file system (directory frames with their checksums,
  block chains, `format`, the 128-byte frame rule; FASYNC reads / writes and the low-level sector calls complete at
  once and deliver the SwCARD / HwCARD events). Neither goes through the CD-ROM controller or SIO0, so the games that
  drive those chips themselves (libpsn00b's CD and pad code, Psy-Q's libcd) are unaffected. Pads read by the BIOS
  (InitPad / StartPad, the outdated B(15h) / B(16h)) come from the pad state each VBlank.
- **Not implemented.** Any other call returns 0 and is reported once on stderr with its number:
  `ps1: HLE BIOS: unimplemented call B(1Ah) from 8001234C` (`ps1_hle_unimplemented_count`). `$PS1_HLE_TRACE=1`
  prints every call. Missing on purpose for now: the asynchronous CD calls (A(78h)..A(81h), A(90h)..A(95h)), the
  device driver internals (A(5Bh)..A(6Fh), C(0Eh)..C(11h), C(14h)..C(1Ah)), the kanji font ROM (B(51h) returns -1:
  there is no ROM), `atof` / `strtod`, the SIO1 TTY.

## Implemented calls

Each one has a check in `tests/test_hle.c` (ctest `ps1_hle`), which fails when an implemented call has none.

| Group | Calls | Checked in |
|---|---|---|
| Files | A(00h)..A(05h) / B(32h)..B(37h) open, lseek, read, write, close, ioctl; A(07h) / B(39h) isatty; A(08h) / B(3Ah) getc; A(09h) / B(3Bh) putc; B(40h) cd; B(41h) format; B(42h) firstfile; B(43h) nextfile; B(44h) rename; B(45h) erase; B(46h) undelete; B(54h) _get_errno; B(55h) _get_error | `test_cards`, `test_disc`, `test_tty` |
| Console | A(3Bh) / B(3Ch) getchar; A(3Ch) / B(3Dh) putchar; A(3Dh) / B(3Eh) gets; A(3Eh) puts; B(3Fh) puts (no newline); A(3Fh) printf (`%d %i %u %x %X %o %p %b %c %s %%`, flags `- 0 + space #`, width, precision, `*`) | `test_tty` |
| Strings, ctype | A(0Ah) todigit; A(0Ch) strtoul; A(0Dh) strtol; A(0Eh) abs; A(0Fh) labs; A(10h) atoi; A(11h) atol; A(12h) atob; A(15h)..A(24h) strcat, strncat, strcmp, strncmp, strcpy, strncpy, strlen, index, rindex, strchr, strrchr, strpbrk, strspn, strcspn, strtok, strstr; A(25h) toupper; A(26h) tolower | `test_strings` |
| Memory | A(27h) bcopy; A(28h) bzero; A(29h) bcmp; A(2Ah) memcpy; A(2Bh) memset; A(2Ch) memmove; A(2Dh) memcmp; A(2Eh) memchr; A(2Fh) rand; A(30h) srand; A(31h) qsort; A(35h) lsearch; A(36h) bsearch (guest comparison functions) | `test_memory`, `test_sort_search` |
| Heaps | A(33h) malloc; A(34h) free; A(37h) calloc; A(38h) realloc; A(39h) InitHeap; B(00h) alloc_kernel_memory; B(01h) free_kernel_memory; C(08h) SysInitMemory | `test_memory` |
| Control flow | A(13h) setjmp; A(14h) longjmp; A(06h) / A(3Ah) / B(38h) exit (halts); A(40h) SystemErrorUnresolvedException; C(0Bh) SystemError | `test_layout`, `test_exit_halts`, `test_syscalls_exceptions` |
| Programs | A(41h) LoadTest; A(42h) Load; A(43h) Exec (BSS cleared, stack from the header, returns 1 when the program returns); A(51h) LoadExec | `test_disc` |
| CD | A(54h) / A(71h) _96_init; A(56h) / A(72h) _96_remove; A(A4h) CdGetLbn; A(A5h) CdReadSector; A(A6h) CdGetStatus | `test_disc`, `test_layout` |
| GPU | A(46h) GPU_dw; A(47h) gpu_send_dma; A(48h) SendGP1Command; A(49h) GPU_cw; A(4Ah) GPU_cwp; A(4Bh) send_gpu_linked_list; A(4Ch) gpu_abort_dma; A(4Dh) GetGPUStatus; A(4Eh) gpu_sync | `test_layout` |
| Events | B(07h) DeliverEvent; B(08h) OpenEvent; B(09h) CloseEvent; B(0Ah) WaitEvent; B(0Bh) TestEvent; B(0Ch) EnableEvent; B(0Dh) DisableEvent; B(20h) UnDeliverEvent; C(04h) get_free_EvCB_slot | `test_events`, `test_irq_chain` |
| Threads | B(0Eh) OpenTh; B(0Fh) CloseTh; B(10h) ChangeTh; C(05h) get_free_TCB_slot | `test_threads` |
| Exceptions, IRQs | B(17h) ReturnFromException; B(18h) SetDefaultExitFromException; B(19h) SetCustomExitFromException; C(00h) EnqueueTimerAndVblankIrqs; C(01h) EnqueueSyscallHandler; C(02h) SysEnqIntRP; C(03h) SysDeqIntRP; C(06h) ExceptionHandler; C(07h) InstallExceptionHandlers; C(0Ah) ChangeClearRCnt; C(0Ch) InitDefInt; C(0Dh) SetIrqAutoAck; C(1Ch) AdjustA0Table; A(45h) init_a0_b0_c0_vectors; A(44h) FlushCache | `test_irq_chain`, `test_syscalls_exceptions`, `test_timers`, `test_layout` |
| Timers | B(02h) init_timer; B(03h) get_timer; B(04h) enable_timer_irq; B(05h) disable_timer_irq; B(06h) restart_timer | `test_timers`, `test_irq_chain` |
| Pads | B(12h) InitPad; B(13h) StartPad; B(14h) StopPad; B(15h) OutdatedPadInitAndStart; B(16h) OutdatedPadGetButtons; B(5Bh) ChangeClearPad | `test_pads` |
| Memory cards | B(4Ah) InitCard; B(4Bh) StartCard; B(4Ch) StopCard; B(4Dh) / A(ABh) _card_info; B(4Eh) write_card_sector; B(4Fh) read_card_sector; B(50h) allow_new_card; B(58h) _card_chan; B(5Ch) _card_status; B(5Dh) _card_wait; A(55h) / A(70h) _bu_init; A(ACh) _card_async_load_directory; A(ADh) set_card_auto_format | `test_cards` |
| System | A(9Ch) SetConf; A(9Dh) GetConf; A(9Fh) SetMemSize; A(B4h) GetSystemInfo; B(51h) Krom2RawAdd (-1); B(56h) GetC0Table; B(57h) GetB0Table | `test_disc`, `test_layout` |
| No-ops (return 0) | A(57h)..A(5Ah), A(73h)..A(77h), A(96h)..A(99h) Add*Device, A(9Eh) SetCdromIrqAutoAbort; B(47h) AddDrv; B(48h) DelDrv; B(49h) PrintInstalledDevices; C(12h) InstallDevices; C(13h) FlushStdInOutPut | `test_layout` |

190 calls in all (108 A, 66 B, 16 C, counting the entries both tables share).

## Tests

`consoles/ps1/tests` (see its CMakeLists.txt): `ps1_hle` (the unit tests above); `ps1_hle_hb_*` our homebrew under
HLE, `ps1_hle_same_*` the same result block as under OpenBIOS, `ps1_hle_hb_disc_*` the discs booted by the HLE
BIOS, `ps1_hle_mcard` the memory card save (new card, saved card, unformatted card), `ps1_hle_jcz_*` JaCzekanski's
ps1-tests, `ps1_hle_rc_*` recompiled programs in lockstep with the interpreter under HLE.
