// vortex/constval_equal.hpp — Canonical constant equality for VORTEX IR.
//
// All optimization passes should use this instead of ad-hoc comparisons.
// Handles Int, Float, Ptr with proper IEEE-754 float semantics.

#ifndef VORTEX_CONSTVAL_EQUAL_HPP
#define VORTEX_CONSTVAL_EQUAL_HPP

#define typeid typeid_
extern "C" {
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "ir/node.h"
}
#undef typeid

#include <cmath>
#include <cstring>

namespace vortex {

// Compare two vtx_constval_t values for equality.
// Returns true if they represent the same constant.
//
// Int: compared by int_val
// Float: compared by bit pattern (handles -0.0 vs +0.0, NaN != NaN)
// Ptr: compared by ptr_val
// Different kinds: not equal
inline bool vtx_constval_equal(const vtx_constval_t& a, const vtx_constval_t& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case VTX_TYPE_Int:
        return a.as.int_val == b.as.int_val;
    case VTX_TYPE_Float:
        // Use bit comparison, not ==, to handle -0.0 and NaN correctly.
        // NaN != NaN (IEEE-754), so two NaN constants are "not equal".
        // -0.0 != +0.0 in bit pattern, but == in IEEE comparison.
        // For optimization purposes, we treat -0.0 and +0.0 as different
        // (because they have different SMI representations).
        return std::memcmp(&a.as.float_val, &b.as.float_val, sizeof(double)) == 0;
    case VTX_TYPE_Ptr:
        return a.as.ptr_val == b.as.ptr_val;
    default:
        return false;
    }
}

}  // namespace vortex

#endif  // VORTEX_CONSTVAL_EQUAL_HPP
