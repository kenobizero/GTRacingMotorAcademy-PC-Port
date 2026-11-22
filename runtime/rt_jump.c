/* The far side of the host setjmps in the generated code (SJLJ_SETJMP, GUEST_SETJMP).
   Built with the same compiler as build/gen (clang), so the __builtin_longjmp matches the
   __builtin_setjmp buffers it made. */
#include "arm_rt.h"

__attribute__((noreturn, noinline)) void rt_longjmp_host(void **jb)
{
    RT_LONGJMP(jb);
}
