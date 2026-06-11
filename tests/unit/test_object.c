/**
 * test_object.c — Unit tests for VORTEX tagged value representation
 *
 * Tests tagged value creation and extraction: SMI, heap ptr, double, bool,
 * null, undefined. Tests NaN-boxing edge cases. Tests heap object init and
 * field access.
 */

#include "test_framework.h"
#include "runtime/object.h"
#include "runtime/type_system.h"

#include <stdint.h>
#include <string.h>
#include <math.h>

/* ========================================================================== */
/* SMI tests                                                                    */
/* ========================================================================== */

VTX_TEST(smi_small_positive)
{
    vtx_value_t v = vtx_make_smi(42);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_FALSE(vtx_is_double(v));
    VTX_ASSERT_FALSE(vtx_is_bool(v));
    VTX_ASSERT_FALSE(vtx_is_null(v));
    VTX_ASSERT_FALSE(vtx_is_undefined(v));
    VTX_ASSERT_FALSE(vtx_is_heap_ptr(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), (int64_t)42);
}

VTX_TEST(smi_small_negative)
{
    vtx_value_t v = vtx_make_smi(-1);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), (int64_t)-1);
}

VTX_TEST(smi_zero)
{
    vtx_value_t v = vtx_make_smi(0);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), (int64_t)0);
}

VTX_TEST(smi_max)
{
    vtx_value_t v = vtx_make_smi(VTX_SMI_MAX);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), VTX_SMI_MAX);
}

VTX_TEST(smi_min)
{
    vtx_value_t v = vtx_make_smi(VTX_SMI_MIN);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), VTX_SMI_MIN);
}

VTX_TEST(smi_large_positive)
{
    int64_t val = (1LL << 45) - 1;
    vtx_value_t v = vtx_make_smi(val);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), val);
}

VTX_TEST(smi_large_negative)
{
    int64_t val = -(1LL << 45);
    vtx_value_t v = vtx_make_smi(val);
    VTX_ASSERT_TRUE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(vtx_smi_value(v), val);
}

/* ========================================================================== */
/* Double tests                                                                 */
/* ========================================================================== */

VTX_TEST(double_positive)
{
    vtx_value_t v = vtx_make_double(3.14159);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_FALSE(vtx_is_smi(v));
    VTX_ASSERT_FALSE(vtx_is_bool(v));
    double extracted = vtx_double_value(v);
    VTX_ASSERT_TRUE(fabs(extracted - 3.14159) < 1e-10);
}

VTX_TEST(double_negative)
{
    vtx_value_t v = vtx_make_double(-2.71828);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    double extracted = vtx_double_value(v);
    VTX_ASSERT_TRUE(fabs(extracted - (-2.71828)) < 1e-10);
}

VTX_TEST(double_zero)
{
    vtx_value_t v = vtx_make_double(0.0);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_EQUAL(vtx_double_value(v), 0.0);
}

VTX_TEST(double_negative_zero)
{
    vtx_value_t v = vtx_make_double(-0.0);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    /* -0.0 should be preserved as a raw non-NaN double */
    VTX_ASSERT_EQUAL(vtx_double_value(v), -0.0);
}

VTX_TEST(double_small_denormalized)
{
    vtx_value_t v = vtx_make_double(5e-324); /* smallest positive denorm */
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(vtx_double_value(v) > 0.0);
}

VTX_TEST(double_infinity)
{
    vtx_value_t v = vtx_make_double(INFINITY);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(isinf(vtx_double_value(v)));
}

VTX_TEST(double_negative_infinity)
{
    vtx_value_t v = vtx_make_double(-INFINITY);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(isinf(vtx_double_value(v)));
    VTX_ASSERT_TRUE(vtx_double_value(v) < 0.0);
}

/* ========================================================================== */
/* NaN-boxing edge cases                                                        */
/* ========================================================================== */

VTX_TEST(double_nan_canonicalized)
{
    vtx_value_t v = vtx_make_double(NAN);
    VTX_ASSERT_TRUE(vtx_is_double(v));
    VTX_ASSERT_TRUE(isnan(vtx_double_value(v)));
    /* NaN should be stored with VTX_TAG_DOUBLE */
    VTX_ASSERT_TRUE(vtx_is_nan_boxed(v));
    VTX_ASSERT_EQUAL(v & VTX_TAG_MASK, (uint64_t)VTX_TAG_DOUBLE);
}

VTX_TEST(nan_box_header_detection)
{
    /* Non-NaN double: exponent is NOT all-1s with quiet bit */
    vtx_value_t v1 = vtx_make_double(1.0);
    VTX_ASSERT_FALSE(vtx_is_nan_boxed(v1));

    /* NaN: should be NaN-boxed */
    vtx_value_t v2 = vtx_make_double(NAN);
    VTX_ASSERT_TRUE(vtx_is_nan_boxed(v2));

    /* SMI: NaN-boxed */
    vtx_value_t v3 = vtx_make_smi(0);
    VTX_ASSERT_TRUE(vtx_is_nan_boxed(v3));
}

VTX_TEST(tag_distinguishability)
{
    /* All tagged values have VTX_NAN_BOX_HEADER set in high bits */
    vtx_value_t smi  = vtx_make_smi(42);
    vtx_value_t dbl  = vtx_make_double(NAN);
    vtx_value_t bo_t = vtx_make_bool(true);
    vtx_value_t nl   = vtx_make_null();
    vtx_value_t undef = vtx_make_undefined();

    /* Each should be uniquely identified */
    VTX_ASSERT_TRUE(vtx_is_smi(smi));
    VTX_ASSERT_TRUE(vtx_is_double(dbl));
    VTX_ASSERT_TRUE(vtx_is_bool(bo_t));
    VTX_ASSERT_TRUE(vtx_is_null(nl));
    VTX_ASSERT_TRUE(vtx_is_undefined(undef));

    /* None should be misidentified */
    VTX_ASSERT_FALSE(vtx_is_smi(dbl));
    VTX_ASSERT_FALSE(vtx_is_smi(bo_t));
    VTX_ASSERT_FALSE(vtx_is_smi(nl));
    VTX_ASSERT_FALSE(vtx_is_smi(undef));

    VTX_ASSERT_FALSE(vtx_is_bool(smi));
    VTX_ASSERT_FALSE(vtx_is_null(smi));
    VTX_ASSERT_FALSE(vtx_is_undefined(smi));
}

/* ========================================================================== */
/* Boolean tests                                                                */
/* ========================================================================== */

VTX_TEST(bool_true)
{
    vtx_value_t v = vtx_make_bool(true);
    VTX_ASSERT_TRUE(vtx_is_bool(v));
    VTX_ASSERT_TRUE(vtx_bool_value(v));
}

VTX_TEST(bool_false)
{
    vtx_value_t v = vtx_make_bool(false);
    VTX_ASSERT_TRUE(vtx_is_bool(v));
    VTX_ASSERT_FALSE(vtx_bool_value(v));
}

VTX_TEST(bool_constants)
{
    VTX_ASSERT_TRUE(vtx_is_bool(VTX_VALUE_TRUE));
    VTX_ASSERT_TRUE(vtx_is_bool(VTX_VALUE_FALSE));
    VTX_ASSERT_TRUE(vtx_bool_value(VTX_VALUE_TRUE));
    VTX_ASSERT_FALSE(vtx_bool_value(VTX_VALUE_FALSE));
    VTX_ASSERT_NOT_EQUAL(VTX_VALUE_TRUE, VTX_VALUE_FALSE);
}

/* ========================================================================== */
/* Null and undefined tests                                                     */
/* ========================================================================== */

VTX_TEST(null_value)
{
    vtx_value_t v = vtx_make_null();
    VTX_ASSERT_TRUE(vtx_is_null(v));
    VTX_ASSERT_FALSE(vtx_is_undefined(v));
    VTX_ASSERT_FALSE(vtx_is_smi(v));
    VTX_ASSERT_FALSE(vtx_is_bool(v));
    VTX_ASSERT_EQUAL(v, VTX_VALUE_NULL);
}

VTX_TEST(undefined_value)
{
    vtx_value_t v = vtx_make_undefined();
    VTX_ASSERT_TRUE(vtx_is_undefined(v));
    VTX_ASSERT_FALSE(vtx_is_null(v));
    VTX_ASSERT_FALSE(vtx_is_smi(v));
    VTX_ASSERT_EQUAL(v, VTX_VALUE_UNDEFINED);
}

VTX_TEST(null_not_equal_undefined)
{
    VTX_ASSERT_NOT_EQUAL(VTX_VALUE_NULL, VTX_VALUE_UNDEFINED);
}

/* ========================================================================== */
/* Heap pointer tests                                                           */
/* ========================================================================== */

VTX_TEST(heap_ptr_roundtrip)
{
    /* Allocate an 8-byte aligned buffer for the heap object */
    _Alignas(8) char buf[256];
    vtx_heap_object_t *obj = (vtx_heap_object_t *)buf;

    vtx_heap_object_init(obj, 1, 1, 2, (uint32_t)sizeof(buf));
    vtx_value_t v = vtx_make_heap_ptr(obj);

    VTX_ASSERT_TRUE(vtx_is_heap_ptr(v));
    VTX_ASSERT_FALSE(vtx_is_smi(v));
    VTX_ASSERT_FALSE(vtx_is_double(v));
    VTX_ASSERT_FALSE(vtx_is_null(v));

    void *extracted = vtx_heap_ptr(v);
    VTX_ASSERT_NOT_NULL(extracted);
    VTX_ASSERT_EQUAL(extracted, (void *)obj);
}

VTX_TEST(heap_ptr_typeid)
{
    _Alignas(8) char buf[256];
    vtx_heap_object_t *obj = (vtx_heap_object_t *)buf;
    vtx_heap_object_init(obj, 7, 1, 0, (uint32_t)sizeof(buf));

    vtx_value_t v = vtx_make_heap_ptr(obj);
    VTX_ASSERT_EQUAL(vtx_value_typeid(v), (uint32_t)7);
}

VTX_TEST(heap_ptr_non_heap_typeid)
{
    /* Non-heap values should return 0 (VTX_TYPE_INVALID) */
    vtx_value_t smi = vtx_make_smi(42);
    VTX_ASSERT_EQUAL(vtx_value_typeid(smi), (uint32_t)0);

    vtx_value_t nl = vtx_make_null();
    VTX_ASSERT_EQUAL(vtx_value_typeid(nl), (uint32_t)0);
}

/* ========================================================================== */
/* Heap object field access tests                                               */
/* ========================================================================== */

VTX_TEST(heap_object_init_and_fields)
{
    _Alignas(8) char buf[512];
    vtx_heap_object_t *obj = (vtx_heap_object_t *)buf;

    vtx_heap_object_init(obj, 3, 99, 4, (uint32_t)sizeof(buf));

    VTX_ASSERT_EQUAL(obj->type_id, (uint32_t)3);
    VTX_ASSERT_EQUAL(obj->gc_mark, (uint8_t)0);
    VTX_ASSERT_EQUAL(obj->gc_age, (uint8_t)0);
    VTX_ASSERT_EQUAL(obj->gc_pinned, (uint8_t)0);
    VTX_ASSERT_EQUAL(obj->gc_remembered, (uint8_t)0);
    VTX_ASSERT_EQUAL(obj->shape_id, (uint32_t)99);
    VTX_ASSERT_EQUAL(obj->field_count, (uint32_t)4);

    /* Fields should be initialized to undefined */
    for (uint32_t i = 0; i < obj->field_count; i++) {
        VTX_ASSERT_TRUE(vtx_is_undefined(vtx_object_get_field(obj, i)));
    }
}

VTX_TEST(heap_object_set_get_field)
{
    _Alignas(8) char buf[512];
    vtx_heap_object_t *obj = (vtx_heap_object_t *)buf;

    vtx_heap_object_init(obj, 1, 1, 4, (uint32_t)sizeof(buf));

    vtx_value_t v1 = vtx_make_smi(100);
    vtx_value_t v2 = vtx_make_double(2.5);
    vtx_value_t v3 = vtx_make_bool(true);
    vtx_value_t v4 = vtx_make_null();

    vtx_object_set_field(obj, 0, v1);
    vtx_object_set_field(obj, 1, v2);
    vtx_object_set_field(obj, 2, v3);
    vtx_object_set_field(obj, 3, v4);

    VTX_ASSERT_EQUAL(vtx_object_get_field(obj, 0), v1);
    VTX_ASSERT_EQUAL(vtx_object_get_field(obj, 1), v2);
    VTX_ASSERT_EQUAL(vtx_object_get_field(obj, 2), v3);
    VTX_ASSERT_EQUAL(vtx_object_get_field(obj, 3), v4);

    /* Verify types through extraction */
    VTX_ASSERT_TRUE(vtx_is_smi(vtx_object_get_field(obj, 0)));
    VTX_ASSERT_EQUAL(vtx_smi_value(vtx_object_get_field(obj, 0)), (int64_t)100);

    VTX_ASSERT_TRUE(vtx_is_double(vtx_object_get_field(obj, 1)));
    VTX_ASSERT_TRUE(fabs(vtx_double_value(vtx_object_get_field(obj, 1)) - 2.5) < 1e-10);

    VTX_ASSERT_TRUE(vtx_is_bool(vtx_object_get_field(obj, 2)));
    VTX_ASSERT_TRUE(vtx_bool_value(vtx_object_get_field(obj, 2)));

    VTX_ASSERT_TRUE(vtx_is_null(vtx_object_get_field(obj, 3)));
}

VTX_TEST(heap_object_alloc_size)
{
    uint32_t nfields = 5;
    size_t expected = VTX_HEAP_OBJECT_HEADER_SIZE + nfields * sizeof(vtx_value_t);
    VTX_ASSERT_EQUAL(vtx_heap_object_alloc_size(nfields), expected);
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    return (result.fail_count > 0) ? 1 : 0;
}
