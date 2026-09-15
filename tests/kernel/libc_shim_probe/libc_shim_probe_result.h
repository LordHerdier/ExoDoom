#ifndef LIBC_SHIM_PROBE_RESULT_H
#define LIBC_SHIM_PROBE_RESULT_H

/*
 * Result bits libc_shim_probe_main() ORs together and hands back through
 * libos_return() (SCRUM-51). Shared between libc_shim_probe.c (which sets
 * them) and test_libc_shim_probe_k.c (which asserts on them), so the two
 * cannot silently drift apart the way two independently-typed literals
 * could.
 */
#define LIBC_SHIM_OK_MALLOC (1u << 0)
#define LIBC_SHIM_OK_PRINTF (1u << 1)
#define LIBC_SHIM_OK_TICKS  (1u << 2)
#define LIBC_SHIM_OK_FB     (1u << 3)

#define LIBC_SHIM_OK_ALL \
    (LIBC_SHIM_OK_MALLOC | LIBC_SHIM_OK_PRINTF | LIBC_SHIM_OK_TICKS | \
     LIBC_SHIM_OK_FB)

#endif /* LIBC_SHIM_PROBE_RESULT_H */
