#ifndef VORTEX_OBJECT_H
#define VORTEX_OBJECT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vortex_config.h"

/**
 * VORTEX Tagged Value Representation
 *
 * Uses NaN-boxing to store doubles, SMIs, heap pointers, booleans,
 * null, and undefined all in a single 64-bit value.
 *
 * Layout:
 *   Non-NaN doubles: stored as raw 64-bit IEEE 754 bits. The exponent
 *     field is NOT all-1s, so these never conflict with NaN-boxed values.
 *
 *   NaN doubles: stored as 0x7FF8000000000000 | VTX_TAG_DOUBLE.
 *     Canonicalized to a single quiet NaN representation.
 *
 *   All other types (SMI, heap ptr, bool, null, undefined) are NaN-boxed:
 *     bits[63:52] = 0x7FF  (all 1s — NaN exponent)
 *     bit [51]    = 1      (quiet NaN bit)
 *     bits[50:3]  = data   (48 bits of payload)
 *     bits[2:0]   = tag    (3 bits identifying the type)
 *
 *   This encoding ensures that:
 *   - Non-NaN doubles have exponent != 0x7FF, so they are never
 *     confused with NaN-boxed values.
 *   - NaN-boxed values always have the quiet NaN bit set and a
 *     non-zero mantissa, so they are valid quiet NaNs.
 *
 * Tag values:
 *   0 = SMI       (data = int64_t value, sign-extended from 48 bits)
 *   1 = heap ptr  (data = pointer >> 3, pointer is 8-byte aligned)
 *   2 = double    (special: stored as raw bits or canonical NaN)
 *   3 = boolean   (data = 0 for false, 1 for true)
 *   4 = null      (data = 0)
 *   5 = undefined (data = 0)
 */

/* Tag enumeration */
#define VTX_TAG_SMI       0
#define VTX_TAG_HEAP_PTR  1
#define VTX_TAG_DOUBLE    2
#define VTX_TAG_BOOL      3
#define VTX_TAG_NULL      4
#define VTX_TAG_UNDEFINED 5

/* Tagged value type */
typedef uint64_t vtx_value_t;

/* NaN-boxing header: a quiet NaN with the exponent all-1s and quiet bit set */
#define VTX_NAN_BOX_HEADER 0x7FF8000000000000ULL

/* Mask to extract just the payload data (bits[50:3] = 48 bits).
 * After right-shifting by VTX_NAN_DATA_SHIFT, the data occupies bits[47:0],
 * so the mask must be 48 bits wide: (1ULL << 48) - 1. */
#define VTX_NAN_DATA_SHIFT 3
#define VTX_NAN_DATA_MASK  0x0000FFFFFFFFFFFFULL  /* bits[47:0] = 48 bits after shift */

/* Special value constants (NaN-boxed) */
#define VTX_VALUE_NULL      ((vtx_value_t)(VTX_NAN_BOX_HEADER | VTX_TAG_NULL))
#define VTX_VALUE_UNDEFINED ((vtx_value_t)(VTX_NAN_BOX_HEADER | VTX_TAG_UNDEFINED))
#define VTX_VALUE_FALSE     ((vtx_value_t)(VTX_NAN_BOX_HEADER | VTX_TAG_BOOL))
#define VTX_VALUE_TRUE      ((vtx_value_t)(VTX_NAN_BOX_HEADER | (1ULL << VTX_NAN_DATA_SHIFT) | VTX_TAG_BOOL))

/* ========================================================================== */
/* Internal NaN-boxing detection                                               */
/* ========================================================================== */

/**
 * Check if a value is NaN-boxed (i.e., has our quiet NaN signature).
 * Non-NaN doubles will NOT match this pattern because their exponent
 * is not all-1s with the quiet bit set simultaneously in this way.
 */
static inline bool vtx_is_nan_boxed(vtx_value_t v)
{
    return (v & VTX_NAN_BOX_HEADER) == VTX_NAN_BOX_HEADER;
}

/* ========================================================================== */
/* Tag checking predicates                                                     */
/* ========================================================================== */

static inline bool vtx_is_smi(vtx_value_t v)
{
    return vtx_is_nan_boxed(v) && (v & VTX_TAG_MASK) == VTX_TAG_SMI;
}

static inline bool vtx_is_heap_ptr(vtx_value_t v)
{
    return vtx_is_nan_boxed(v) && (v & VTX_TAG_MASK) == VTX_TAG_HEAP_PTR;
}

/**
 * A double is identified by NOT being NaN-boxed (raw non-NaN double)
 * or by being NaN-boxed with the VTX_TAG_DOUBLE tag (canonical NaN double).
 */
static inline bool vtx_is_double(vtx_value_t v)
{
    if (!vtx_is_nan_boxed(v)) {
        /* Not NaN-boxed → raw non-NaN double */
        return true;
    }
    /* NaN-boxed: check if the tag is VTX_TAG_DOUBLE */
    return (v & VTX_TAG_MASK) == VTX_TAG_DOUBLE;
}

static inline bool vtx_is_bool(vtx_value_t v)
{
    return vtx_is_nan_boxed(v) && (v & VTX_TAG_MASK) == VTX_TAG_BOOL;
}

static inline bool vtx_is_null(vtx_value_t v)
{
    return vtx_is_nan_boxed(v) && (v & VTX_TAG_MASK) == VTX_TAG_NULL;
}

static inline bool vtx_is_undefined(vtx_value_t v)
{
    return vtx_is_nan_boxed(v) && (v & VTX_TAG_MASK) == VTX_TAG_UNDEFINED;
}

/* ========================================================================== */
/* Value extraction                                                            */
/* ========================================================================== */

/**
 * Extract the SMI value. The value is stored in bits[50:3] and is
 * sign-extended from 48 bits to int64_t.
 */
static inline int64_t vtx_smi_value(vtx_value_t v)
{
    VTX_ASSERT(vtx_is_smi(v), "value is not an SMI");
    /* Extract data bits[50:3], then sign-extend from 48 bits */
    uint64_t raw = (v >> VTX_NAN_DATA_SHIFT) & VTX_NAN_DATA_MASK;
    /* Sign-extend: if bit 47 is set, fill bits 63:48 with 1s */
    if (raw & (1ULL << 47)) {
        raw |= ~VTX_NAN_DATA_MASK;
    }
    return (int64_t)raw;
}

/**
 * Extract the heap pointer. The pointer was stored as ptr >> 3
 * in bits[50:3]. We extract and shift back.
 */
static inline void *vtx_heap_ptr(vtx_value_t v)
{
    VTX_ASSERT(vtx_is_heap_ptr(v), "value is not a heap pointer");
    uint64_t raw = (v >> VTX_NAN_DATA_SHIFT) & VTX_NAN_DATA_MASK;
    return (void *)(uintptr_t)(raw << 3);
}

/**
 * Extract the double value. For non-NaN doubles, the raw bits are
 * the IEEE 754 representation. For NaN doubles, we return a
 * canonical quiet NaN.
 */
static inline double vtx_double_value(vtx_value_t v)
{
    VTX_ASSERT(vtx_is_double(v), "value is not a double");
    if (!vtx_is_nan_boxed(v)) {
        /* Raw non-NaN double */
        union { uint64_t bits; double d; } u;
        u.bits = v;
        return u.d;
    }
    /* NaN double: return canonical quiet NaN */
    union { uint64_t bits; double d; } u;
    u.bits = 0x7FF8000000000000ULL;
    return u.d;
}

/**
 * Extract the boolean value.
 */
static inline bool vtx_bool_value(vtx_value_t v)
{
    VTX_ASSERT(vtx_is_bool(v), "value is not a boolean");
    uint64_t raw = (v >> VTX_NAN_DATA_SHIFT) & VTX_NAN_DATA_MASK;
    return raw != 0;
}

/* ========================================================================== */
/* Value construction                                                          */
/* ========================================================================== */

static inline vtx_value_t vtx_make_smi(int64_t val)
{
    VTX_ASSERT(val >= VTX_SMI_MIN && val <= VTX_SMI_MAX,
               "SMI value out of range");
    /* Truncate to 48-bit signed representation */
    uint64_t raw = (uint64_t)val & VTX_NAN_DATA_MASK;
    return (vtx_value_t)(VTX_NAN_BOX_HEADER | (raw << VTX_NAN_DATA_SHIFT) | VTX_TAG_SMI);
}

static inline vtx_value_t vtx_make_heap_ptr(void *ptr)
{
    VTX_ASSERT(ptr != NULL, "cannot make null heap pointer (use vtx_make_null)");
    VTX_ASSERT(((uintptr_t)ptr & VTX_TAG_MASK) == 0,
               "heap pointer must be 8-byte aligned");
    /* Verify pointer fits in 48 bits (x86-64 user-space) */
    uintptr_t p = (uintptr_t)ptr;
    VTX_ASSERT((p >> 48) == 0 || (p >> 48) == 0xFFFF,
               "pointer does not fit in 48-bit address space");
    uint64_t raw = (p >> 3) & VTX_NAN_DATA_MASK;
    return (vtx_value_t)(VTX_NAN_BOX_HEADER | (raw << VTX_NAN_DATA_SHIFT) | VTX_TAG_HEAP_PTR);
}

static inline vtx_value_t vtx_make_double(double d)
{
    union { uint64_t bits; double d; } u;
    u.d = d;

    /* Check if the double is a NaN (exponent all 1s, mantissa non-zero) */
    if (((u.bits >> 52) & 0x7FF) == 0x7FF &&
        (u.bits & 0x000FFFFFFFFFFFFFULL) != 0) {
        /* Canonicalize all NaNs to our tagged NaN double representation */
        return (vtx_value_t)(VTX_NAN_BOX_HEADER | VTX_TAG_DOUBLE);
    }
    /* Non-NaN double: store raw bits */
    return (vtx_value_t)u.bits;
}

static inline vtx_value_t vtx_make_bool(bool b)
{
    return b ? VTX_VALUE_TRUE : VTX_VALUE_FALSE;
}

static inline vtx_value_t vtx_make_null(void)
{
    return VTX_VALUE_NULL;
}

static inline vtx_value_t vtx_make_undefined(void)
{
    return VTX_VALUE_UNDEFINED;
}

/* ========================================================================== */
/* Heap object representation                                                  */
/* ========================================================================== */

/**
 * Heap object layout:
 *
 *   +-------------------+
 *   | type_id   (4)     |  Type descriptor index
 *   | gc_mark   (1)     |  GC mark bit
 *   | gc_age    (1)     |  Number of young-gen collections survived
 *   | gc_pinned (1)     |  1 if pinned (cannot be moved by GC)
 *   | gc_remembered (1) |  1 if in old gen remembered set
 *   | size      (4)     |  Total size in bytes including this header
 *   | shape_id  (4)     |  Shape/layout fingerprint
 *   | field_count (4)   |  Number of fields following this header
 *   +-------------------+
 *   | fields[0]  (8)    |  Field 0 (tagged value)
 *   | fields[1]  (8)    |  Field 1
 *   | ...               |
 *   +-------------------+
 *
 * The header is padded to 8-byte alignment. Fields are vtx_value_t (8 bytes each).
 */
typedef struct {
    uint32_t     type_id;
    uint8_t      gc_mark;
    uint8_t      gc_age;
    uint8_t      gc_pinned;
    uint8_t      gc_remembered;
    uint32_t     size;
    uint32_t     shape_id;
    uint32_t     field_count;
    vtx_value_t  fields[];
} vtx_heap_object_t;

/* Size of the header (everything before fields[]) */
#define VTX_HEAP_OBJECT_HEADER_SIZE (offsetof(vtx_heap_object_t, fields))

/**
 * Compute the total allocation size for a heap object with `nfields` fields.
 */
static inline size_t vtx_heap_object_alloc_size(uint32_t nfields)
{
    return VTX_HEAP_OBJECT_HEADER_SIZE + nfields * sizeof(vtx_value_t);
}

/**
 * Get a field value from a heap object.
 * offset is the 0-based field index.
 */
static inline vtx_value_t vtx_object_get_field(const vtx_heap_object_t *obj, uint32_t offset)
{
    VTX_ASSERT(obj != NULL, "object must not be NULL");
    VTX_ASSERT(offset < obj->field_count, "field offset out of bounds");
    return obj->fields[offset];
}

/**
 * Set a field value in a heap object.
 * offset is the 0-based field index.
 */
static inline void vtx_object_set_field(vtx_heap_object_t *obj, uint32_t offset, vtx_value_t value)
{
    VTX_ASSERT(obj != NULL, "object must not be NULL");
    VTX_ASSERT(offset < obj->field_count, "field offset out of bounds");
    obj->fields[offset] = value;
}

/* ========================================================================== */
/* Non-inline object functions (implemented in object.c)                       */
/* ========================================================================== */

/**
 * Initialize a heap object header. Does NOT initialize fields.
 */
void vtx_heap_object_init(vtx_heap_object_t *obj, uint32_t type_id,
                          uint32_t shape_id, uint32_t field_count, uint32_t total_size);

/**
 * Get the type_id from a tagged value that is a heap pointer.
 */
uint32_t vtx_value_typeid(vtx_value_t v);

#endif /* VORTEX_OBJECT_H */
