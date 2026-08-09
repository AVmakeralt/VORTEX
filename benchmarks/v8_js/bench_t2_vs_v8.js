/**
 * bench_t2_vs_v8.js — V8 companion for bench_t2_vs_v8.c
 * Same inputs: sum(100), fib(20), gcd(1234567890,123456789), collatz(27)
 */

'use strict';

const N_SAMPLES = 20;

function now_ns() { return process.hrtime.bigint(); }

function median(samples) {
    const sorted = [...samples].sort((a, b) => Number(a - b) > 0 ? 1 : -1);
    return Number(sorted[Math.floor(sorted.length / 2)]) / 1e6;
}

function fib_iter(n) {
    if (n <= 1) return n;
    let a = 0, b = 1;
    for (let i = 2; i <= n; i++) {
        const t = a + b; a = b; b = t;
    }
    return b;
}

function sum_loop(n) {
    let s = 0;
    for (let i = n; i > 0; i--) s += i;
    return s;
}

function gcd(a, b) {
    while (b !== 0) { const t = a % b; a = b; b = t; }
    return a;
}

function collatz(n) {
    let steps = 0;
    while (n !== 1) {
        if (n % 2 === 0) n = n / 2;
        else n = 3 * n + 1;
        steps++;
    }
    return steps;
}

function bench(fn, ...args) {
    const samples = [];
    for (let s = 0; s < N_SAMPLES; s++) {
        const t0 = now_ns();
        let acc = 0;
        for (let i = 0; i < 2000; i++) {
            acc += fn(...args);
        }
        const t1 = now_ns();
        samples.push(t1 - t0);
        if (acc === 0xDEAD) console.log('unreachable');
    }
    return median(samples) / 2000 * 1000;  // ns per call
}

function main() {
    console.log('================================================================');
    console.log('  V8 (Node.js) — T2 JIT companion benchmark');
    console.log('  Same inputs as bench_t2_vs_v8.c (large N, eval to prevent folding)');
    console.log('  ' + N_SAMPLES + ' samples, median reported');
    console.log('================================================================\n');

    // Use eval() to create functions that V8 cannot inline or
    // constant-fold across call boundaries. TurboFan will JIT-compile
    // the functions but cannot eliminate them entirely.
    const sum_loop = eval('(function(n){let s=0;for(let i=n;i>0;i--)s+=i;return s;})');
    const fib_iter = eval('(function(n){if(n<=1)return n;let a=0,b=1;for(let i=2;i<=n;i++){const t=a+b;a=b;b=t;}return b;})');
    const gcd = eval('(function(a,b){while(b!==0){const t=a%b;a=b;b=t;}return a;})');
    const collatz = eval('(function(n){let s=0;while(n!==1){if(n%2===0)n=n/2;else n=3*n+1;s++;}return s;})');

    // LCG for unpredictable inputs
    let lcg_state = 12345;
    function lcg_next() {
        lcg_state = (lcg_state * 6364136223846793005 + 1442695040888963407) | 0;
        return lcg_state;
    }

    function bench_lcg(fn, baseN, iters) {
        const samples = [];
        for (let s = 0; s < N_SAMPLES; s++) {
            lcg_state = 12345 + s;
            const t0 = now_ns();
            let acc = 0;
            for (let i = 0; i < iters; i++) {
                const n = baseN + (lcg_next() & 15);
                acc += fn(n);
            }
            const t1 = now_ns();
            samples.push(t1 - t0);
            if (acc === 0xDEAD) console.log('unreachable');
        }
        return median(samples) / iters * 1000;
    }

    function bench_lcg2(fn, a, b, iters) {
        const samples = [];
        for (let s = 0; s < N_SAMPLES; s++) {
            lcg_state = 12345 + s;
            const t0 = now_ns();
            let acc = 0;
            for (let i = 0; i < iters; i++) {
                acc += fn(a + (lcg_next() & 15), b + (lcg_next() & 7));
            }
            const t1 = now_ns();
            samples.push(t1 - t0);
            if (acc === 0xDEAD) console.log('unreachable');
        }
        return median(samples) / iters * 1000;
    }

    // Warmup
    for (let i = 0; i < 50000; i++) {
        sum_loop(10000 + (i & 15)); fib_iter(30 + (i & 15));
        gcd(1234567890 + (i & 15), 123456789 + (i & 7));
        collatz(97 + (i & 15));
    }

    const r_sum = bench_lcg(sum_loop, 10000, 2000);
    const r_fib = bench_lcg(fib_iter, 30, 2000);
    const r_gcd = bench_lcg2(gcd, 1234567890, 123456789, 2000);
    const r_col = bench_lcg(collatz, 97, 2000);

    console.log('  Benchmark              V8 (ns)');
    console.log('  ----------             -------');
    console.log('  sum(10000)           ' + r_sum.toFixed(1).padStart(10));
    console.log('  fib(30)              ' + r_fib.toFixed(1).padStart(10));
    console.log('  gcd(big)            ' + r_gcd.toFixed(1).padStart(10));
    console.log('  collatz(97)          ' + r_col.toFixed(1).padStart(10));

    console.log('\n--- JSON ---');
    console.log(JSON.stringify([
        {name: 'sum(10000)', ns: r_sum},
        {name: 'fib(30)', ns: r_fib},
        {name: 'gcd(big)', ns: r_gcd},
        {name: 'collatz(97)', ns: r_col},
    ]));
}

main();
