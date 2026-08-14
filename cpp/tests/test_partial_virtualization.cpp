// test_partial_virtualization.cpp — Unit tests for the partial
// virtualization pass.
//
// Tests cover:
//   1. Object with one constant field → LoadField replaced
//   2. Object with one runtime field → NOT replaced
//   3. Object that escapes → NOT replaced
//   4. Object with two constant fields → both replaced
//   5. Object with conflicting writes → NOT replaced (conservative)
//   6. No objects → no-op

#define typeid typeid_
extern "C" {
#include "runtime/bytecode.h"
#include "runtime/object.h"
#include "ir/graph.h"
#include "ir/node.h"
}
#undef typeid

#include "vortex/partial_virtualization.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace vortex;

static vtx_graph_t* make_graph(uint32_t param_count) {
    vtx_graph_t* g = (vtx_graph_t*)calloc(1, sizeof(vtx_graph_t));
    vtx_graph_init(g, param_count > 0 ? param_count : 1);
    return g;
}

static vtx_nodeid_t make_const(vtx_graph_t* g, int64_t val) {
    vtx_nodeid_t id = vtx_node_create(&g->node_table, VTX_OP_Constant);
    vtx_node_t* n = vtx_node_get(&g->node_table, id);
    n->constval.kind = VTX_TYPE_Int;
    n->constval.as.int_val = val;
    n->type = VTX_TYPE_Int;
    return id;
}

static vtx_nodeid_t make_alloc(vtx_graph_t* g) {
    vtx_nodeid_t id = vtx_node_create(&g->node_table, VTX_OP_NewObject);
    vtx_node_t* n = vtx_node_get(&g->node_table, id);
    n->type = VTX_TYPE_Ptr;
    return id;
}

static vtx_nodeid_t make_store_field(vtx_graph_t* g, vtx_nodeid_t obj, uint32_t field_off, vtx_nodeid_t val) {
    vtx_nodeid_t id = vtx_node_create(&g->node_table, VTX_OP_StoreField);
    vtx_node_t* n = vtx_node_get(&g->node_table, id);
    n->local_index = field_off;
    n->type = VTX_TYPE_Void;
    vtx_node_add_input(&g->node_table, id, obj);
    vtx_node_add_input(&g->node_table, id, val);
    return id;
}

static vtx_nodeid_t make_load_field(vtx_graph_t* g, vtx_nodeid_t obj, uint32_t field_off) {
    vtx_nodeid_t id = vtx_node_create(&g->node_table, VTX_OP_LoadField);
    vtx_node_t* n = vtx_node_get(&g->node_table, id);
    n->local_index = field_off;
    n->type = VTX_TYPE_Int;
    vtx_node_add_input(&g->node_table, id, obj);
    return id;
}

static void test_one_constant_field() {
    vtx_graph_t* g = make_graph(0);
    vtx_nodeid_t alloc = make_alloc(g);
    vtx_nodeid_t c42 = make_const(g, 42);
    vtx_nodeid_t store = make_store_field(g, alloc, 0, c42);
    vtx_nodeid_t load = make_load_field(g, alloc, 0); (void)load;
    (void)store;

    auto r = partial_virtualize(g); (void)r;
    assert(r.objects_analyzed == 1);
    assert(r.fields_virtualized == 1);
    // The LoadField should be dead, and its uses redirected to the constant
    vtx_node_t* load_node = vtx_node_get(&g->node_table, load); (void)load_node;
    assert(load_node->dead);
    printf("  [PASS] test_one_constant_field\n");
    free(g->node_table.nodes);
    free(g);
}

static void test_runtime_field_not_replaced() {
    vtx_graph_t* g = make_graph(1);
    vtx_nodeid_t alloc = make_alloc(g);
    // Store a Parameter (runtime value) into field 0
    vtx_nodeid_t param = vtx_node_create(&g->node_table, VTX_OP_Parameter);
    vtx_node_get(&g->node_table, param)->type = VTX_TYPE_Int;
    vtx_nodeid_t store = make_store_field(g, alloc, 0, param);
    vtx_nodeid_t load = make_load_field(g, alloc, 0); (void)load;
    (void)store;

    auto r = partial_virtualize(g); (void)r;
    assert(r.objects_analyzed == 1);
    assert(r.fields_virtualized == 0);  // NOT replaced — runtime value
    vtx_node_t* load_node = vtx_node_get(&g->node_table, load); (void)load_node;
    assert(!load_node->dead);
    printf("  [PASS] test_runtime_field_not_replaced\n");
    free(g->node_table.nodes);
    free(g);
}

static void test_escaping_object_not_replaced() {
    vtx_graph_t* g = make_graph(1);
    vtx_nodeid_t alloc = make_alloc(g);
    vtx_nodeid_t c42 = make_const(g, 42);
    vtx_nodeid_t store = make_store_field(g, alloc, 0, c42);
    // Make the object "escape" by using it as a Return input
    vtx_nodeid_t ret = vtx_node_create(&g->node_table, VTX_OP_Return);
    vtx_node_add_input(&g->node_table, ret, alloc);
    vtx_nodeid_t load = make_load_field(g, alloc, 0); (void)load;
    (void)store; (void)ret;

    auto r = partial_virtualize(g); (void)r;
    assert(r.fields_virtualized == 0);  // NOT replaced — object escapes
    vtx_node_t* load_node = vtx_node_get(&g->node_table, load); (void)load_node;
    assert(!load_node->dead);
    printf("  [PASS] test_escaping_object_not_replaced\n");
    free(g->node_table.nodes);
    free(g);
}

static void test_two_constant_fields() {
    vtx_graph_t* g = make_graph(0);
    vtx_nodeid_t alloc = make_alloc(g);
    vtx_nodeid_t c1 = make_const(g, 10);
    vtx_nodeid_t c2 = make_const(g, 20);
    make_store_field(g, alloc, 0, c1);
    make_store_field(g, alloc, 1, c2);
    vtx_nodeid_t load0 = make_load_field(g, alloc, 0); (void)load0;
    vtx_nodeid_t load1 = make_load_field(g, alloc, 1); (void)load1;

    auto r = partial_virtualize(g); (void)r;
    assert(r.fields_virtualized == 2);
    assert(vtx_node_get(&g->node_table, load0)->dead);
    assert(vtx_node_get(&g->node_table, load1)->dead);
    printf("  [PASS] test_two_constant_fields\n");
    free(g->node_table.nodes);
    free(g);
}

static void test_conflicting_writes_not_replaced() {
    vtx_graph_t* g = make_graph(0);
    vtx_nodeid_t alloc = make_alloc(g);
    vtx_nodeid_t c1 = make_const(g, 10);
    vtx_nodeid_t c2 = make_const(g, 20);
    make_store_field(g, alloc, 0, c1);
    make_store_field(g, alloc, 0, c2);  // conflicting!
    vtx_nodeid_t load = make_load_field(g, alloc, 0); (void)load;

    auto r = partial_virtualize(g); (void)r;
    assert(r.fields_virtualized == 0);  // NOT replaced — conflicting writes
    assert(!vtx_node_get(&g->node_table, load)->dead);
    printf("  [PASS] test_conflicting_writes_not_replaced\n");
    free(g->node_table.nodes);
    free(g);
}

static void test_no_objects_noop() {
    vtx_graph_t* g = make_graph(0);
    vtx_nodeid_t c1 = make_const(g, 10);
    (void)c1;
    auto r = partial_virtualize(g); (void)r;
    assert(r.objects_analyzed == 0);
    assert(r.fields_virtualized == 0);
    printf("  [PASS] test_no_objects_noop\n");
    free(g->node_table.nodes);
    free(g);
}

int main() {
    printf("=== Partial virtualization tests ===\n");
    test_one_constant_field();
    test_runtime_field_not_replaced();
    test_escaping_object_not_replaced();
    test_two_constant_fields();
    test_conflicting_writes_not_replaced();
    test_no_objects_noop();
    printf("=== All tests passed ===\n");
    return 0;
}
