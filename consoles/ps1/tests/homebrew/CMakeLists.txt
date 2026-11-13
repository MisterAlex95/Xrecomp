# PS1 homebrew corpus (XBA-149). A standalone PSn00bSDK project, NOT part of the repo's CMake tree:
# it only configures with the PSn00bSDK toolchain inside the xba-ps1-homebrew image
# (tools/docker/ps1-homebrew.Dockerfile), normally through tools/consoles/build_homebrew.sh:
#   cmake -S consoles/ps1/tests/homebrew -B /tmp/build -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$PSN00BSDK_LIBS/cmake/sdk.cmake
#   cmake --build /tmp/build -j4
# Every program gives <prog>.exe (PS-X EXE), <prog>.elf, and <prog>.bin + <prog>.cue (a disc with
# SYSTEM.CNF booting it). overlay's disc also holds OVL_A.BIN and OVL_B.BIN.

cmake_minimum_required(VERSION 3.21)

project(xba-ps1-homebrew LANGUAGES C ASM VERSION 1.0.0
        DESCRIPTION "XBA PS1 homebrew test corpus")

# PSn00bSDK's addPrim()/setaddr() macros pun OT words: keep that legal and quiet.
set(XBA_CFLAGS -Wall -Wextra -Werror -fno-strict-aliasing -Wno-array-bounds)

function(xba_ps1_program prog disc_name)
    file(GLOB srcs CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/${prog}/main.c ${PROJECT_SOURCE_DIR}/${prog}/*.s)
    psn00bsdk_add_executable(${prog} GPREL ${srcs})
    target_compile_options(${prog} PRIVATE ${XBA_CFLAGS})
    target_include_directories(${prog} PRIVATE ${PROJECT_SOURCE_DIR}/common)

    set(XBA_PROG ${prog})
    set(XBA_DISC_NAME ${disc_name})
    string(TOUPPER ${prog} XBA_PROG_UPPER)
    file(WRITE ${PROJECT_BINARY_DIR}/${prog}.cnf
         "BOOT=cdrom:\\${disc_name}.EXE;1\r\nTCB=4\r\nEVENT=10\r\nSTACK=801FFFF0\r\n")
    configure_file(${PROJECT_SOURCE_DIR}/iso.xml ${PROJECT_BINARY_DIR}/${prog}_iso.xml)
    psn00bsdk_add_cd_image(${prog}_disc ${prog} ${PROJECT_BINARY_DIR}/${prog}_iso.xml
                           DEPENDS ${prog} ${ARGN})
endfunction()

# Overlays: flat binaries linked at 0x80100000, outside the PSn00bSDK executable machinery.
set(OVL_FLAGS -march=r3000 -mabi=32 -EL -msoft-float -mno-abicalls -fno-pic -G0 -O2
              -ffreestanding -fno-builtin -nostdlib -Wall -Wextra -Werror)
foreach(ovl a b)
    string(TOUPPER ${ovl} OVL)
    add_custom_command(
        OUTPUT ${PROJECT_BINARY_DIR}/ovl_${ovl}.elf ${PROJECT_BINARY_DIR}/OVL_${OVL}.BIN
        COMMAND ${CMAKE_C_COMPILER} ${OVL_FLAGS} -T ${PROJECT_SOURCE_DIR}/overlay/ovl.ld
                -o ${PROJECT_BINARY_DIR}/ovl_${ovl}.elf ${PROJECT_SOURCE_DIR}/overlay/ovl_${ovl}.c
        COMMAND ${CMAKE_OBJCOPY} -O binary ${PROJECT_BINARY_DIR}/ovl_${ovl}.elf
                ${PROJECT_BINARY_DIR}/OVL_${OVL}.BIN
        DEPENDS ${PROJECT_SOURCE_DIR}/overlay/ovl_${ovl}.c ${PROJECT_SOURCE_DIR}/overlay/ovl.ld
        VERBATIM)
endforeach()
add_custom_target(overlays ALL DEPENDS ${PROJECT_BINARY_DIR}/OVL_A.BIN ${PROJECT_BINARY_DIR}/OVL_B.BIN)

set(XBA_EXTRA_FILES "")
xba_ps1_program(hello HELLO)
xba_ps1_program(cpu CPU)
xba_ps1_program(timer_irq TIMERIRQ)
xba_ps1_program(gfx GFX)
set(XBA_EXTRA_FILES
    "\t\t\t<file name=\"OVL_A.BIN\" type=\"data\" source=\"OVL_A.BIN\" />\n\t\t\t<file name=\"OVL_B.BIN\" type=\"data\" source=\"OVL_B.BIN\" />")
xba_ps1_program(overlay OVERLAY ${PROJECT_BINARY_DIR}/OVL_A.BIN ${PROJECT_BINARY_DIR}/OVL_B.BIN overlays)
