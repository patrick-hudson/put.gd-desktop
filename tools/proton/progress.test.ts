import { expect, jest, spyOn, test } from 'bun:test';
import { createProgressReporter } from './progress';
import type { JsonObject } from './protocol';

test('progress coalesces synchronous SDK bursts, preserves retry rollback and has no guessed total', async () => {
    const events: JsonObject[] = [];
    const reporter = createProgressReporter(async value => { events.push(value); });
    await reporter.phase('uploading');
    for (let i = 0; i < 100000; ++i) reporter.report(i);
    reporter.report(64); // The SDK rolls back accounted encrypted bytes on retries.
    await reporter.finish();
    expect(events).toEqual([{ event: 'progress', phase: 'uploading' }, { event: 'progress', phase: 'uploading', bytesTransferred: 64 }]);
    reporter.report(128);
    await reporter.finish();
    expect(events).toHaveLength(2);
});
test('slow stdout has one in-flight write and one replaceable report, then drains before completion', async () => {
    let release!: () => void;
    let active = 0, maximum = 0;
    const events: JsonObject[] = [];
    const reporter = createProgressReporter(async value => {
        maximum = Math.max(maximum, ++active); events.push(value);
        if (events.length === 1) await new Promise<void>(resolve => { release = resolve; });
        --active;
    });
    const first = reporter.phase('uploading');
    for (let i = 0; i < 100000; ++i) reporter.report(i);
    const finishing = reporter.finish();
    expect(events).toHaveLength(1);
    release(); await first; await finishing;
    expect(maximum).toBe(1); expect(events).toHaveLength(2);
    expect(events[1].bytesTransferred).toBe(99999);
});
test('invalid counters are ignored and failed writes are surfaced without an unhandled promise', async () => {
    const events: JsonObject[] = [];
    const reporter = createProgressReporter(async value => { events.push(value); });
    for (const value of [-1, Infinity, NaN, 1.5, Number.MAX_SAFE_INTEGER + 1]) reporter.report(value);
    await reporter.finish(); expect(events).toEqual([]);
    const broken = createProgressReporter(async () => { throw new Error('Synthetic closed reader'); });
    await expect(broken.phase('uploading')).rejects.toThrow('Synthetic closed reader');
    await expect(broken.finish()).rejects.toThrow('Synthetic closed reader');
});

async function settleWrites() {
    // Drain the writer's catch/finally continuation without advancing any timer.
    for (let i = 0; i < 4; ++i) await Promise.resolve();
}
function controlledClock() {
    let now = 0;
    jest.useFakeTimers();
    const clock = spyOn(performance, 'now').mockImplementation(() => now);
    return {
        async advance(milliseconds: number) {
            now += milliseconds;
            jest.advanceTimersByTime(milliseconds);
            await settleWrites();
        },
        restore() { clock.mockRestore(); jest.useRealTimers(); },
    };
}
test('scheduled progress reaches the reader while the reporter remains open', async () => {
    const clock = controlledClock();
    const events: JsonObject[] = [];
    const reporter = createProgressReporter(async value => { events.push(value); });
    try {
        await reporter.phase('uploading');
        reporter.report(100); reporter.report(200);
        await clock.advance(499);
        expect(events).toEqual([{ event: 'progress', phase: 'uploading' }]);
        await clock.advance(1);
        expect(events[1]).toEqual({ event: 'progress', phase: 'uploading', bytesTransferred: 200 });
        reporter.report(75); // A retry rollback must also arrive before finish().
        await clock.advance(500);
        expect(events[2]).toEqual({ event: 'progress', phase: 'uploading', bytesTransferred: 75 });
        expect(events).toHaveLength(3);
    } finally { try { await reporter.finish(); } finally { clock.restore(); } }
});
test('an open blocked write reschedules the latest counter without overlapping writes', async () => {
    const clock = controlledClock();
    let release!: () => void;
    const blocked = new Promise<void>(resolve => { release = resolve; });
    let active = 0, maximum = 0;
    const events: JsonObject[] = [];
    const reporter = createProgressReporter(async value => {
        maximum = Math.max(maximum, ++active); events.push(value);
        if (events.length === 2) await blocked;
        --active;
    });
    try {
        await reporter.phase('uploading'); reporter.report(10);
        await clock.advance(500);
        expect(events[1]).toEqual({ event: 'progress', phase: 'uploading', bytesTransferred: 10 }); expect(active).toBe(1);
        reporter.report(20); reporter.report(30);
        await clock.advance(1000);
        expect(events).toHaveLength(2); expect(maximum).toBe(1);
        release(); await settleWrites(); await clock.advance(0);
        expect(events[2]).toEqual({ event: 'progress', phase: 'uploading', bytesTransferred: 30 });
        expect(events).toHaveLength(3); expect(maximum).toBe(1);
        reporter.report(40); await clock.advance(500);
        expect(events[3]).toEqual({ event: 'progress', phase: 'uploading', bytesTransferred: 40 }); expect(maximum).toBe(1);
    } finally { release(); try { await reporter.finish(); } finally { clock.restore(); } }
});
