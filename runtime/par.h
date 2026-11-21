/* par.h - fork-join for independent loop iterations (par.c) */
#pragma once
#include <stdint.h>
/* fn(ctx, begin, end) for [0, n) in chunks; returns when all are done */
typedef void (*fam_par_fn)(void *ctx, uint32_t begin, uint32_t end);
void fam_par_for(fam_par_fn fn, void *ctx, uint32_t n, uint32_t chunk);
