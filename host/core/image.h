/* Puts the game image at its own addresses and runs the low range from its copy. */
#ifndef GMV_IMAGE_H
#define GMV_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include "macho.h"

typedef struct {
    uint32_t addr;
    uint8_t kind, reg;
    uint16_t pad;
} gmvp_patch;

enum { GMVP_REL32_OUT = 1, GMVP_PIC_POP, GMVP_REL32_IN, GMVP_PTR_IN };

typedef struct {
    uint32_t macho_size, text_crc32, low_start, low_end, npatch;
    const gmvp_patch *patch;
} gmvp;

typedef struct {
    uint32_t mapped_bytes, copied_low_bytes;
    uint32_t applied[5];            /* by kind */
} image_stats;

uint32_t crc32_bytes(const void *p, size_t n);
/* Validates a gmv_patch.bin against the Mach-O it claims to describe. */
int gmvp_parse(gmvp *t, const uint8_t *data, uint32_t size, const macho *m, char *err, size_t errsz);
/* Copies the segments into the reserved range (the low range into its copy). */
int image_copy(const macho *m, const gmvp *t, image_stats *st, char *err, size_t errsz);
/* Bytes that differ between the mapped image and the file; 0 right after image_copy. */
uint32_t image_compare(const macho *m, const gmvp *t);
/* Applies the low-range patches.  Every patch's expected bytes are checked first; on any
   mismatch nothing is written and -1 is returned. */
int image_patch(const macho *m, const gmvp *t, image_stats *st, char *err, size_t errsz);
/* Where game code at a game address actually lives (the copy for the low range). */
uint32_t image_code_addr(const gmvp *t, uint32_t addr);

#endif
