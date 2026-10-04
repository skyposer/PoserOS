#ifndef PSTD_COMPILERCMD_H
#define PSTD_COMPILERCMD_H

#define COMP_CLSFILL    __attribute__((packed))
#define COMP_ALIGN(n)   __attribute__((aligned(n)))
#define COMP_INLINE     __attribute__((always_inline)) inline
#define COMP_NINLINE    __attribute__((noinline))
#define COMP_NAKED  __attribute__((naked))

#endif