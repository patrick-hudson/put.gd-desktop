import { failure, type JsonObject } from './protocol';

// A closed reader has no useful error channel. Never let Bun print an unhandled stream error.
process.stdout.on('error', () => process.exit(1));

export async function writeJson(value: JsonObject): Promise<void> {
    let serialized = JSON.stringify(value);
    if (Buffer.byteLength(serialized) > 1024 * 1024)
        serialized = JSON.stringify({ ok: false, error: failure('operation_failed') });
    await new Promise<void>((resolve, reject) => {
        process.stdout.write(serialized + '\n', error => error ? reject(error) : resolve());
    });
}
