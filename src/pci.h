#pragma once
#include <stdint.h>

/*
 * pci — PCI bus enumeration and configuration-space driver (SCRUM-209).
 *
 * Ring-0 only, and deliberately syscall-free, the same relationship
 * src/pic.c/pit.c/ata.c have to their hardware: this file has no notion of a
 * caller or an owner. It exists because nothing in src/ walked the PCI bus
 * before, and every real-hardware device this kernel might ever want -- the
 * Intel HDA audio controller the stretch tickets are aiming at first -- is
 * found, sized and enabled through config space.
 *
 * Access is legacy configuration mechanism #1 (the 0xCF8/0xCFC I/O port
 * pair), not ECAM/MMCONFIG. Mechanism #1 is what every PCI-era host bridge
 * must implement, PCIe systems included, and it needs no ACPI table parsing
 * to find a base address -- so it is the one mechanism that works before
 * this kernel has any ACPI support at all. Its cost is the 256-byte config
 * space limit per function (no PCIe extended capabilities above offset
 * 0xFF); ECAM can be layered behind the same pci_config_*() API later
 * without touching a caller.
 *
 * Mechanism #2 is not implemented and will not be: it was deprecated in PCI
 * 2.0 (1993) and QEMU does not emulate it.
 *
 * Enumeration is the recursive scan, not brute force: bus 0 is walked, and a
 * PCI-to-PCI bridge (class 0x06, subclass 0x04) found on it is followed to
 * its secondary bus. A visited-bus bitmap makes that recursion terminate
 * even if a bridge reports a secondary bus number that loops back -- a
 * malformed device must not hang boot. Bus numbers are read, never assigned:
 * QEMU's firmware has already configured the bridges by the time GRUB hands
 * off, and reassigning them would mean owning the whole memory-window
 * forwarding problem for no gain on the one platform this targets.
 */

/* Configuration mechanism #1's two I/O ports. */
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* Config-space offsets, header type 0x0 (and the common header above 0x10). */
#define PCI_OFF_VENDOR_ID   0x00
#define PCI_OFF_DEVICE_ID   0x02
#define PCI_OFF_COMMAND     0x04
#define PCI_OFF_STATUS      0x06
#define PCI_OFF_REVISION    0x08
#define PCI_OFF_PROG_IF     0x09
#define PCI_OFF_SUBCLASS    0x0A
#define PCI_OFF_CLASS       0x0B
#define PCI_OFF_HEADER_TYPE 0x0E
#define PCI_OFF_BAR0        0x10
#define PCI_OFF_SECONDARY_BUS 0x19   /* header type 0x1 only */

/* Interrupt routing, header type 0x0. `Interrupt Pin` is read-only and names
 * which of INTA#-INTD# the function drives (0 = none); `Interrupt Line` is a
 * read/write byte the firmware fills in with the 8259 IRQ that pin was routed
 * to. Nothing in hardware enforces that the two agree -- the Line register is
 * purely a place for firmware to leave a note for the OS -- but on QEMU (as on
 * any BIOS machine) it is filled in correctly before GRUB runs, and it is the
 * only way to learn the IRQ without an I/O APIC or ACPI tables. src/hda.c
 * reads both: the pin to confirm the function drives an interrupt at all, the
 * line to pick the IDT vector. */
#define PCI_OFF_INTERRUPT_LINE 0x3C
#define PCI_OFF_INTERRUPT_PIN  0x3D

/* Command register bits (see the PCI spec's command register layout). */
#define PCI_CMD_IO_SPACE     (1u << 0)
#define PCI_CMD_MEM_SPACE    (1u << 1)
#define PCI_CMD_BUS_MASTER   (1u << 2)

/* Header type register: bit 7 is the multi-function flag, bits 6-0 the type. */
#define PCI_HEADER_TYPE_MASK 0x7F
#define PCI_HEADER_MULTIFUNC 0x80
#define PCI_HEADER_GENERAL   0x00
#define PCI_HEADER_BRIDGE    0x01

/* Class/subclass of the device this ticket's acceptance names: QEMU's
 * `-device intel-hda` reports class 0x04 (multimedia controller), subclass
 * 0x03 (audio device) -- i.e. the "0x0403" of the ticket. */
#define PCI_CLASS_MULTIMEDIA     0x04
#define PCI_SUBCLASS_AUDIO_DEV   0x03
#define PCI_CLASS_BRIDGE         0x06
#define PCI_SUBCLASS_PCI_BRIDGE  0x04

/* A non-existent function answers a config read with all ones -- there is no
 * vendor 0xFFFF, which is exactly what makes the probe work. */
#define PCI_VENDOR_NONE 0xFFFF

#define PCI_OK        0
#define PCI_ENODEV  (-1)   /* no such device/function, or BAR unimplemented */
#define PCI_EINVAL  (-2)   /* bad argument (BAR index, null pointer, ...)   */
#define PCI_ENOSPC  (-3)   /* MMIO window exhausted while assigning a BAR   */

/* How many discovered functions pci_init() will record. QEMU's default
 * machine plus a handful of -device flags is well under a dozen; the table
 * is fixed-size because this runs before any allocator the kernel would want
 * to depend on at enumeration time. Overflow is reported on serial and
 * counted by pci_devices_skipped(), never silently dropped. */
#define PCI_MAX_DEVICES 32

/* Every BAR index a header-type-0x0 function can have. */
#define PCI_MAX_BARS 6

/* One discovered PCI function. */
typedef struct {
    uint8_t  bus;
    uint8_t  device;        /* 0-31  */
    uint8_t  function;      /* 0-7   */
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  revision;
    uint8_t  prog_if;
    uint8_t  subclass;
    uint8_t  class_code;
    uint8_t  header_type;   /* masked: the MF bit is not stored here */
    uint8_t  multifunction; /* 1 if the MF bit was set on function 0  */
} pci_device_t;

/* One decoded Base Address Register. `size` is 0 for an unimplemented BAR.
 * For a 64-bit memory BAR, `base`/`size` describe the whole pair and the
 * upper half's index (index + 1) is not separately reportable. */
typedef struct {
    uint64_t base;
    uint64_t size;
    uint8_t  is_io;          /* 1: I/O space BAR, 0: memory space BAR */
    uint8_t  is_64;          /* 1: 64-bit memory BAR (consumes two slots) */
    uint8_t  prefetchable;
} pci_bar_t;

/*
 * Raw configuration-space access. `offset` is a byte offset into the
 * function's 256-byte config space; the 8/16-bit forms do the aligned
 * 32-bit access mechanism #1 requires and shift/mask in software, so a
 * 16-bit read must still be 2-byte aligned and a 32-bit one 4-byte aligned.
 * A read of a function that does not exist returns all ones rather than
 * faulting -- that is the host bridge's defined behaviour, not a bug.
 */
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
uint8_t  pci_config_read8 (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                        uint32_t val);
void pci_config_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                        uint16_t val);

/*
 * Walk the bus and fill the device table. Idempotent: calling it again
 * re-scans from scratch, which is what lets a test drive it without caring
 * whether kernel_main got there first. Never fails -- a machine with no PCI
 * at all simply enumerates zero devices, because the absent host bridge
 * reads back as vendor 0xFFFF like any other absent function.
 */
void pci_init(void);

/* How many functions the last pci_init() recorded, and how many it found but
 * had no table slot for. */
int pci_device_count(void);
int pci_devices_skipped(void);

/* The idx'th discovered function, in discovery order, or NULL if idx is out
 * of range. The pointer is into the driver's static table and stays valid
 * until the next pci_init(). */
const pci_device_t *pci_device_at(int idx);

/* First discovered function matching a class/subclass, or a vendor/device ID
 * pair. NULL if there is none. */
const pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);
const pci_device_t *pci_find_id(uint16_t vendor_id, uint16_t device_id);

/*
 * Decode BAR `index` (0..PCI_MAX_BARS-1) of `dev` into *out, sizing it in
 * the process. Returns PCI_OK, PCI_EINVAL for a bad argument or a non-
 * general header, or PCI_ENODEV for a BAR the function does not implement
 * (out->size == 0 is set in that case too).
 *
 * Sizing writes all-ones to the register and reads back the writable bits,
 * so it is destructive for the duration of the call: the original value is
 * saved and restored, and -- per the PCI spec's warning that some devices
 * decode the all-ones write as an unintended access -- I/O and memory decode
 * are disabled in the command register for the whole sequence and restored
 * afterwards. Not safe to call concurrently with a driver actively using the
 * device; call it during discovery, before handing the device to one.
 */
int pci_bar_read(const pci_device_t *dev, int index, pci_bar_t *out);

/*
 * Assign an unprogrammed memory BAR a base address out of the kernel's MMIO
 * window and write it back. Returns PCI_OK (BAR now programmed, *out updated),
 * PCI_EINVAL for an I/O-space BAR or a bad argument, PCI_ENODEV for an
 * unimplemented BAR, or PCI_ENOSPC when the window cannot fit it.
 *
 * On QEMU (and any BIOS/UEFI machine) firmware has already programmed every
 * BAR before GRUB runs, so this is normally a no-op path -- pci_bar_read()
 * reports a non-zero base and callers never need it. It exists for the case
 * firmware left one at zero, and because a BAR is only usable once something
 * owns the decision of where it lives.
 */
int pci_bar_assign(const pci_device_t *dev, int index, pci_bar_t *out);

/*
 * Turn on the decoders a driver needs: bus mastering always, memory space if
 * the function has any memory BAR, I/O space if it has any I/O BAR. Returns
 * PCI_OK or PCI_EINVAL. Leaves every other command-register bit alone.
 */
int pci_enable_device(const pci_device_t *dev);

/*
 * Identity-map a memory BAR's window into the kernel address space,
 * uncacheable (PCD), so the returned base is a pointer a driver can actually
 * dereference. vmm_init() maps RAM and the framebuffer aperture only, so
 * every other BAR lands on unmapped memory until this runs. Returns PCI_OK,
 * PCI_EINVAL for an I/O BAR/unassigned base, or the negative PCI_* mapping
 * of a vmm failure. Safe to call twice for the same BAR.
 */
int pci_bar_map(const pci_bar_t *bar);

/* Print the device table on serial, one line per function. Called from
 * kernel_main after pci_init(); exposed for debugging. */
void pci_dump(void);
