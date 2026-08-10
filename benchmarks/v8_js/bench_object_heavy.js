/**
 * bench_object_heavy.js — V8 companion for bench_object_heavy.c
 *
 * Same workload: config object with constant fields + shape objects
 * in a loop, computing transformed area.
 *
 * Run: node benchmarks/v8_js/bench_object_heavy.js
 */

'use strict';

const N_SAMPLES = 20;

function now_ns() { return process.hrtime.bigint(); }

function median(samples) {
    const sorted = [...samples].sort((a, b) => Number(a - b) > 0 ? 1 : -1);
    return Number(sorted[Math.floor(sorted.length / 2)]);
}

// The same computation as the C benchmark
function objectHeavy(n) {
    // Config with constant fields (V8 will JIT-specialize these)
    const config = { width: 800, height: 600, scale: 2 };
    const area = (config.width * config.scale) * (config.height * config.scale);
    let sum = 0;

    for (let i = 0; i < n; i++) {
        // Shape with runtime fields
        const shape = { x: i, y: i * 2 };
        const tmp = shape.x * area + shape.y;
        sum += tmp;
    }
    return sum;
}

function bench(n, iters) {
    const samples = [];
    let sink = 0;
    // Warmup (let TurboFan JIT-compile)
    for (let i = 0; i < 1000; i++) sink += objectHeavy(n);

    for (let s = 0; s < N_SAMPLES; s++) {
        const t0 = now_ns();
        let acc = 0;
        for (let i = 0; i < iters; i++) {
            acc += objectHeavy(n);
        }
        const t1 = now_ns();
        sink += acc;
        samples.push(t1 - t0);
    }
    if (sink === 0xDEAD) console.log('unreachable');
    return median(samples) / iters;  // ns per call
}

function main() {
    console.log('================================================================');
    console.log('  V8 (Node.js) — Object-Heavy Benchmark');
    console.log('  Same workload as bench_object_heavy.c');
    console.log('  ' + N_SAMPLES + ' samples, median reported');
    console.log('================================================================\n');

    const r = bench(1000, 2000);
    console.log('  Benchmark              V8 (ns)');
    console.log('  ----------             -------');
    console.log('  object_heavy(1K)     ' + r.toFixed(1).padStart(10));

    console.log('\n--- JSON ---');
    console.log(JSON.stringify([{name: 'object_heavy(1K)', ns: r}]));
}

main();
