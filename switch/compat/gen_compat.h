/* Force-included into the generated code (switch/Makefile GENFLAGS).
 * [familia] Newer pcrecomp lifters emit RECOMP_TRAP for int1/int3/into (upstream
 * offers the fault to the guest's SEH handlers). In Mafia these are only data
 * bytes decoded as code: reaching one is fatal and names the address. */
#pragma once
#include <stdint.h>
void recomp_unimplemented(uint32_t va, const char *insn);
#ifndef RECOMP_TRAP
#define RECOMP_TRAP(code_, va_) recomp_unimplemented((va_), "trap")
#endif
