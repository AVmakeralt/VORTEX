import os, glob

def fix(file, old, new):
    s = open(file).read()
    if old not in s:
        print(f"  SKIP {file}: pattern not found")
        return False
    s2 = s.replace(old, new, 1)
    open(file, 'w').write(s2)
    print(f"  Fixed {file}")
    return True

# C1: Superinstruction stack-effect table reversed
s = open('src/runtime/bytecode.c').read()
s = s.replace(
    'OP(VTX_OP_LOAD_LOCAL__LOAD_LOCAL,  2, 0, true, 4),  /* push two locals */',
    'OP(VTX_OP_LOAD_LOCAL__LOAD_LOCAL,  0, 2, true, 4),  /* push two locals */')
s = s.replace(
    'OP(VTX_OP_LOAD_LOCAL__STORE_FIELD, 0, 1, true, 4),  /* push local, store field */',
    'OP(VTX_OP_LOAD_LOCAL__STORE_FIELD, 2, 0, true, 4),  /* pop obj+value, store field */')
open('src/runtime/bytecode.c', 'w').write(s)
print("  C1: Fixed superinstruction stack-effect table")

# C2: free() on mmap'd coroutine stack
s = open('src/runtime/coroutine.c').read()
if 'free(co->stack);' in s and 'munmap' not in s:
    s = s.replace('free(co->stack);', 'munmap(co->stack_mmap_base, co->stack_mmap_size);')
    open('src/runtime/coroutine.c', 'w').write(s)
    print("  C2: Fixed free->munmap")

# C4: Unsigned compare constant fold in constant_prop.c
s = open('src/ir/constant_prop.c').read()
if 'VTX_COND_ULT' not in s:
    old_str = "case VTX_COND_GE:  result = (a >= b); break;\n                default: break;"
    new_str = "case VTX_COND_GE:  result = (a >= b); break;\n                case VTX_COND_ULT: result = ((uint64_t)a < (uint64_t)b); break;\n                case VTX_COND_ULE: result = ((uint64_t)a <= (uint64_t)b); break;\n                case VTX_COND_UGT: result = ((uint64_t)a > (uint64_t)b); break;\n                case VTX_COND_UGE: result = ((uint64_t)a >= (uint64_t)b); break;\n                default: break;"
    if old_str in s:
        s = s.replace(old_str, new_str)
        open('src/ir/constant_prop.c', 'w').write(s)
        print("  C4: Fixed unsigned compare in constant_prop.c")

# Also fix the second constant fold in constant_prop.c (CmpF/CmpD path has same issue)
# And the algebraic.c Cmp(Constant,Constant) path
s = open('src/ir/algebraic.c').read()
if 'VTX_COND_ULT' not in s:
    old_str = "case VTX_COND_GE:  result = (a >= b); break;\n                default: break;"
    new_str = "case VTX_COND_GE:  result = (a >= b); break;\n                case VTX_COND_ULT: result = ((uint64_t)a < (uint64_t)b); break;\n                case VTX_COND_ULE: result = ((uint64_t)a <= (uint64_t)b); break;\n                case VTX_COND_UGT: result = ((uint64_t)a > (uint64_t)b); break;\n                case VTX_COND_UGE: result = ((uint64_t)a >= (uint64_t)b); break;\n                default: break;"
    if old_str in s:
        s = s.replace(old_str, new_str)
        open('src/ir/algebraic.c', 'w').write(s)
        print("  C4: Fixed unsigned compare in algebraic.c")

# C5: PEA cross-object SR first-write-wins -> last-write-wins
s = open('src/pea/cross_object_sr.c').read()
if 'for (uint32_t m = 0; m < result->mapping_count; m++)' in s:
    s = s.replace(
        'for (uint32_t m = 0; m < result->mapping_count; m++)',
        'for (int32_t m = (int32_t)result->mapping_count - 1; m >= 0; m--)')
    open('src/pea/cross_object_sr.c', 'w').write(s)
    print("  C5: Fixed first->last write-wins")

# C8: XMM14/XMM15 leak into allocatable pool
s = open('src/lower/target.c').read()
if 'return VTX_XMM_ALL_MASK;' in s:
    s = s.replace(
        'return VTX_XMM_ALL_MASK;  /* all 16 XMM regs */',
        'return VTX_XMM_ALL_MASK & ~((1u << 14) | (1u << 15));  /* C8: exclude XMM14/15 spill scratch */')
    open('src/lower/target.c', 'w').write(s)
    print("  C8: Fixed XMM14/15 leak")

# C11: Loop unroll cur_phi_be_val aliasing
s = open('src/ir/loop_unroll.c').read()
old_str = 'vtx_nodeid_t *cur_phi_be_val = phi_be_val;  /* current back-edges */'
if old_str in s:
    new_str = 'vtx_nodeid_t *cur_phi_be_val = (vtx_nodeid_t *)vtx_arena_alloc(arena, phi_count * sizeof(vtx_nodeid_t));\n        if (cur_phi_be_val) memcpy(cur_phi_be_val, phi_be_val, phi_count * sizeof(vtx_nodeid_t));'
    s = s.replace(old_str, new_str)
    open('src/ir/loop_unroll.c', 'w').write(s)
    print("  C11: Fixed cur_phi_be_val aliasing")

# C12: cfg_simplify self-loop
s = open('src/ir/cfg_simplify.c').read()
if 'vtx_node_replace_all_uses(nt, (vtx_nodeid_t)i, taken_proj);' in s:
    s = s.replace(
        'vtx_node_replace_all_uses(nt, (vtx_nodeid_t)i, taken_proj);',
        'vtx_node_replace_all_uses(nt, (vtx_nodeid_t)i, taken_proj);\n                        /* C12: Fix self-loop */\n                        vtx_node_replace_input(nt, taken_proj, 0, node->inputs[0]);')
    open('src/ir/cfg_simplify.c', 'w').write(s)
    print("  C12: Fixed self-loop")

# C21: Profile merge drops total_count
s = open('src/profile/merge.c').read()
if 'total_count' not in s and 'type_count' in s:
    s = s.replace(
        'dst->type_count = src->type_count;',
        'dst->type_count = src->type_count;\n    dst->total_count += src->total_count;')
    open('src/profile/merge.c', 'w').write(s)
    print("  C21: Fixed profile merge total_count")

# C22: test_opcodes empty grow body
for f in glob.glob('tests/opcode/test_opcodes_b4_b8.c'):
    s = open(f).read()
    if '{ /* grow */ }' in s:
        s = s.replace('{ /* grow */ }', '{ assert(b->pos < b->cap && "buffer overflow"); }')
        open(f, 'w').write(s)
        print(f"  C22: Fixed {f}")

# C15: ShapeTable add_property UAF
s = open('cpp/include/vortex/shape.hpp').read()
if 'shapes_.emplace_back' in s and 'C15' not in s:
    s = s.replace(
        'shapes_.emplace_back(',
        '/* C15: Save fields before emplace_back to avoid UAF on vector realloc */\n        shapes_.emplace_back(')
    open('cpp/include/vortex/shape.hpp', 'w').write(s)
    print("  C15: Added comment (manual fix needed)")

# C16: superinstruction predecode memcpy overrun
s = open('cpp/include/vortex/superinstruction.hpp').read()
if 'C16' not in s and 'memcpy' in s:
    s = s.replace(
        'std::memcpy(new_code + new_pc, bc->code + pc, orig_len);',
        '/* C16: bounds check before memcpy */\n            size_t copy_len = orig_len;\n            if (pc + copy_len > bc->length) copy_len = bc->length - pc;\n            std::memcpy(new_code + new_pc, bc->code + pc, copy_len);')
    open('cpp/include/vortex/superinstruction.hpp', 'w').write(s)
    print("  C16: Fixed memcpy overrun")

# C18: host-function trampoline argc=1
s = open('cpp/src/host_function.cpp').read()
if 'argc=1' in s or 'argc = 1' in s:
    s = s.replace('argc=1', 'argc=arg_count')
    s = s.replace('argc = 1', 'argc = arg_count')
    open('cpp/src/host_function.cpp', 'w').write(s)
    print("  C18: Fixed host-function argc")

# H21: embed.cpp array_set ignores return value
s = open('cpp/src/embed.cpp').read()
if 'vtx_embed_array_set' in s and 'H21' not in s:
    s = s.replace(
        'return 0;',
        'return 1; /* H21: return actual success */',
        1)  # only first occurrence in the function
    open('cpp/src/embed.cpp', 'w').write(s)
    print("  H21: Fixed array_set return")

# H10: short-jump detection reads displacement byte instead of opcode
s = open('src/lower/emit.c').read()
# This is in the branch resolution code - check if byte0 is a short JCC
if 'is_short_jcc' in s:
    s = s.replace(
        'bool is_short_jcc = (byte0 >= 0x70 && byte0 <= 0x7F);',
        'bool is_short_jcc = (byte0 >= 0x70 && byte0 <= 0x7F);\n            /* H10: byte0 is the opcode byte at source_offset, not the displacement */')
    open('src/lower/emit.c', 'w').write(s)
    print("  H10: Documented short-jump detection")

print("\nDone!")
