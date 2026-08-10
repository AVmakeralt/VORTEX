/**
 * bench_jit_vs_jit.js — V8 companion for fair JIT-vs-JIT comparison.
 *
 * Uses the SAME LCG seed (getpid equivalent) and the SAME workloads
 * as bench_jit_vs_jit.c. V8's TurboFan will JIT-compile these but
 * cannot constant-fold because inputs vary per iteration.
 *
 * The functions are created via eval() to prevent V8 from inlining
 * them across call boundaries.
 */

'use strict';

const N_SAMPLES = 20;

function now_ns() { return process.hrtime.bigint(); }

function median(samples) {
    const sorted = [...samples].sort((a, b) => Number(a - b) > 0 ? 1 : -1);
    return Number(sorted[Math.floor(sorted.length / 2)]);
}

// LCG matching the C implementation
let lcg_state;
function lcg_seed(s) { lcg_state = BigInt(s); }
function lcg_next() {
    lcg_state = (lcg_state * 6364136223846793005n + 1442695040888963407n) & 0xFFFFFFFFFFFFFFFFn;
    return lcg_state;
}

// Functions via eval to prevent cross-boundary inlining
const sum_fn = eval('(function(n){let s=0;for(let i=n;i>0;i--)s+=i;return s;})');
const gcd_fn = eval('(function(a,b){while(b!==0){const t=a%b;a=b;b=t;}return a;})');
const collatz_fn = eval('(function(n){let s=0;while(n!==1){if(n%2===0)n=n/2;else n=3*n+1;s++;}return s;})');

function bench1(fn, baseN, iters, seed) {
    const samples = [];
    let sink = 0;
    // Warmup — let TurboFan JIT-compile
    for (let i = 0; i < 50000; i++) sink += fn(baseN + (i & 15));
    if (sink === 0xDEAD) console.log('unreachable');

    for (let s = 0; s < N_SAMPLES; s++) {
        lcg_seed(BigInt(seed) + BigInt(s * 31));
        const t0 = now_ns();
        let acc = 0;
        for (let i = 0; i < iters; i++) {
            const n = baseN + Number(lcg_next() & 15n);
            acc += fn(n);
        }
        const t1 = now_ns();
        sink += acc;
        samples.push(t1 - t0);
    }
    if (sink === 0xDEAD) console.log('unreachable');
    return median(samples) / iters;  // ns per call
}

function bench2(fn, a, b, iters, seed) {
    const samples = [];
    let sink = 0;
    for (let i = 0; i < 50000; i++) sink += fn(a + (i & 15), b + (i & 7));
    if (sink === 0xDEAD) console.log('unreachable');

    for (let s = 0; s < N_SAMPLES; s++) {
        lcg_seed(BigInt(seed) + BigInt(s * 31));
        const t0 = now_ns();
        let acc = 0;
        for (let i = 0; i < iters; i++) {
            acc += fn(a + Number(lcg_next() & 15n), b + Number(lcg_next() & 7n));
        }
        const t1 = now_ns();
        sink += acc;
        samples.push(t1 - t0);
    }
    if (sink === 0xDEAD) console.log('unreachable');
    return median(samples) / iters;
}

function main() {
    const seed = process.pid * 1000000007;

    console.log('================================================================');
    console.log('  V8 (Node.js) — JIT vs JIT companion');
    console.log('  Same LCG seed as C benchmark (pid-based)');
    console.log('  ' + N_SAMPLES + ' samples, median reported');
    console.log('================================================================\n');

    const r_sum = bench1(sum_fn, 10000, 2000, seed);
    const r_gcd = bench2(gcd_fn, 1234567890, 123456789, 2000, seed);
    const r_col = bench1(collatz_fn, 97, 2000, seed);

    console.log('  Benchmark          V8 (ns/call)');
    console.log('  ----------         -----------');
    console.log('  sum(10000)        ' + r_sum.toFixed(0).padStart(10));
    console.log('  gcd(big)         ' + r_gcd.toFixed(0).padStart(10));
    console.log('  collatz(97)       ' + r_col.toFixed(0).padStart(10));

    console.log('\n--- JSON ---');
    console.log(JSON.stringify([
        {name: 'sum(10000)', ns: r_sum},
        {name: 'gcd(big)', ns: r_gcd},
        {name: 'collatz(97)', ns: r_col},
    ]));
}

main();
