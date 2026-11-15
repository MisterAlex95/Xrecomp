"""rep stos through an access-counting guest-memory fixture (XBA-38): the
forward fill over plain memory goes through recomp_rep_fill (memset, or a
host loop for a value whose bytes differ) without a guest access; backwards,
into the MMIO range, crossing into it or wrapping, element by element."""
import os
import re

from tools.recomp.lifter import Lifter
from tools.recomp.disasm import Instruction
from tools.recomp.test_rep_movs_mmio import _build_and_run

HEADER = os.path.join(os.path.dirname(__file__), "..", "..", "templates", "runtime", "recomp_types.h")


def _rep_fill():
    """recomp_rep_fill as the runtime header defines it."""
    src = open(HEADER, encoding="utf-8").read()
    m = re.search(r"^static inline void recomp_rep_fill\(.*?^}\n", src, re.S | re.M)
    assert m, "recomp_rep_fill not in recomp_types.h"
    return m.group(0)


def test_fill_fast_path_and_fallbacks_at_every_element_width():
    prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint32_t eax,edi,ecx;
int g_df;
unsigned accesses;
uint8_t mem[131076],refmem[131076];
unsigned slot(uint32_t a){return (a&0xffffu)+(a>=0xFD000000u?65536u:0u);}
void* touch(uint32_t a){accesses++;return mem+slot(a);}
#define XBOX_PTR(a) (mem+slot(a))
#define MEM8(a) (*(volatile uint8_t*)touch(a))
#define MEM16(a) (*(volatile uint16_t*)touch(a))
#define MEM32(a) (*(volatile uint32_t*)touch(a))
#define LO8(r) ((uint8_t)(r))
#define LO16(r) ((uint16_t)(r))
#define RECOMP_DF_STEP(s) (g_df?-(s):(s))
''' + _rep_fill()
    functions = []
    for size in (1, 2, 4):
        mnem = {1: "rep stosb", 2: "rep stosw", 4: "rep stosd"}[size]
        functions.append(f"void fill_{size}(void){{" +
                         "".join(Lifter().lift_instruction(Instruction(0, 2, mnem, "", ""))) + "}")
    main = r'''
int main(void){
    void(*fills[])(void)={fill_1,fill_2,fill_4};
    const unsigned sizes[]={1,2,4};
    const uint32_t values[]={0x00000000u,0x41414141u,0x12345678u,0x0000FF00u};
    for(unsigned w=0;w<3;w++) for(unsigned v=0;v<4;v++) for(unsigned mode=0;mode<7;mode++)
    for(unsigned n=0;n<=9;n++){
        unsigned z=sizes[w];
        uint32_t d=0x400;
        int df=0;
        if(mode==1){d+=8*z;df=1;}            /* backwards */
        if(mode==2)d=0xFD000400;             /* hardware */
        if(mode==3)d=0xFD000000-2*z;         /* range crosses into hardware */
        if(mode==4)d=0xFFFFFFF0u;            /* address wrap */
        if(mode==5)d=0xFD000000-n*z;         /* ends at the aperture */
        if(mode==6)d=0x401;                  /* unaligned */
        for(unsigned i=0;i<sizeof mem;i++)mem[i]=refmem[i]=(uint8_t)(i*37+13);
        uint32_t rd=d;
        for(unsigned i=0;i<n;i++){
            memcpy(refmem+slot(rd),&values[v],z);
            rd+=df?-(int)z:z;
        }
        eax=values[v];edi=d;ecx=n;g_df=df;accesses=0;fills[w]();
        int fast=!df && d<0xFD000000u && (uint64_t)d+n*z<=0xFD000000u;
        if(memcmp(mem,refmem,sizeof mem) || edi!=rd || ecx!=0 || eax!=values[v]
                || accesses!=(fast?0:n)){
            printf("size %u value %08X mode %u count %u access %u\n",z,values[v],mode,n,accesses);
            return 1;
        }
    }
    return 0;
}
'''
    ran = _build_and_run(prelude + "\n".join(functions) + main)
    assert ran.returncode == 0, ran.stdout + ran.stderr
