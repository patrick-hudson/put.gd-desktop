import { writeJson } from './stdout';
import type { JsonObject } from './protocol';

type Phase = 'preparing' | 'uploading' | 'finalizing';
// SDK progress callbacks are synchronous. Retain one latest report and at most
// one pending stdout write; never accumulate a promise for every block/retry.
export function createProgressReporter(write: (value: JsonObject) => Promise<void> = writeJson) {
    let latest: JsonObject | undefined;
    let writing: Promise<void> | undefined;
    let timer: ReturnType<typeof setTimeout> | undefined;
    let lastWrite = -Infinity;
    let closed = false;
    let writeError: unknown;
    let finishing: Promise<void> | undefined;
    let phase: Phase = 'preparing';
    let bytes: number | undefined;
    const schedule = () => {
        if (closed || writing || timer || !latest || writeError) return;
        timer = setTimeout(() => { timer = undefined; startWrite(); }, Math.max(0, 500 - (performance.now() - lastWrite)));
    };
    const startWrite = () => {
        if (writing || !latest || writeError) return;
        const value = latest; latest = undefined; lastWrite = performance.now();
        writing = write(value).catch(error => { writeError = error; latest = undefined; }).finally(() => {
            writing = undefined; schedule();
        });
    };
    const flush = async () => {
        if (timer) { clearTimeout(timer); timer = undefined; }
        while (writing || latest) {
            if (!writing) startWrite();
            await writing;
            if (writeError) throw writeError;
        }
        if (writeError) throw writeError;
    };
    const queue = () => {
        latest = { event: 'progress', phase, ...(bytes === undefined ? {} : { bytesTransferred: bytes }) };
        schedule();
    };
    return {
        async phase(value: Phase) {
            if (closed) return;
            phase = value; queue(); await flush();
        },
        report(value: number) {
            if (closed || !Number.isSafeInteger(value) || value < 0) return;
            bytes = value; queue();
        },
        finish(): Promise<void> {
            if (finishing) return finishing;
            closed = true;
            return finishing = flush();
        },
    };
}
