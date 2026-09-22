#pragma once

/* The most 512-byte sectors a single exo_disk_read/exo_disk_write call will
 * transfer. The whole call runs with interrupts off (`syscall`'s FMASK
 * clears IF, src/syscall.c) and each sector is a busy-polled PIO transfer
 * (src/ata.c), so an uncapped count would stall the timer and keyboard IRQs
 * for as long as the drive takes to drain it -- same reasoning as
 * src/syscall_serial.h's SERIAL_WRITE_MAX_LEN. 128 sectors (64 KiB) is
 * generous for a save-file-sized transfer. Exposed here (rather than kept
 * `static` in syscall_disk.c) so test_syscall_disk_k.c can assert the real
 * limit instead of a restated literal. */
#define EXO_DISK_MAX_SECTORS 128u

/*
 * syscall_disk.h -- exo_disk_read/exo_disk_write handlers (SCRUM-103).
 *
 * Binds #27/#28 to the dispatcher in src/syscall.c, the same split
 * syscall_serial.c already has against src/serial.c: src/ata.c knows how to
 * drive the ATA bus, this file knows what a LibOS is allowed to ask it to
 * transfer and how to answer in -EXO_E* terms. Deliberately no ownership
 * check -- the disk has no binding table yet, that is SCRUM-188, which is
 * blocked *by* this ticket rather than the other way around.
 *
 * Call from kernel_main after syscall_init() and ata_init() (the latter so
 * `drive_present` reflects whether a drive actually answered), ahead of the
 * TESTING branch, so the handlers are bound for both a normal boot and the
 * in-kernel test run -- same placement rule as every other syscall_*_init().
 */
void syscall_disk_init(int drive_present);
