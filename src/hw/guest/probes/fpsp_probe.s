| FPSP probe (tests/verify.py, os-boot): floating-point operations the 68040
| leaves to the ROM's floating-point package (unimplemented instructions and
| data types, exceptions disabled), each result stored for comparison of
| the trapping FPU with emulation.fpu_full. Position independent; run by
| mc_hw_trace --run-code, which prints the last 512 bytes (res).
        .text
start:
        fmove.l #0,%fpcr
        lea     res(%pc),%a5            | results
        lea     ops(%pc),%a0
| 1. unimplemented instructions with (An)+ and -(An) operands
        fsin.x  (%a0)+,%fp0             | A0 must step by 12 exactly once
        fmove.x %fp0,(%a5)+
        move.l  %a0,(%a5)+
        lea     ops+24(%pc),%a0
        fetox.x -(%a0),%fp1
        fmove.x %fp1,(%a5)+
        move.l  %a0,(%a5)+
        lea     ops(%pc),%a0
        fcos.x  (%a0)+,%fp2
        flogn.x (%a0)+,%fp3
        fatan.x (%a0)+,%fp4
        fmove.x %fp2,(%a5)+
        fmove.x %fp3,(%a5)+
        fmove.x %fp4,(%a5)+
        move.l  %a0,(%a5)+
        lea     sgl(%pc),%a0
        ftwotox.s (%a0)+,%fp5
        fmove.x %fp5,(%a5)+
        move.l  %a0,(%a5)+
        lea     dbl(%pc),%a0
        ftentox.d (%a0)+,%fp6
        fmove.x %fp6,(%a5)+
        move.l  %a0,(%a5)+
| 2. sin/cos pair and mod/rem
        lea     ops(%pc),%a0
        fmove.x (%a0),%fp0
        fsincos.x %fp0,%fp1:%fp2
        fmove.x %fp1,(%a5)+
        fmove.x %fp2,(%a5)+
        fmove.x ops+12(%pc),%fp3
        fmod.x  %fp0,%fp3
        fmove.x %fp3,(%a5)+
        fmove.l %fpsr,(%a5)+
| 3. constants and packed decimal
        fmovecr #0,%fp0                 | pi
        fmove.x %fp0,(%a5)+
        lea     pkd(%pc),%a0
        fmove.p (%a0)+,%fp1
        fmove.x %fp1,(%a5)+
        move.l  %a0,(%a5)+
        fmove.p %fp1,(%a5){#5}
        lea     12(%a5),%a5
| 4. exceptions disabled: overflow, underflow, operand error results
        fmove.l #0,%fpsr
        fmove.x big(%pc),%fp0
        fmul.x  %fp0,%fp0               | overflow
        fmove.x %fp0,(%a5)+
        fmove.l %fpsr,(%a5)+
        fmove.l #0,%fpsr
        fmove.x tiny(%pc),%fp1
        fmul.x  %fp1,%fp1               | underflow
        fmove.x %fp1,(%a5)+
        fmove.l %fpsr,(%a5)+
        fmove.l #0,%fpsr
        fmove.x big(%pc),%fp2
        fmove.l %fp2,%d0                | operand error: out of range
        move.l  %d0,(%a5)+
        fmove.l %fpsr,(%a5)+
        fmove.l #0,%fpsr
        fmove.s nan(%pc),%fp3
        moveq   #0,%d1                  | the word result leaves the upper half
        fmove.w %fp3,%d1                | NaN to integer
        move.l  %d1,(%a5)+
        fmove.l %fpsr,(%a5)+
| 5. a denormalized operand (unsupported data type on the 68040)
        fmove.l #0,%fpsr
        fmove.x den(%pc),%fp4
        fadd.x  ops(%pc),%fp4
        fmove.x %fp4,(%a5)+
        fmove.x den(%pc),%fp5
        fmul.x  ops+12(%pc),%fp5
        fmove.x %fp5,(%a5)+
        fmove.l %fpsr,(%a5)+
| 6. FINTRZ, FGETEXP, FGETMAN, FSCALE (unimplemented on the 68040)
        fmove.x ops+12(%pc),%fp0
        fintrz.x %fp0,%fp1
        fgetexp.x %fp0,%fp2
        fgetman.x %fp0,%fp3
        fmove.x ops(%pc),%fp4
        fscale.l #3,%fp4
        fmove.x %fp1,(%a5)+
        fmove.x %fp2,(%a5)+
        fmove.x %fp3,(%a5)+
        fmove.x %fp4,(%a5)+
        move.l  #0x4C564458,(%a5)+      | "LVDX": done
done:   bra.s   done
        .even
ops:    .long   0x3FFF0000,0xC90FDAA2,0x2168C235   | pi/2 ~ 1.5708 (x)
        .long   0x40010000,0xA0000000,0x00000000   | 5.0
        .long   0x3FFE0000,0x80000000,0x00000000   | 0.5
sgl:    .long   0x40400000                         | 3.0
dbl:    .long   0x40000000,0x00000000              | 2.0
pkd:    .long   0x00030001,0x23456789,0xA0000000   | 1.23456789A e3 (packed)
big:    .long   0x7FFE0000,0x80000000,0x00000000
tiny:   .long   0x00010000,0x80000000,0x00000000
nan:    .long   0x7FC00000
den:    .long   0x00000000,0x40000000,0x00000000
res:    .space  512
