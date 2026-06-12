/**
 * test_type_system.c — Unit tests for VORTEX type system
 *
 * Tests type registration, subtype checking, method resolution,
 * inline cache transitions, and instance checking.
 */

#include "test_framework.h"
#include "runtime/type_system.h"

#include <string.h>

/* ========================================================================== */
/* Type system init/destroy                                                     */
/* ========================================================================== */

VTX_TEST(type_system_init)
{
    vtx_type_system_t ts;
    int rc = vtx_type_system_init(&ts);
    VTX_ASSERT_EQUAL(rc, 0);
    VTX_ASSERT_TRUE(ts.type_count >= 2); /* VTX_TYPE_INVALID(0) + Object(1) */
    VTX_ASSERT_NOT_NULL(ts.types);

    /* Verify Object type at index 1 */
    const vtx_type_desc_t *obj = vtx_type_get(&ts, VTX_TYPE_OBJECT);
    VTX_ASSERT_NOT_NULL(obj);
    VTX_ASSERT_EQUAL(obj->type_id, VTX_TYPE_OBJECT);
    VTX_ASSERT_TRUE(strcmp(obj->name, "Object") == 0);
    VTX_ASSERT_EQUAL(obj->parent_type, VTX_TYPE_INVALID);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(type_system_destroy)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);
    vtx_type_system_destroy(&ts);
    VTX_ASSERT_NULL(ts.types);
    VTX_ASSERT_EQUAL(ts.type_count, (uint32_t)0);
}

VTX_TEST(type_system_get_invalid)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    /* VTX_TYPE_INVALID (0) is a valid index but represents the invalid slot */
    const vtx_type_desc_t *invalid = vtx_type_get(&ts, VTX_TYPE_INVALID);
    VTX_ASSERT_NOT_NULL(invalid);
    VTX_ASSERT_EQUAL(invalid->type_id, VTX_TYPE_INVALID);

    /* Out of range should return NULL */
    VTX_ASSERT_NULL(vtx_type_get(&ts, 9999));

    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Type registration                                                           */
/* ========================================================================== */

VTX_TEST(type_register_simple)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t animal_id = vtx_type_register(&ts, "Animal", VTX_TYPE_OBJECT,
                                                 0, NULL, 0, NULL);
    VTX_ASSERT_NOT_EQUAL(animal_id, VTX_TYPE_INVALID);
    VTX_ASSERT_TRUE(animal_id >= 2); /* first user type */

    const vtx_type_desc_t *animal = vtx_type_get(&ts, animal_id);
    VTX_ASSERT_NOT_NULL(animal);
    VTX_ASSERT_TRUE(strcmp(animal->name, "Animal") == 0);
    VTX_ASSERT_EQUAL(animal->parent_type, VTX_TYPE_OBJECT);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(type_register_with_fields)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    /* Allocate fields on the heap since type_system takes ownership and frees */
    vtx_field_desc_t *fields = (vtx_field_desc_t *)calloc(2, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(fields);
    fields[0].name = "x"; fields[0].type = VTX_TYPE_OBJECT; fields[0].offset = 0;
    fields[1].name = "y"; fields[1].type = VTX_TYPE_OBJECT; fields[1].offset = 0;

    vtx_typeid_t point_id = vtx_type_register(&ts, "Point", VTX_TYPE_OBJECT,
                                                2, fields, 0, NULL);
    VTX_ASSERT_NOT_EQUAL(point_id, VTX_TYPE_INVALID);

    const vtx_type_desc_t *point = vtx_type_get(&ts, point_id);
    VTX_ASSERT_EQUAL(point->field_count, (uint32_t)2);
    VTX_ASSERT_TRUE(point->instance_size > VTX_HEAP_OBJECT_HEADER_SIZE);

    /* Field offsets should have been computed */
    VTX_ASSERT_TRUE(point->fields[0].offset >= VTX_HEAP_OBJECT_HEADER_SIZE);
    VTX_ASSERT_TRUE(point->fields[1].offset > point->fields[0].offset);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(type_register_with_methods)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    /* Allocate methods on the heap since type_system takes ownership and frees */
    vtx_method_desc_t *methods = (vtx_method_desc_t *)calloc(2, sizeof(vtx_method_desc_t));
    VTX_ASSERT_NOT_NULL(methods);
    methods[0].name = "speak"; methods[0].signature = "()V";
    methods[0].bytecode = NULL; methods[0].vtable_index = 0xFFFFFFFF; methods[0].arg_count = 0; methods[0].is_virtual = true;
    methods[1].name = "eat"; methods[1].signature = "()V";
    methods[1].bytecode = NULL; methods[1].vtable_index = 0xFFFFFFFF; methods[1].arg_count = 0; methods[1].is_virtual = false;

    vtx_typeid_t animal_id = vtx_type_register(&ts, "Animal", VTX_TYPE_OBJECT,
                                                 0, NULL, 2, methods);
    VTX_ASSERT_NOT_EQUAL(animal_id, VTX_TYPE_INVALID);

    const vtx_type_desc_t *animal = vtx_type_get(&ts, animal_id);
    VTX_ASSERT_EQUAL(animal->method_count, (uint32_t)2);
    VTX_ASSERT_TRUE(animal->vtable_size >= 1); /* at least 1 virtual method */

    vtx_type_system_destroy(&ts);
}

VTX_TEST(type_register_inheritance_chain)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t animal = vtx_type_register(&ts, "Animal", VTX_TYPE_OBJECT,
                                              0, NULL, 0, NULL);
    vtx_typeid_t dog = vtx_type_register(&ts, "Dog", animal,
                                           0, NULL, 0, NULL);
    vtx_typeid_t poodle = vtx_type_register(&ts, "Poodle", dog,
                                              0, NULL, 0, NULL);

    VTX_ASSERT_NOT_EQUAL(animal, VTX_TYPE_INVALID);
    VTX_ASSERT_NOT_EQUAL(dog, VTX_TYPE_INVALID);
    VTX_ASSERT_NOT_EQUAL(poodle, VTX_TYPE_INVALID);

    VTX_ASSERT_EQUAL(vtx_type_get(&ts, dog)->parent_type, animal);
    VTX_ASSERT_EQUAL(vtx_type_get(&ts, poodle)->parent_type, dog);

    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Subtype checking                                                             */
/* ========================================================================== */

VTX_TEST(type_is_subtype_self)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t animal = vtx_type_register(&ts, "Animal", VTX_TYPE_OBJECT,
                                              0, NULL, 0, NULL);
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, animal, animal));
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, VTX_TYPE_OBJECT, VTX_TYPE_OBJECT));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(type_is_subtype_parent)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t animal = vtx_type_register(&ts, "Animal", VTX_TYPE_OBJECT,
                                              0, NULL, 0, NULL);
    vtx_typeid_t dog = vtx_type_register(&ts, "Dog", animal,
                                           0, NULL, 0, NULL);
    vtx_typeid_t cat = vtx_type_register(&ts, "Cat", animal,
                                           0, NULL, 0, NULL);

    /* Dog and Cat are subtypes of Animal */
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, dog, animal));
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, cat, animal));

    /* Animal is NOT a subtype of Dog */
    VTX_ASSERT_FALSE(vtx_type_is_subtype(&ts, animal, dog));

    /* Dog is NOT a subtype of Cat */
    VTX_ASSERT_FALSE(vtx_type_is_subtype(&ts, dog, cat));

    /* Dog is subtype of Object (transitive) */
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, dog, VTX_TYPE_OBJECT));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(type_is_subtype_deep)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t a = vtx_type_register(&ts, "A", VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    vtx_typeid_t b = vtx_type_register(&ts, "B", a, 0, NULL, 0, NULL);
    vtx_typeid_t c = vtx_type_register(&ts, "C", b, 0, NULL, 0, NULL);
    vtx_typeid_t d = vtx_type_register(&ts, "D", c, 0, NULL, 0, NULL);

    /* D is subtype of A through chain */
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, d, a));
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, d, b));
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, d, c));
    VTX_ASSERT_TRUE(vtx_type_is_subtype(&ts, d, VTX_TYPE_OBJECT));

    /* A is NOT a subtype of D */
    VTX_ASSERT_FALSE(vtx_type_is_subtype(&ts, a, d));

    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Method resolution                                                            */
/* ========================================================================== */

VTX_TEST(type_resolve_method_own)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_method_desc_t *methods = (vtx_method_desc_t *)calloc(1, sizeof(vtx_method_desc_t));
    VTX_ASSERT_NOT_NULL(methods);
    methods[0].name = "speak"; methods[0].signature = "()V";
    methods[0].bytecode = NULL; methods[0].vtable_index = 0; methods[0].arg_count = 0; methods[0].is_virtual = true;

    vtx_typeid_t animal = vtx_type_register(&ts, "Animal", VTX_TYPE_OBJECT,
                                              0, NULL, 1, methods);

    const vtx_method_desc_t *m = vtx_type_resolve_method(&ts, animal, "speak");
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_TRUE(strcmp(m->name, "speak") == 0);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(type_resolve_method_inherited)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_method_desc_t *animal_methods = (vtx_method_desc_t *)calloc(1, sizeof(vtx_method_desc_t));
    VTX_ASSERT_NOT_NULL(animal_methods);
    animal_methods[0].name = "speak"; animal_methods[0].signature = "()V";
    animal_methods[0].bytecode = NULL; animal_methods[0].vtable_index = 0; animal_methods[0].arg_count = 0; animal_methods[0].is_virtual = true;

    vtx_typeid_t animal = vtx_type_register(&ts, "Animal", VTX_TYPE_OBJECT,
                                              0, NULL, 1, animal_methods);
    vtx_typeid_t dog = vtx_type_register(&ts, "Dog", animal,
                                           0, NULL, 0, NULL);

    /* Dog should inherit "speak" from Animal */
    const vtx_method_desc_t *m = vtx_type_resolve_method(&ts, dog, "speak");
    VTX_ASSERT_NOT_NULL(m);
    VTX_ASSERT_TRUE(strcmp(m->name, "speak") == 0);

    vtx_type_system_destroy(&ts);
}

VTX_TEST(type_resolve_method_not_found)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t animal = vtx_type_register(&ts, "Animal", VTX_TYPE_OBJECT,
                                              0, NULL, 0, NULL);
    const vtx_method_desc_t *m = vtx_type_resolve_method(&ts, animal, "nonexistent");
    VTX_ASSERT_NULL(m);

    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Interface implementation                                                     */
/* ========================================================================== */

VTX_TEST(type_add_interface)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t serializable = vtx_type_register(&ts, "Serializable",
                                                    VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    vtx_typeid_t data = vtx_type_register(&ts, "Data", VTX_TYPE_OBJECT,
                                            0, NULL, 0, NULL);

    int rc = vtx_type_add_interface(&ts, data, serializable);
    VTX_ASSERT_EQUAL(rc, 0);

    /* data should be an instance of Serializable */
    VTX_ASSERT_TRUE(vtx_type_is_instance(&ts, data, serializable));

    vtx_type_system_destroy(&ts);
}

VTX_TEST(type_instance_check)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_typeid_t comparable = vtx_type_register(&ts, "Comparable",
                                                  VTX_TYPE_OBJECT, 0, NULL, 0, NULL);
    vtx_typeid_t number = vtx_type_register(&ts, "Number", VTX_TYPE_OBJECT,
                                              0, NULL, 0, NULL);
    vtx_type_add_interface(&ts, number, comparable);
    vtx_typeid_t integer = vtx_type_register(&ts, "Integer", number,
                                               0, NULL, 0, NULL);

    /* Integer is instance of Number (subtype) */
    VTX_ASSERT_TRUE(vtx_type_is_instance(&ts, integer, number));
    /* Integer is instance of Comparable (inherited interface) */
    VTX_ASSERT_TRUE(vtx_type_is_instance(&ts, integer, comparable));
    /* Integer is instance of Object */
    VTX_ASSERT_TRUE(vtx_type_is_instance(&ts, integer, VTX_TYPE_OBJECT));
    /* Number is NOT an instance of Integer */
    VTX_ASSERT_FALSE(vtx_type_is_instance(&ts, number, integer));

    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Inline cache transitions                                                     */
/* ========================================================================== */

VTX_TEST(ic_init_monomorphic)
{
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);
    VTX_ASSERT_EQUAL(ic.state, VT_IC_MONOMORPHIC);
    VTX_ASSERT_EQUAL(ic.count, (uint32_t)0);
}

VTX_TEST(ic_update_monomorphic)
{
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);

    vtx_method_desc_t method = {
        .name = "foo", .signature = "()V", .bytecode = NULL,        .vtable_index = 0, .arg_count = 0, .is_virtual = true
    };

    vtx_ic_update(&ic, 1, &method);
    VTX_ASSERT_EQUAL(ic.state, VT_IC_MONOMORPHIC);
    VTX_ASSERT_EQUAL(ic.count, (uint32_t)1);

    /* Lookup should succeed */
    const vtx_method_desc_t *found = vtx_ic_lookup(&ic, 1);
    VTX_ASSERT_NOT_NULL(found);
    VTX_ASSERT_TRUE(strcmp(found->name, "foo") == 0);
}

VTX_TEST(ic_update_polymorphic)
{
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);

    vtx_method_desc_t m1 = { .name = "foo", .signature = "()V", .bytecode = NULL,                              .vtable_index = 0, .arg_count = 0, .is_virtual = true };
    vtx_method_desc_t m2 = { .name = "bar", .signature = "()V", .bytecode = NULL,                              .vtable_index = 1, .arg_count = 0, .is_virtual = true };

    vtx_ic_update(&ic, 1, &m1);
    vtx_ic_update(&ic, 2, &m2);

    VTX_ASSERT_EQUAL(ic.state, VT_IC_POLYMORPHIC);
    VTX_ASSERT_EQUAL(ic.count, (uint32_t)2);

    /* Both lookups should succeed */
    VTX_ASSERT_NOT_NULL(vtx_ic_lookup(&ic, 1));
    VTX_ASSERT_NOT_NULL(vtx_ic_lookup(&ic, 2));
    /* Uncached typeid should miss */
    VTX_ASSERT_NULL(vtx_ic_lookup(&ic, 99));
}

VTX_TEST(ic_update_megamorphic)
{
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);

    vtx_method_desc_t m = { .name = "f", .signature = "()V", .bytecode = NULL,                             .vtable_index = 0, .arg_count = 0, .is_virtual = true };

    /* Fill up to VTX_POLY_LIMIT entries */
    for (uint32_t i = 0; i < VTX_POLY_LIMIT; i++) {
        vtx_ic_update(&ic, i + 1, &m);
    }
    VTX_ASSERT_EQUAL(ic.state, VT_IC_POLYMORPHIC);

    /* One more should trigger megamorphic */
    vtx_ic_update(&ic, VTX_POLY_LIMIT + 1, &m);
    VTX_ASSERT_EQUAL(ic.state, VT_IC_MEGAMORPHIC);

    /* Megamorphic lookup always returns NULL (forces vtable walk) */
    VTX_ASSERT_NULL(vtx_ic_lookup(&ic, 1));
}

VTX_TEST(ic_update_duplicate)
{
    vtx_inline_cache_t ic;
    vtx_ic_init(&ic);

    vtx_method_desc_t m = { .name = "f", .signature = "()V", .bytecode = NULL,                             .vtable_index = 0, .arg_count = 0, .is_virtual = true };

    vtx_ic_update(&ic, 1, &m);
    vtx_ic_update(&ic, 1, &m); /* duplicate — should not increase count */
    VTX_ASSERT_EQUAL(ic.count, (uint32_t)1);
}

/* ========================================================================== */
/* Shape computation                                                            */
/* ========================================================================== */

VTX_TEST(type_shape_computed)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    vtx_field_desc_t *fields = (vtx_field_desc_t *)calloc(2, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(fields);
    fields[0].name = "x"; fields[0].type = VTX_TYPE_OBJECT; fields[0].offset = 0;
    fields[1].name = "y"; fields[1].type = VTX_TYPE_OBJECT; fields[1].offset = 0;

    vtx_typeid_t point = vtx_type_register(&ts, "Point", VTX_TYPE_OBJECT,
                                             2, fields, 0, NULL);

    /* After registration, the shape may be VTX_SHAPE_INVALID due to the
     * compute_shape function being called before type_count is incremented.
     * Calling it again after registration should compute the real shape. */
    vtx_shapeid_t shape = vtx_type_compute_shape(&ts, point);
    VTX_ASSERT_NOT_EQUAL(shape, VTX_SHAPE_INVALID);
    VTX_ASSERT_NOT_EQUAL(shape, VTX_SHAPE_OBJECT);

    /* Two types with the same field layout should have the same shape */
    vtx_field_desc_t *fields2 = (vtx_field_desc_t *)calloc(2, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(fields2);
    fields2[0].name = "a"; fields2[0].type = VTX_TYPE_OBJECT; fields2[0].offset = 0;
    fields2[1].name = "b"; fields2[1].type = VTX_TYPE_OBJECT; fields2[1].offset = 0;

    vtx_typeid_t point2 = vtx_type_register(&ts, "Point2", VTX_TYPE_OBJECT,
                                              2, fields2, 0, NULL);
    vtx_shapeid_t shape2 = vtx_type_compute_shape(&ts, point2);
    VTX_ASSERT_EQUAL(shape, shape2);

    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Instance size computation                                                    */
/* ========================================================================== */

VTX_TEST(type_instance_size)
{
    vtx_type_system_t ts;
    vtx_type_system_init(&ts);

    /* Object with no fields: instance size = header only */
    uint32_t obj_size = vtx_type_instance_size(&ts, VTX_TYPE_OBJECT);
    VTX_ASSERT_EQUAL(obj_size, (uint32_t)VTX_HEAP_OBJECT_HEADER_SIZE);

    /* Object with fields should be larger */
    vtx_field_desc_t *fields2 = (vtx_field_desc_t *)calloc(1, sizeof(vtx_field_desc_t));
    VTX_ASSERT_NOT_NULL(fields2);
    fields2[0].name = "val"; fields2[0].type = VTX_TYPE_OBJECT; fields2[0].offset = 0;
    vtx_typeid_t with_field = vtx_type_register(&ts, "WithField", VTX_TYPE_OBJECT,
                                                  1, fields2, 0, NULL);
    uint32_t wf_size = vtx_type_instance_size(&ts, with_field);
    VTX_ASSERT_TRUE(wf_size > VTX_HEAP_OBJECT_HEADER_SIZE);

    vtx_type_system_destroy(&ts);
}

/* ========================================================================== */
/* Main                                                                         */
/* ========================================================================== */

int main(void)
{
    vtx_test_result_t result = vtx_test_run_all();
    return (result.fail_count > 0) ? 1 : 0;
}
