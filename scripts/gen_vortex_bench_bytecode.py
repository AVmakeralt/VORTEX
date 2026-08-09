#!/usr/bin/env python3
"""
gen_vortex_bench_bytecode.py — Generate VORTEX bytecode arrays for the
bench_v8_comparison.c benchmark with correct PC targets.

Each benchmark's bytecode is described by a sequence of (opcode, *operands)
tuples, with labels for branch targets. The script computes the correct
byte offset for each label and emits a C array literal.

Output: bench_v8_bytecode.h (included by bench_v8_comparison.c)
"""

# Opcode constants (matching enum vtx_opcode_t)
OP_HALT         = 0
OP_NOP          = 1
OP_LOAD_LOCAL   = 2
OP_STORE_LOCAL  = 3
OP_LOAD_FIELD   = 4
OP_STORE_FIELD  = 5
OP_LOAD_CONST_INT = 6
OP_LOAD_CONST_FLOAT = 7
OP_LOAD_CONST_STR = 8
OP_LOAD_NULL    = 9
OP_LOAD_TRUE    = 10
OP_LOAD_FALSE   = 11
OP_LOAD_UNDEFINED = 12
OP_IADD = 13;  OP_ISUB = 14;  OP_IMUL = 15
OP_IDIV = 16;  OP_IMOD = 17
OP_FADD = 18;  OP_FSUB = 19;  OP_FMUL = 20;  OP_FDIV = 21
OP_ISHL = 22;  OP_ISHR = 23
OP_IAND = 24;  OP_IOR  = 25;  OP_IXOR = 26
OP_INEG = 27;  OP_INOT = 28
OP_ICMP_EQ = 29; OP_ICMP_NE = 30
OP_ICMP_LT = 31; OP_ICMP_LE = 32
OP_ICMP_GT = 33; OP_ICMP_GE = 34
OP_GOTO     = 41
OP_IF_TRUE  = 42
OP_IF_FALSE = 43
OP_RETURN_VALUE = 48

# Operand size table
OPND = {  # opcode -> operand size in bytes
    OP_LOAD_LOCAL: 2, OP_STORE_LOCAL: 2, OP_LOAD_FIELD: 2, OP_STORE_FIELD: 2,
    OP_LOAD_CONST_INT: 2, OP_LOAD_CONST_FLOAT: 2, OP_LOAD_CONST_STR: 2,
    OP_GOTO: 2, OP_IF_TRUE: 2, OP_IF_FALSE: 2,
}
OPND_DEFAULT = 0  # all other opcodes have no operand

# Stack effect table (output - input)
STACK_EFFECT = {
    OP_LOAD_LOCAL: +1, OP_STORE_LOCAL: -1,
    OP_LOAD_FIELD: 0, OP_STORE_FIELD: -2,
    OP_LOAD_CONST_INT: +1, OP_LOAD_CONST_FLOAT: +1, OP_LOAD_CONST_STR: +1,
    OP_LOAD_NULL: +1, OP_LOAD_TRUE: +1, OP_LOAD_FALSE: +1, OP_LOAD_UNDEFINED: +1,
    OP_IADD: -1, OP_ISUB: -1, OP_IMUL: -1, OP_IDIV: -1, OP_IMOD: -1,
    OP_FADD: -1, OP_FSUB: -1, OP_FMUL: -1, OP_FDIV: -1,
    OP_ISHL: -1, OP_ISHR: -1,
    OP_IAND: -1, OP_IOR: -1, OP_IXOR: -1,
    OP_INEG: 0, OP_INOT: 0,
    OP_ICMP_EQ: -1, OP_ICMP_NE: -1, OP_ICMP_LT: -1, OP_ICMP_LE: -1,
    OP_ICMP_GT: -1, OP_ICMP_GE: -1,
    OP_GOTO: 0, OP_IF_TRUE: -1, OP_IF_FALSE: -1,
    OP_RETURN_VALUE: -1,
}

def insn_len(op):
    return 1 + OPND.get(op, OPND_DEFAULT)


def verify_stack(insns):
    """Linear pass: track stack depth, return (max_depth, errors).
    Note: branches may have non-linear stack effects; this is a best-effort
    check for straight-line underflow bugs."""
    depth = 0
    max_depth = 0
    errors = []
    pc = 0
    for insn in insns:
        if insn[0] == 'label':
            continue
        op = insn[0]
        eff = STACK_EFFECT.get(op, 0)
        depth += eff
        if depth < 0:
            errors.append(f"stack underflow at PC={pc} (op={op}, depth={depth})")
            depth = 0  # reset to continue checking
        if depth > max_depth:
            max_depth = depth
        pc += insn_len(op)
    return max_depth, errors


def emit_bytecode(name, insns, consts):
    """
    insns: list of either:
        ('label', name)         — define a label at the current PC
        (opcode_int, label_str) — opcode with operand=branch label (1+2 bytes)
        (opcode_int, int_operand) — opcode with numeric operand (1+2 bytes)
        (opcode_int,)            — no-operand opcode (1 byte)
    """
    # First pass: compute label positions
    pc = 0
    labels = {}
    for insn in insns:
        if insn[0] == 'label':
            labels[insn[1]] = pc
        else:
            pc += insn_len(insn[0])

    # Second pass: emit bytes
    bytes_out = []
    for insn in insns:
        if insn[0] == 'label':
            continue
        op = insn[0]
        bytes_out.append(op)
        if op in OPND:
            operand = insn[1]
            if isinstance(operand, str):
                operand = labels[operand]
            bytes_out.append((operand >> 8) & 0xFF)
            bytes_out.append(operand & 0xFF)

    # Format as C array
    arr_name = f"bench_{name}_code"
    lines = []
    lines.append(f"static const uint8_t {arr_name}[] = {{")
    # 12 bytes per line, with comments
    line = "    "
    for i, b in enumerate(bytes_out):
        line += f"0x{b:02X},"
        if (i + 1) % 8 == 0:
            lines.append(line)
            line = "    "
    if line.strip():
        lines.append(line)
    lines.append("};")

    return "\n".join(lines)

def emit_consts(name, consts):
    arr_name = f"bench_{name}_consts"
    items = []
    for c in consts:
        if isinstance(c, str):
            items.append(f'vtx_make_smi((int64_t){c}LL)')
        else:
            items.append(f'vtx_make_smi({c})')
    body = ", ".join(items)
    # vtx_make_smi() is not constexpr, so we need runtime initialization.
    return (f"static vtx_value_t {arr_name}[{len(consts)}];\n"
            f"static void bench_{name}_init_consts(void) {{\n"
            f"    vtx_value_t tmp[] = {{ {body} }};\n"
            f"    memcpy({arr_name}, tmp, sizeof(tmp));\n"
            f"}}")

# Define each benchmark's bytecode. Labels are strings; branches reference labels.

# === fib_iter(30) ===
# locals: 0=n, 1=a, 2=b, 3=tmp, 4=i
# consts: 0=0, 1=1, 2=2
fib_iter = [
    ('label', 'init'),
    (OP_LOAD_CONST_INT, 0), (OP_STORE_LOCAL, 1),    # a=0
    (OP_LOAD_CONST_INT, 1), (OP_STORE_LOCAL, 2),    # b=1
    (OP_LOAD_CONST_INT, 2), (OP_STORE_LOCAL, 4),    # i=2
    ('label', 'loop_top'),
    (OP_LOAD_LOCAL, 4), (OP_LOAD_LOCAL, 0),
    (OP_ICMP_LE,), (OP_IF_TRUE, 'body'),
    (OP_LOAD_LOCAL, 2), (OP_RETURN_VALUE,),
    ('label', 'body'),
    (OP_LOAD_LOCAL, 1), (OP_LOAD_LOCAL, 2),
    (OP_IADD,), (OP_STORE_LOCAL, 3),                # tmp = a+b
    (OP_LOAD_LOCAL, 2), (OP_STORE_LOCAL, 1),        # a = b
    (OP_LOAD_LOCAL, 3), (OP_STORE_LOCAL, 2),        # b = tmp
    (OP_LOAD_LOCAL, 4), (OP_LOAD_CONST_INT, 1),
    (OP_IADD,), (OP_STORE_LOCAL, 4),                # i++
    (OP_GOTO, 'loop_top'),
]
fib_iter_consts = [0, 1, 2]

# === loop_sum(N) ===
# locals: 0=N, 1=sum, 2=i
# consts: 0=0, 1=1
loop_sum = [
    (OP_LOAD_CONST_INT, 0), (OP_STORE_LOCAL, 1),    # sum=0
    (OP_LOAD_CONST_INT, 0), (OP_STORE_LOCAL, 2),    # i=0
    ('label', 'loop_top'),
    (OP_LOAD_LOCAL, 2), (OP_LOAD_LOCAL, 0),
    (OP_ICMP_LT,), (OP_IF_TRUE, 'body'),
    (OP_LOAD_LOCAL, 1), (OP_RETURN_VALUE,),
    ('label', 'body'),
    (OP_LOAD_LOCAL, 1), (OP_LOAD_LOCAL, 2),
    (OP_IADD,), (OP_STORE_LOCAL, 1),                # sum += i
    (OP_LOAD_LOCAL, 2), (OP_LOAD_CONST_INT, 1),
    (OP_IADD,), (OP_STORE_LOCAL, 2),                # i++
    (OP_GOTO, 'loop_top'),
]
loop_sum_consts = [0, 1]

# === tight_loop(N) — empty body ===
# locals: 0=N, 1=i
# consts: 0=0, 1=1
tight_loop = [
    (OP_LOAD_CONST_INT, 0), (OP_STORE_LOCAL, 1),    # i=0
    ('label', 'loop_top'),
    (OP_LOAD_LOCAL, 1), (OP_LOAD_LOCAL, 0),
    (OP_ICMP_LT,), (OP_IF_TRUE, 'body'),
    (OP_LOAD_LOCAL, 1), (OP_RETURN_VALUE,),
    ('label', 'body'),
    (OP_LOAD_LOCAL, 1), (OP_LOAD_CONST_INT, 1),
    (OP_IADD,), (OP_STORE_LOCAL, 1),                # i++
    (OP_GOTO, 'loop_top'),
]
tight_loop_consts = [0, 1]

# === bit_ops(N) — xorshift ===
# locals: 0=N, 1=x, 2=i, 3=t
# consts: 0=seed, 1=1, 2=13, 3=7, 4=17
bit_ops = [
    (OP_LOAD_CONST_INT, 0), (OP_STORE_LOCAL, 1),    # x=seed
    (OP_LOAD_CONST_INT, 1), (OP_STORE_LOCAL, 2),    # i=1
    ('label', 'loop_top'),
    (OP_LOAD_LOCAL, 2), (OP_LOAD_LOCAL, 0),
    (OP_ICMP_LT,), (OP_IF_TRUE, 'body'),
    (OP_LOAD_LOCAL, 1), (OP_RETURN_VALUE,),
    ('label', 'body'),
    # t = x << 13
    (OP_LOAD_LOCAL, 1), (OP_LOAD_CONST_INT, 2),
    (OP_ISHL,), (OP_STORE_LOCAL, 3),
    # x ^= t
    (OP_LOAD_LOCAL, 1), (OP_LOAD_LOCAL, 3),
    (OP_IXOR,), (OP_STORE_LOCAL, 1),
    # t = x >> 7
    (OP_LOAD_LOCAL, 1), (OP_LOAD_CONST_INT, 3),
    (OP_ISHR,), (OP_STORE_LOCAL, 3),
    # x ^= t
    (OP_LOAD_LOCAL, 1), (OP_LOAD_LOCAL, 3),
    (OP_IXOR,), (OP_STORE_LOCAL, 1),
    # t = x << 17
    (OP_LOAD_LOCAL, 1), (OP_LOAD_CONST_INT, 4),
    (OP_ISHL,), (OP_STORE_LOCAL, 3),
    # x ^= t
    (OP_LOAD_LOCAL, 1), (OP_LOAD_LOCAL, 3),
    (OP_IXOR,), (OP_STORE_LOCAL, 1),
    # i++
    (OP_LOAD_LOCAL, 2), (OP_LOAD_CONST_INT, 1),
    (OP_IADD,), (OP_STORE_LOCAL, 2),
    (OP_GOTO, 'loop_top'),
]
bit_ops_consts = ['0x9E3779B97F4A7C15', 1, 13, 7, 17]

# === gcd_loop(N) ===
# locals: 0=N, 1=a, 2=b, 3=i, 4=g
# consts: 0=0, 1=1, 2=2, 3=3
gcd_loop = [
    (OP_LOAD_CONST_INT, 0), (OP_STORE_LOCAL, 4),    # g=0
    (OP_LOAD_CONST_INT, 0), (OP_STORE_LOCAL, 3),    # i=0
    ('label', 'outer_top'),
    (OP_LOAD_LOCAL, 3), (OP_LOAD_LOCAL, 0),
    (OP_ICMP_LT,), (OP_IF_TRUE, 'outer_body'),
    (OP_LOAD_LOCAL, 4), (OP_RETURN_VALUE,),
    ('label', 'outer_body'),
    # a = i*2 + 1
    (OP_LOAD_LOCAL, 3), (OP_LOAD_CONST_INT, 2),
    (OP_IMUL,), (OP_LOAD_CONST_INT, 1),
    (OP_IADD,), (OP_STORE_LOCAL, 1),
    # b = i*3 + 2
    (OP_LOAD_LOCAL, 3), (OP_LOAD_CONST_INT, 3),
    (OP_IMUL,), (OP_LOAD_CONST_INT, 2),
    (OP_IADD,), (OP_STORE_LOCAL, 2),
    ('label', 'inner_top'),
    # if a==0 jump out
    (OP_LOAD_LOCAL, 1), (OP_LOAD_CONST_INT, 0),
    (OP_ICMP_EQ,), (OP_IF_TRUE, 'inner_end'),
    # if b==0 jump out
    (OP_LOAD_LOCAL, 2), (OP_LOAD_CONST_INT, 0),
    (OP_ICMP_EQ,), (OP_IF_TRUE, 'inner_end'),
    # if a>b: a-=b
    (OP_LOAD_LOCAL, 1), (OP_LOAD_LOCAL, 2),
    (OP_ICMP_GT,), (OP_IF_TRUE, 'a_minus_b'),
    # else: b-=a
    (OP_LOAD_LOCAL, 2), (OP_LOAD_LOCAL, 1),
    (OP_ISUB,), (OP_STORE_LOCAL, 2),
    (OP_GOTO, 'inner_top'),
    ('label', 'a_minus_b'),
    (OP_LOAD_LOCAL, 1), (OP_LOAD_LOCAL, 2),
    (OP_ISUB,), (OP_STORE_LOCAL, 1),
    (OP_GOTO, 'inner_top'),
    ('label', 'inner_end'),
    # g += a + b
    (OP_LOAD_LOCAL, 4), (OP_LOAD_LOCAL, 1),
    (OP_IADD,), (OP_LOAD_LOCAL, 2),
    (OP_IADD,), (OP_STORE_LOCAL, 4),
    # i++
    (OP_LOAD_LOCAL, 3), (OP_LOAD_CONST_INT, 1),
    (OP_IADD,), (OP_STORE_LOCAL, 3),
    (OP_GOTO, 'outer_top'),
]
gcd_loop_consts = [0, 1, 2, 3]

# === collatz(N) ===
# locals: 0=N, 1=steps, 2=x
# consts: 0=0, 1=1, 2=2, 3=3
collatz = [
    (OP_LOAD_CONST_INT, 0), (OP_STORE_LOCAL, 1),    # steps=0
    (OP_LOAD_LOCAL, 0), (OP_STORE_LOCAL, 2),        # x=N
    ('label', 'loop_top'),
    # if x<=1 exit
    (OP_LOAD_LOCAL, 2), (OP_LOAD_CONST_INT, 1),
    (OP_ICMP_LE,), (OP_IF_TRUE, 'exit'),
    # if (x&1) odd path
    (OP_LOAD_LOCAL, 2), (OP_LOAD_CONST_INT, 1),
    (OP_IAND,), (OP_IF_TRUE, 'odd'),
    # even: x = x / 2
    (OP_LOAD_LOCAL, 2), (OP_LOAD_CONST_INT, 2),
    (OP_IDIV,), (OP_STORE_LOCAL, 2),
    (OP_GOTO, 'step_inc'),
    ('label', 'odd'),
    # x = 3*x + 1
    (OP_LOAD_LOCAL, 2), (OP_LOAD_CONST_INT, 3),
    (OP_IMUL,), (OP_LOAD_CONST_INT, 1),
    (OP_IADD,), (OP_STORE_LOCAL, 2),
    ('label', 'step_inc'),
    # steps++
    (OP_LOAD_LOCAL, 1), (OP_LOAD_CONST_INT, 1),
    (OP_IADD,), (OP_STORE_LOCAL, 1),
    (OP_GOTO, 'loop_top'),
    ('label', 'exit'),
    (OP_LOAD_LOCAL, 1), (OP_RETURN_VALUE,),
]
collatz_consts = [0, 1, 2, 3]

# === fnv_hash(N) ===
# locals: 0=N, 1=hash, 2=i
# consts: 0=0, 1=1, 2=basis, 3=prime
fnv_hash = [
    (OP_LOAD_CONST_INT, 2), (OP_STORE_LOCAL, 1),    # hash=basis
    (OP_LOAD_CONST_INT, 0), (OP_STORE_LOCAL, 2),    # i=0
    ('label', 'loop_top'),
    (OP_LOAD_LOCAL, 2), (OP_LOAD_LOCAL, 0),
    (OP_ICMP_LT,), (OP_IF_TRUE, 'body'),
    (OP_LOAD_LOCAL, 1), (OP_RETURN_VALUE,),
    ('label', 'body'),
    # hash ^= i
    (OP_LOAD_LOCAL, 1), (OP_LOAD_LOCAL, 2),
    (OP_IXOR,), (OP_STORE_LOCAL, 1),
    # hash *= prime
    (OP_LOAD_LOCAL, 1), (OP_LOAD_CONST_INT, 3),
    (OP_IMUL,), (OP_STORE_LOCAL, 1),
    # i++
    (OP_LOAD_LOCAL, 2), (OP_LOAD_CONST_INT, 1),
    (OP_IADD,), (OP_STORE_LOCAL, 2),
    (OP_GOTO, 'loop_top'),
]
fnv_hash_consts = [0, 1, '0xcbf29ce484222325', '0x100000001b7']

# === arith_chain(N) ===
# locals: 0=N, 1=acc, 2=i
# consts: 0=0, 1=1, 2=7, 3=11, 4=13
arith_chain = [
    (OP_LOAD_CONST_INT, 0), (OP_STORE_LOCAL, 1),    # acc=0
    (OP_LOAD_CONST_INT, 0), (OP_STORE_LOCAL, 2),    # i=0
    ('label', 'loop_top'),
    (OP_LOAD_LOCAL, 2), (OP_LOAD_LOCAL, 0),
    (OP_ICMP_LT,), (OP_IF_TRUE, 'body'),
    (OP_LOAD_LOCAL, 1), (OP_RETURN_VALUE,),
    ('label', 'body'),
    # acc = (acc+7)*11 - 13
    (OP_LOAD_LOCAL, 1), (OP_LOAD_CONST_INT, 2),
    (OP_IADD,), (OP_LOAD_CONST_INT, 3),
    (OP_IMUL,), (OP_LOAD_CONST_INT, 4),
    (OP_ISUB,), (OP_STORE_LOCAL, 1),
    # i++
    (OP_LOAD_LOCAL, 2), (OP_LOAD_CONST_INT, 1),
    (OP_IADD,), (OP_STORE_LOCAL, 2),
    (OP_GOTO, 'loop_top'),
]
arith_chain_consts = [0, 1, 7, 11, 13]

# Emit all benchmarks
benches = [
    ('fib_iter',     fib_iter,     fib_iter_consts,     5, 4),
    ('loop_sum',     loop_sum,     loop_sum_consts,     3, 4),
    ('tight_loop',   tight_loop,   tight_loop_consts,   2, 4),
    ('bit_ops',      bit_ops,      bit_ops_consts,      4, 4),
    ('gcd_loop',     gcd_loop,     gcd_loop_consts,     5, 4),
    ('collatz',      collatz,      collatz_consts,     3, 4),
    ('fnv_hash',     fnv_hash,     fnv_hash_consts,     3, 4),
    ('arith_chain',  arith_chain,  arith_chain_consts,  3, 4),
]

out = []
out.append("/* AUTO-GENERATED by gen_vortex_bench_bytecode.py — DO NOT EDIT BY HAND. */")
out.append("/* All branch targets are computed from labels, so PC mismatches are impossible. */")
out.append("/* Stack depth is verified at generation time to prevent underflow bugs. */")
out.append("")
out.append("#ifndef BENCH_V8_BYTECODE_H")
out.append("#define BENCH_V8_BYTECODE_H")
out.append("")
for name, insns, consts, max_locals, max_stack_decl in benches:
    actual_max_stack, errs = verify_stack(insns)
    if errs:
        print(f"[{name}] STACK ERRORS:")
        for e in errs:
            print(f"  {e}")
    declared = max(max_stack_decl, actual_max_stack)
    out.append(f"/* ---- {name} ---- */")
    out.append(f"/* max_locals={max_locals}, declared max_stack={max_stack_decl}, "
               f"computed max_stack={actual_max_stack}, using={declared}, consts={len(consts)} */")
    out.append(emit_consts(name, consts))
    out.append(emit_bytecode(name, insns, consts))
    out.append(f"#define BENCH_{name.upper()}_CODE      bench_{name}_code")
    out.append(f"#define BENCH_{name.upper()}_CODE_LEN  sizeof(bench_{name}_code)")
    out.append(f"#define BENCH_{name.upper()}_CONSTS    bench_{name}_consts")
    out.append(f"#define BENCH_{name.upper()}_NCONSTS   {len(consts)}")
    out.append(f"#define BENCH_{name.upper()}_LOCALS    {max_locals}")
    out.append(f"#define BENCH_{name.upper()}_STACK     {declared}")
    out.append("")

out.append("#endif /* BENCH_V8_BYTECODE_H */")
out.append("")

import sys
if any(verify_stack(b[1])[1] for b in benches):
    print("ERROR: stack underflow detected — fix the bytecode and re-run.")
    sys.exit(1)

with open('/home/z/my-project/benchmarks/bench_v8_bytecode.h', 'w') as f:
    f.write("\n".join(out))
print("Wrote /home/z/my-project/benchmarks/bench_v8_bytecode.h")
