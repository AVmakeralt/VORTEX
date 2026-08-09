/**
 * bench_v8_comparison.js — V8 (Node.js) companion benchmark.
 *
 * Run with:
 *   node benchmarks/v8_js/bench_v8_comparison.js
 *
 * Same workloads as bench_v8_comparison.c. The C program prints a
 * results table — this JS file prints its own table that you can copy
 * into the V8 (ms) column of the C report.
 *
 * Methodology mirrors the C benchmark:
 *   1. Varying inputs (+/-4) to prevent constant folding
 *   2. Results consumed (accumulated into a sink) to prevent DCE
 *   3. 32 samples with median reporting
 *   4. Warmup phase before measurement
 *
 * V8 will JIT-compile all of these via TurboFan after warmup.
 */

'use strict';

const N_SAMPLES = 32;

/* ---- timing ---- */
function now_ns() {
    const t = process.hrtime.bigint();
    return t;
}
function ns_diff_ms(start, end) {
    return Number(end - start) / 1e6;
}

/* ---- sample stats (median) ---- */
function median(samples) {
    const sorted = [...samples].sort((a, b) => Number(a - b) > 0 ? 1 : -1);
    return sorted[Math.floor(sorted.length / 2)];
}

/* ---- C reference implementations (mirrors bench_v8_comparison.c) ---- */

function fib_iter(n) {
    if (n <= 1) return n;
    let a = 0, b = 1;
    for (let i = 2; i <= n; i++) {
        const t = a + b; a = b; b = t;
    }
    return b;
}

function loop_sum(n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += i;
    return s;
}

function tight_loop(n) {
    let i = 0;
    for (; i < n; i++) {}
    return i;
}

function bit_ops(n) {
    let x = 0x9E3779B97F4A7C15n | 0n;  /* BigInt for 64-bit; V8 will still JIT it. */
    for (let i = 0n; i < n; i++) {
        x ^= (x << 13n);
        x ^= (x >> 7n);
        x ^= (x << 17n);
    }
    return x;
}

/* Use Number version (more representative of typical JS code; V8 has dedicated
 * fast paths for 32-bit int ops on Number). */
function bit_ops_num(n) {
    let x = -4738385681997132715;  /* closest double approximation */
    for (let i = 0; i < n; i++) {
        x ^= (x << 13);
        x ^= (x >> 7);
        x ^= (x << 17);
    }
    return x;
}

function gcd_loop(n) {
    let g = 0;
    for (let i = 0; i < n; i++) {
        let a = i * 2 + 1, b = i * 3 + 2;
        while (a !== 0 && b !== 0) {
            if (a > b) a -= b; else b -= a;
        }
        g += a + b;
    }
    return g;
}

function collatz(n) {
    let steps = 0, x = n;
    while (x > 1) {
        if (x & 1) x = 3 * x + 1; else x = x / 2;
        steps++;
    }
    return steps;
}

function fnv_hash(n) {
    let h = -3750763034362895579;  /* 0xcbf29ce484222325 as int64 */
    for (let i = 0; i < n; i++) {
        h ^= i;
        /* multiply by prime: emulate 64-bit truncation via Math */
        h = h * 1099511628211;  /* 0x100000001b7 — within double precision */
    }
    return h;
}

function arith_chain(n) {
    let acc = 0;
    for (let i = 0; i < n; i++) {
        acc = (acc + 7) * 11 - 13;
    }
    return acc;
}

/* ---- generic benchmark runner ---- */
function bench(fn, n, iters, warmup) {
    let sink = 0;
    for (let i = 0; i < warmup; i++) sink += fn(n + (i % 5));
    const samples = [];
    for (let i = 0; i < iters; i++) {
        const nn = n + (i % 5);
        const t0 = now_ns();
        sink += fn(nn);
        const t1 = now_ns();
        samples.push(t1 - t0);
    }
    if (sink === 0xDEAD_BEEF) console.log('unreachable');  /* consume sink */
    return median(samples);
}

/* ---- main ---- */
function main() {
    console.log('==============================================================');
    console.log('  V8 (Node.js) — Honest Benchmark Suite');
    console.log('  All times: median over ' + N_SAMPLES + ' samples (varying input +/-4)');
    console.log('  Node version: ' + process.version);
    console.log('==============================================================\n');

    const rows = [
        { name: 'fib_iter(30)',     N: 30,       fn: fib_iter      },
        { name: 'loop_sum(10K)',    N: 10000,    fn: loop_sum      },
        { name: 'tight_loop(100K)', N: 100000,   fn: tight_loop    },
        { name: 'bit_ops(100K)',   N: 100000,   fn: bit_ops_num   },
        { name: 'gcd_loop(1K)',     N: 1000,     fn: gcd_loop      },
        { name: 'collatz(1000)',   N: 1000,     fn: collatz       },
        { name: 'fnv_hash(100K)',   N: 100000,   fn: fnv_hash      },
        { name: 'arith_chain(100K)', N: 100000, fn: arith_chain   },
    ];

    console.log('  Benchmark               V8 median (ms)');
    console.log('  ----------               ---------------');
    const results = [];
    for (const r of rows) {
        const ns = bench(r.fn, r.N, N_SAMPLES, 50);
        const ms = Number(ns) / 1e6;
        results.push({ name: r.name, ms });
        console.log('  ' + r.name.padEnd(22) + '  ' + ms.toFixed(4).padStart(12));
    }

    /* JSON dump so the comparison script can pick it up */
    console.log('\n--- JSON for comparison script ---');
    console.log(JSON.stringify(results));
}

main();
