import { expect, test } from 'bun:test';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { MemberRole, NodeType, type NodeEntity } from '@protontech/drive-sdk';
import { checkName, createFolder, destinationFolder, inspectAccount, stat, upload } from './runtime';
import { makeReceipt, type JsonObject } from './protocol';

type Context = Parameters<typeof inspectAccount>[0];
type Call = { method: string; args: unknown[] };
const destination = { providerId: 'proton-drive', profileId: 'profile', profileRevision: 'revision',
    settings: { accountRootUid: 'root', folderUid: '' }, credentials: { mode: 'keychain', reference: 'synthetic-reference' } };
const setup = { schemaVersion: 1, method: 'inspectAccount', destination, options: { name: 'Uploads' } };
function folder(uid: string, parentUid?: string): NodeEntity {
    return { uid, parentUid, type: NodeType.Folder, name: { ok: true, value: uid === 'root' ? 'My files' : 'Uploads' },
        keyAuthor: { ok: true, value: 'synthetic@example.invalid' }, nameAuthor: { ok: true, value: 'synthetic@example.invalid' },
        directRole: MemberRole.Admin, ownedBy: { email: 'synthetic@example.invalid' }, isShared: false, isSharedByUrl: false,
        creationTime: new Date(0), modificationTime: new Date(0), treeEventScopeId: 'scope' };
}
// Only SDK methods reached by production setup/stat functions are stubbed.
// This proves their branching and scope, not a live account or desktop gate.
function context(options: { rootUid?: string; listedUids?: string[]; listThrows?: boolean } = {}) {
    const calls: Call[] = [];
    let listCompleted = false;
    const ctx = { auth: { isLoggedIn: () => true }, sdk: {
        getMyFilesRootFolder: async () => {
            calls.push({ method: 'root', args: [] }); return folder(options.rootUid ?? 'root');
        },
        iterateFolderChildren: async function* (...args: unknown[]) {
            calls.push({ method: 'children', args }); yield folder('folder', 'root');
        },
        createFolder: async (...args: unknown[]) => {
            calls.push({ method: 'create', args }); return folder('new-folder', 'root');
        },
        getNode: async (uid: string) => {
            calls.push({ method: 'node', args: [uid] });
            if (uid === 'folder') return folder('folder', 'root');
            throw new Error('Synthetic denied lookup');
        },
        iterateFolderChildrenNodeUids: async function* (...args: unknown[]) {
            calls.push({ method: 'uids', args });
            for (const uid of options.listedUids ?? []) yield uid;
            if (options.listThrows) throw new Error('Synthetic incomplete listing');
            listCompleted = true;
        },
    } } as unknown as Context;
    return { ctx, calls, listCompleted: () => listCompleted };
}
function statRequest(): JsonObject {
    const upload = { schemaVersion: 1, method: 'upload', destination: {
        ...destination, settings: { accountRootUid: 'root', folderUid: 'folder' } }, candidate: 'photo.png',
        publicationId: 'publication', artifact: { sha256: 'a'.repeat(64) } };
    return { ...upload, method: 'stat', receipt: makeReceipt(upload, 'file-node', 'file-revision') };
}
test('inspect accepts a matching configured account before any folder is selected', async () => {
    const stub = context();
    expect(await inspectAccount(stub.ctx, setup)).toEqual({ accountRootUid: 'root', folders: [{ uid: 'folder', name: 'Uploads' }] });
    expect(stub.calls.map(call => call.method)).toEqual(['root', 'children']);
    expect(stub.calls[1].args[0]).toBe('root');
    expect(stub.calls[1].args[1]).toEqual({ type: NodeType.Folder });
});
test('first folder creation accepts matching configured account with empty folder UID', async () => {
    const stub = context();
    expect(await createFolder(stub.ctx, setup)).toEqual({ uid: 'new-folder', name: 'Uploads', accountRootUid: 'root' });
    expect(stub.calls).toEqual([{ method: 'root', args: [] }, { method: 'create', args: ['root', 'Uploads'] }]);
});
test('wrong configured account rejects inspection and creation before either SDK action', async () => {
    for (const operation of [inspectAccount, createFolder]) {
        const stub = context({ rootUid: 'other-root' });
        await expect(operation(stub.ctx, setup)).rejects.toThrow('account_changed');
        expect(stub.calls).toEqual([{ method: 'root', args: [] }]);
    }
});
test('file operations still require a selected folder', async () => {
    const stub = context();
    await expect(destinationFolder(stub.ctx, setup)).rejects.toThrow('invalid_request');
    expect(stub.calls).toEqual([{ method: 'root', args: [] }]);
});
test('failed node lookup remains unknown when destination UID listing contains the target', async () => {
    const stub = context({ listedUids: ['other-node', 'file-node'] });
    expect((await stat(stub.ctx, statRequest())).status).toBe('unknown');
    expect(stub.calls.map(call => call.method)).toEqual(['root', 'node', 'node', 'uids']);
    expect(stub.calls.at(-1)?.args[0]).toBe('folder');
});
test('failed node lookup is missing only after a complete destination UID list excludes it', async () => {
    const stub = context({ listedUids: ['other-node'] });
    expect((await stat(stub.ctx, statRequest())).status).toBe('missing');
    expect(stub.listCompleted()).toBe(true);
});
test('failed or incomplete destination UID listing cannot establish absence', async () => {
    const stub = context({ listedUids: ['other-node'], listThrows: true });
    expect((await stat(stub.ctx, statRequest())).status).toBe('unknown');
    expect(stub.listCompleted()).toBe(false);
});

test('name preflight uses SDK hashed availability without listing unrelated folder contents', async () => {
    for (const available of ['photo.png', 'photo (1).png']) {
        const stub = context();
        (stub.ctx.sdk as any).getAvailableName = async (...args: unknown[]) => { stub.calls.push({ method: 'available', args }); return available; };
        const request = { ...setup, destination: { ...destination, settings: { accountRootUid: 'root', folderUid: 'folder' } }, candidate: 'photo.png' };
        const result = await checkName(stub.ctx, request);
        expect(result.status).toBe(available === 'photo.png' ? 'absent' : 'present');
        expect(stub.calls.map(call => call.method)).toEqual(['root', 'node', 'available']);
        expect(stub.calls[2].args).toEqual(['folder', 'photo.png']);
    }
});
test('failed name availability is inconclusive rather than permission-as-absence', async () => {
    const stub = context();
    (stub.ctx.sdk as any).getAvailableName = async () => { throw new Error('Synthetic denied name lookup'); };
    const request = { ...setup, destination: { ...destination, settings: { accountRootUid: 'root', folderUid: 'folder' } }, candidate: 'photo.png' };
    expect((await checkName(stub.ctx, request)).status).toBe('inconclusive');
});

test('production upload wires the SDK progress callback and confirms publication only through completion', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'putgd-progress-'));
    const path = join(directory, 'synthetic.bin'), data = Buffer.alloc(65536, 112);
    await writeFile(path, data, { mode: 0o600 });
    let progressType: string | undefined;
    try {
        const stub = context();
        (stub.ctx.sdk as any).getFileUploader = async () => ({
            uploadFromStream: async (stream: ReadableStream, thumbnails: unknown[], onProgress?: (bytes: number) => void) => {
                progressType = typeof onProgress;
                const reader = stream.getReader();
                while (!(await reader.read()).done) {}
                onProgress?.(100000); onProgress?.(65536); // Retry rollback; overhead can exceed artifact size.
                return { completion: async () => ({ nodeUid: 'file-node', nodeRevisionUid: 'file-revision' }) };
            },
        });
        const result = await upload(stub.ctx, { ...statRequest(), method: 'upload', condition: 'absent',
            artifact: { path, bytes: data.length, mime: 'application/octet-stream', sha256: createHash('sha256').update(data).digest('hex') } });
        expect(progressType).toBe('function'); expect(result.status).toBe('uploaded');
        expect(result.receipt.objectReference.revisionUid).toBe('file-revision');
    } finally { await rm(directory, { recursive: true, force: true }); }
});
