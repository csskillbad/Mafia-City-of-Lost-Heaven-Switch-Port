/* everything is linked into one static PIE: no symbol is preemptible, so
 * globals are addressed PC-relative instead of through the GOT */
#pragma GCC visibility push(hidden)
