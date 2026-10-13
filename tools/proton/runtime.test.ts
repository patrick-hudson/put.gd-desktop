import { expect, test } from 'bun:test';
import { MemberRole, NodeType, type NodeEntity } from '@protontech/drive-sdk';
import { createFolder, destinationFolder, inspectAccount, stat } from './runtime';
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
