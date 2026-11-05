// Standalone private Bun 1.4.4 broker. API provenance: tools/proton/account.ts
// and pinned upstream/cli/src/credentials/{parseCredentials,secretCredentialsStore}.
// The Qt relay holds the production account QLockFile for every invocation.
import { fstatSync } from 'node:fs';
const SERVICE = 'ch.proton.drive/putgd';
const MAX_INPUT = 262144;
const MAX_SNAPSHOT = 65536;
const LAB_REFERENCE = /^putgd-lab-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
type ObjectValue = Record<string, unknown>;

function fail(): never { throw new Error('credential_provisioning_failed'); }
function object(value: unknown): ObjectValue {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) fail();
    return value as ObjectValue;
}
function text(value: unknown, limit: number, empty = false): string {
    if (typeof value !== 'string' || value.length > limit || (!empty && !value) || /[\x00-\x1f]/.test(value)) fail();
    return value;
}
function snapshot(value: unknown): string {
    if (typeof value !== 'string' || Buffer.byteLength(value, 'utf8') > MAX_SNAPSHOT) fail();
    const stored = object(JSON.parse(value));
    const session = object(stored.session);
    for (const field of [stored.userKeyPassword, session.uid, session.accessToken])
        if (typeof field !== 'string' || !field) fail();
    if (session.refreshToken && typeof session.refreshToken !== 'string') fail();
    if (stored.cachePassword && typeof stored.cachePassword !== 'string') fail();
    if (stored.telemetryEnabled !== undefined && typeof stored.telemetryEnabled !== 'boolean') fail();
    return value; // Keep the official serialization and extension fields opaque.
}
async function input(): Promise<ObjectValue> {
    const chunks: Uint8Array[] = [];
    let size = 0;
    for await (const chunk of Bun.stdin.stream()) {
        size += chunk.byteLength;
        if (size > MAX_INPUT) fail();
        chunks.push(chunk);
    }
    return object(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks))));
}
async function run(): Promise<ObjectValue> {
    const request = await input();
    const op = request.op;
    const fields: Record<string, string[]> = {
        export: ['op', 'reference'], import: ['op', 'reference', 'snapshot'],
        replace: ['op', 'reference', 'expectedSnapshot', 'snapshot'], delete: ['op', 'reference'],
    };
    if (typeof op !== 'string' || !Object.hasOwn(fields, op) ||
        Object.keys(request).length !== fields[op].length || Object.keys(request).some(key => !fields[op].includes(key))) fail();
    const reference = text(request.reference, 256);
    if (op === 'export') {
        const output = fstatSync(1);
        if (!output.isFIFO() && !output.isSocket()) fail(); // No terminal/file snapshot export.
    }
    if ((op === 'import' || op === 'delete') && !LAB_REFERENCE.test(reference)) fail();
    const value = op === 'import' || op === 'replace' ? snapshot(request.snapshot) : undefined;
    const expected = op === 'replace' ? snapshot(request.expectedSnapshot) : undefined;
    const existing = await Bun.secrets.get({ service: SERVICE, name: reference });
    if (op === 'export') {
        return { ok: true, snapshot: snapshot(existing) }; // Raw reply goes only to the private Qt pipe.
    }
    if (op === 'import') {
        if (existing !== null) fail(); // Never replace an unrelated or previous entry.
        await Bun.secrets.set({ service: SERVICE, name: reference, value: value! });
    } else if (op === 'replace') {
        if (existing !== expected) fail(); // Exact in-memory baseline under the shared production lock.
        await Bun.secrets.set({ service: SERVICE, name: reference, value: value! });
    } else if (existing !== null) {
        await Bun.secrets.delete({ service: SERVICE, name: reference }); // Local only; no remote logout.
    }
    return { ok: true };
}
try {
    // No operation or secret material is read from argv.
    const result = JSON.stringify(await run()) + '\n';
    if (Buffer.byteLength(result, 'utf8') > MAX_INPUT) fail();
    await Bun.write(Bun.stdout, result);
} catch {
    await Bun.write(Bun.stdout, '{"ok":false,"status":"credential_provisioning_failed"}\n');
    process.exitCode = 1; // Backend exceptions never reach stderr or stdout.
}
