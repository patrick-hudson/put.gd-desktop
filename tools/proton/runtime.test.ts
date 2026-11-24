import { afterEach, expect, test } from 'bun:test';
import http2 from 'node:http2';
import zlib from 'node:zlib';
import type { AddressInfo } from 'node:net';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { MemberRole, NodeType, RevisionState, NodeWithSameNameExistsValidationError, RateLimitedError, ServerError, ValidationError, type NodeEntity } from '@protontech/drive-sdk';
import { checkName, createFolder, destinationFolder, readDestination, inspectAccount, sessionUnread, stat, upload, share, serviceFailure, transferHost, REREAD_PAUSES_MS } from './runtime';
import { ProtocolError, makeReceipt, type JsonObject } from './protocol';
import { LINK_AHEAD_MS, LinkAhead, LinkOpenPGP, SLOW_ANSWER_MS, SharedAccountReads, fastestConnectionFetch, keepUserDataAcrossRefresh, keychain, linkPasswordAhead, linkPreparation, saveToKeychain } from './account';
import { initSharingModule } from '@protontech/drive-sdk/internal/sharing';
import { Addresses } from 'proton-drive-sdk-account';
import { Credentials } from './upstream/cli/src/credentials/credentials';
import { AccountApiError, ApiClient } from 'proton-drive-sdk-account';

type Context = Parameters<typeof inspectAccount>[0];
type Call = { method: string; args: unknown[] };
test('account response hooks synchronize crypto server time using official header precedence', async () => {
    // Crypto time is monotonic module state: isolate it from other SDK tests.
    const script = `
        import assert from 'node:assert/strict';
        import { accountResponseHooks } from './account.ts';
        import { serverTime } from '@protontech/crypto/serverTime';
        const hooks = accountResponseHooks(() => { throw new Error('unexpected requirement refusal'); });
        for (const [headers, expected] of [
            [{ date: 'Fri, 09 Oct 2026 05:00:00 GMT' }, '2026-10-09T05:00:00.000Z'],
            [{ 'x-pm-date': 'invalid', date: 'Sat, 10 Oct 2026 05:00:00 GMT' }, '2026-10-10T05:00:00.000Z'],
            [{ 'x-pm-date': 'Sun, 11 Oct 2026 06:00:00 GMT', date: 'Mon, 12 Oct 2026 06:00:00 GMT' }, '2026-10-11T06:00:00.000Z'],
            [{ date: 'invalid' }, '2026-10-11T06:00:00.000Z'],
        ]) {
            for (const hook of hooks) await hook(new Request('https://synthetic.invalid'), {}, new Response(null, { headers }), {});
            assert.equal(serverTime().toISOString(), expected);
        }
        console.log('server-time-hooks-passed');`;
    const child = Bun.spawn([process.execPath, '--eval', script], {
        cwd: import.meta.dir, stdout: 'pipe', stderr: 'pipe',
    });
    const [stdout, stderr, code] = await Promise.all([
        new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited,
    ]);
    expect(code).toBe(0); expect(stderr).toBe(''); expect(stdout.trim()).toBe('server-time-hooks-passed');
});
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
function context(options: { rootUid?: string; listedUids?: string[]; listThrows?: boolean; batchThrows?: boolean } = {}) {
    const calls: Call[] = [];
    let listCompleted = false, linkPrepared = false, settledBeforeChange = 0, transfers: ((host: string) => void) | undefined;
    const ctx = { auth: { isLoggedIn: () => true }, fresh: (read: () => Promise<unknown>) => read(),
        warm: () => { calls.push({ method: 'warm', args: [] }); },
        modulusAhead: () => { calls.push({ method: 'modulus', args: [] }); return () => { calls.push({ method: 'modulus-unused', args: [] }); }; },
        watchTransfers: (seen?: (host: string) => void) => { transfers = seen; },
        prepareLink: () => { calls.push({ method: 'prepare-link', args: [] }); linkPrepared = true; },
        linkBeingMade: (making: boolean) => { calls.push({ method: 'link-being-made', args: [making] }); },
        linkPrepared: () => linkPrepared,
        linkUnused: () => { calls.push({ method: 'link-unused', args: [] }); linkPrepared = false; },
        connectionSettled: () => { ++settledBeforeChange; }, sdk: {
        // The batched read: the root as the service names it, and each node the single-node
        // stub answers. A node whose lookup the stub refuses is absent from the batch.
        iterateNodes: async function* (uids: string[]) {
            calls.push({ method: 'nodes', args: [uids] });
            for (const uid of uids) {
                if (uid === (options.rootUid ?? 'root')) { yield folder(uid); continue; }
                let node;
                try { node = await (ctx.sdk as any).getNode(uid); } catch (error) { if (options.batchThrows) throw error; continue; }
                yield node;
            }
        },
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
    return { ctx, calls, listCompleted: () => listCompleted, transfer: (host: string) => transfers?.(host), watching: () => !!transfers,
        prepared: (value: boolean) => { linkPrepared = value; }, settledBeforeChange: () => settledBeforeChange };
}
function statRequest(): JsonObject {
    const upload = { schemaVersion: 1, method: 'upload', destination: {
        ...destination, settings: { accountRootUid: 'root', folderUid: 'folder' } }, candidate: 'photo.png',
        publicationId: 'publication', artifact: { sha256: 'a'.repeat(64) } };
    return { ...upload, method: 'stat', receipt: makeReceipt(upload, 'file-node', 'file-revision') };
}
test('inspect accepts a matching configured account before any folder is selected', async () => {
    const stub = context();
    expect(await inspectAccount(stub.ctx, setup)).toEqual({ accountRootUid: 'root', accountEmail: 'synthetic@example.invalid', folders: [{ uid: 'folder', name: 'Uploads' }] });
    expect(stub.calls.map(call => call.method)).toEqual(['warm', 'root', 'children']);
    expect(stub.calls[2].args[0]).toBe('root');
    expect(stub.calls[2].args[1]).toEqual({ type: NodeType.Folder });
});
test('first folder creation accepts matching configured account with empty folder UID', async () => {
    const stub = context();
    expect(await createFolder(stub.ctx, setup)).toEqual({ uid: 'new-folder', name: 'Uploads', accountRootUid: 'root' });
    expect(stub.calls).toEqual([{ method: 'warm', args: [] }, { method: 'root', args: [] }, { method: 'create', args: ['root', 'Uploads'] }]);
});
test('wrong configured account rejects inspection and creation before either SDK action', async () => {
    for (const operation of [inspectAccount, createFolder]) {
        const stub = context({ rootUid: 'other-root' });
        await expect(operation(stub.ctx, setup)).rejects.toThrow('account_changed');
        expect(stub.calls).toEqual([{ method: 'warm', args: [] }, { method: 'root', args: [] }]);
    }
});
test('another account under the configured root rejects file operations whatever the batch returned', async () => {
    const request = { ...setup, destination: { ...destination, settings: { accountRootUid: 'root', folderUid: 'folder' } } };
    for (const batchThrows of [false, true]) {
        const stub = context({ rootUid: 'other-root', batchThrows });
        (stub.ctx.sdk as any).getNode = async () => { throw new Error('Synthetic foreign volume'); };
        await expect(destinationFolder(stub.ctx, request)).rejects.toThrow('account_changed');
    }
});
test('the folder and a further node are read from the service in one batch, past the request cache', async () => {
    const stub = context(); let fresh = 0, batchesWhileFresh = 0, reading = false;
    stub.ctx.fresh = async read => { ++fresh; reading = true; try { return await read(); } finally { reading = false; } };
    const batch = (stub.ctx.sdk as any).iterateNodes;
    (stub.ctx.sdk as any).iterateNodes = (...args: unknown[]) => { if (reading) ++batchesWhileFresh; return batch(...args); };
    const request = { ...setup, destination: { ...destination, settings: { accountRootUid: 'root', folderUid: 'folder' } } };
    const { folder: selected, nodes } = await readDestination(stub.ctx, request, ['file-node', 'folder']);
    expect(selected.uid).toBe('folder'); expect([...nodes.keys()]).toEqual(['root', 'folder']);
    expect(fresh).toBe(1); expect(batchesWhileFresh).toBe(1);
    expect(stub.calls.filter(call => call.method === 'nodes').map(call => call.args[0])).toEqual([['root', 'folder', 'file-node']]);
});
test('a folder the service no longer returns, or one outside My files, is an unavailable destination', async () => {
    for (const parent of [undefined, 'elsewhere']) {
        const stub = context();
        (stub.ctx.sdk as any).getNode = async (uid: string) => { if (parent) return folder(uid, parent); throw new Error('Synthetic missing folder'); };
        const request = { ...setup, destination: { ...destination, settings: { accountRootUid: 'root', folderUid: 'folder' } } };
        await expect(destinationFolder(stub.ctx, request)).rejects.toThrow('destination_unavailable');
    }
});
test('file operations still require a selected folder', async () => {
    const stub = context();
    await expect(destinationFolder(stub.ctx, setup)).rejects.toThrow('invalid_request');
    expect(stub.calls).toEqual([]);
});
test('failed node lookup remains unknown when destination UID listing contains the target', async () => {
    const stub = context({ listedUids: ['other-node', 'file-node'] });
    expect((await stat(stub.ctx, statRequest())).status).toBe('unknown');
    expect(stub.calls.map(call => call.method)).toEqual(['warm', 'nodes', 'warm', 'root', 'uids']);
    expect(stub.calls[1].args[0]).toEqual(['root', 'folder', 'file-node']);
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
        expect(stub.calls.map(call => call.method)).toEqual(['warm', 'nodes', 'warm', 'root', 'available']);
        expect(stub.calls[4].args).toEqual(['folder', 'photo.png', NodeType.File]);
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

test('unfinished upload collision remains distinguishable without overriding another client draft', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'putgd-draft-'));
    const path = join(directory, 'synthetic.bin'), data = Buffer.alloc(32, 112);
    await writeFile(path, data, { mode: 0o600 });
    try {
        const stub = context(); let override;
        (stub.ctx.sdk as any).getFileUploader = async (_folder: string, _name: string, metadata: { overrideExistingDraftByOtherClient?: boolean }) => {
            override = metadata.overrideExistingDraftByOtherClient;
            return { uploadFromStream: async () => { throw new NodeWithSameNameExistsValidationError('Synthetic draft', 2500, 'draft-node', true); } };
        };
        const result = await upload(stub.ctx, { ...statRequest(), method: 'upload', condition: 'absent',
            artifact: { path, bytes: data.length, mime: 'application/octet-stream', sha256: createHash('sha256').update(data).digest('hex') } });
        expect(result.status).toBe('collision'); expect(result.error.code).toBe('upload_draft_conflict');
        expect(override).not.toBe(true);
    } finally { await rm(directory, { recursive: true, force: true }); }
});


test('name and ownership failures retain safe service recovery codes', async () => {
    const cases = [
        [new RateLimitedError('synthetic-private-provider-detail'), 'rate_limited'],
        [new ValidationError('synthetic-private-provider-detail', 2011), 'permission_denied'],
        [new ValidationError('synthetic-private-provider-detail', 200002), 'quota_exceeded'],
        [new ValidationError('synthetic-private-provider-detail', 200003), 'destination_file_too_large_for_plan'],
        [Object.assign(new ServerError('synthetic-private-provider-detail'), { statusCode: 401 }), 'authentication_required'],
    ] as const;
    for (const [error, code] of cases) {
        const stub = context();
        (stub.ctx.sdk as any).getAvailableName = async () => { throw error; };
        const check: JsonObject = await checkName(stub.ctx, { ...statRequest(), method: 'checkName' });
        expect(check.status).toBe('inconclusive');
        expect(check.error.code).toBe(code);
        expect(JSON.stringify(check)).not.toContain('synthetic-private-provider-detail');
        (stub.ctx.sdk as any).getMyFilesRootFolder = async () => { throw error; };
        const inspected: JsonObject = await stat(stub.ctx, statRequest());
        expect(inspected.status).toBe('unknown');
        expect(inspected.error.code).toBe(code);
    }
});

test('quota and rate limiting after upload dispatch retain uncertainty and recovery guidance', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'putgd-p1-'));
    const path = join(directory, 'synthetic.bin'), data = Buffer.alloc(32, 112);
    await writeFile(path, data, { mode: 0o600 });
    try {
        for (const [error, code] of [
            [new ValidationError('synthetic-private-provider-detail', 200002), 'quota_exceeded'],
            [new ValidationError('synthetic-private-provider-detail', 200003), 'destination_file_too_large_for_plan'],
            [new RateLimitedError('synthetic-private-provider-detail'), 'rate_limited'],
        ] as const) {
            const stub = context(); let dispatched = 0;
            (stub.ctx.sdk as any).getFileUploader = async () => { ++dispatched; throw error; };
            const result = await upload(stub.ctx, { ...statRequest(), method: 'upload', condition: 'absent',
                artifact: { path, bytes: data.length, mime: 'application/octet-stream', sha256: createHash('sha256').update(data).digest('hex') } });
            expect(dispatched).toBe(1);
            expect(result.status).toBe('unknown');
            expect(result.error.code).toBe(code);
            expect(JSON.stringify(result)).not.toContain('synthetic-private-provider-detail');
        }
    } finally { await rm(directory, { recursive: true, force: true }); }
});


function sharingContext() {
    const stub = context();
    const request: JsonObject = { ...statRequest(), method: 'share', options: { action: 'create', expirationDays: 2, passwordReference: null } };
    const proof = request.receipt;
    let reads = 0, mutations = 0, changedAfter = false, deniedAfter = false;
    let revision = proof.objectReference.revisionUid;
    let sharingError: Error | undefined, expiration: Date | undefined;
    (stub.ctx.sdk as any).getNode = async (uid: string) => {
        if (uid === 'folder') return folder('folder', 'root');
        ++reads;
        if (deniedAfter && mutations) throw new ValidationError('synthetic-private-provider-detail', 2011);
        return { ...folder(uid, 'folder'), type: NodeType.File, name: { ok: true, value: 'photo.png' },
            activeRevision: { uid: changedAfter && mutations ? 'changed-revision' : revision, state: RevisionState.Active,
                contentAuthor: { ok: true, value: 'synthetic@example.invalid' },
                claimedAdditionalMetadata: { putgd: { publicationId: 'publication', sha256: 'a'.repeat(64) } } } };
    };
    (stub.ctx.sdk as any).iterateFolderChildrenNodeUids = async function* () { throw new ValidationError('synthetic-private-provider-detail', 2011); };
    const info = () => {
        if (sharingError) throw sharingError;
        return { urlAccess: { url: 'https://synthetic.invalid/share#capability', expirationTime: expiration } };
    };
    (stub.ctx.sdk as any).shareNode = async (...args: unknown[]) => { ++mutations; stub.calls.push({ method: 'share', args }); return info(); };
    (stub.ctx.sdk as any).unshareNode = async () => { ++mutations; if (sharingError) throw sharingError; return {}; };
    (stub.ctx.sdk as any).getSharingInfo = async () => info();
    return { ...stub, request, reads: () => reads, mutations: () => mutations,
        changeBefore: () => { revision = 'changed-revision'; }, changeAfter: () => { changedAfter = true; },
        denyAfter: () => { deniedAfter = true; }, failSharing: (error: Error) => { sharingError = error; },
        expire: () => { expiration = new Date(Date.now() - 1000); } };
}
test('a plan or quota refusal settles as failed only when the name is still free afterwards', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'putgd-plan-'));
    const path = join(directory, 'synthetic.bin'), data = Buffer.alloc(32, 112);
    await writeFile(path, data, { mode: 0o600 });
    try {
        // Free name: neither a completed file nor a draft holds it, so nothing was created (#201).
        // Taken name (a draft the SDK could not delete, or a racing file) or a failed query stays unknown.
        for (const [serviceCode, code] of [[200003, 'destination_file_too_large_for_plan'], [200002, 'quota_exceeded']] as const) {
            for (const [available, expected] of [['photo.png', 'failed'], ['photo (1).png', 'unknown'], [undefined, 'unknown']] as const) {
                const stub = context();
                (stub.ctx.sdk as any).getFileUploader = async () => ({
                    uploadFromStream: async () => ({ completion: async () => { throw new ValidationError('synthetic-private-provider-detail', serviceCode); } }),
                });
                (stub.ctx.sdk as any).getAvailableName = async (...args: unknown[]) => {
                    stub.calls.push({ method: 'available', args });
                    if (available === undefined) throw new Error('Synthetic denied name lookup');
                    return available;
                };
                const result = await upload(stub.ctx, { ...statRequest(), method: 'upload', condition: 'absent',
                    artifact: { path, bytes: data.length, mime: 'application/octet-stream', sha256: createHash('sha256').update(data).digest('hex') } });
                expect(result.status).toBe(expected);
                expect(result.error.code).toBe(code);
                expect(stub.calls.filter(call => call.method === 'available').map(call => call.args)).toEqual([['folder', 'photo.png', NodeType.File]]);
                expect(JSON.stringify(result)).not.toContain('synthetic-private-provider-detail');
            }
        }
        // Other refusals never consult the name: rate limiting says nothing about what was created.
        const stub = context(); let consulted = 0;
        (stub.ctx.sdk as any).getFileUploader = async () => { throw new RateLimitedError('synthetic-private-provider-detail'); };
        (stub.ctx.sdk as any).getAvailableName = async () => { ++consulted; return 'photo.png'; };
        const result = await upload(stub.ctx, { ...statRequest(), method: 'upload', condition: 'absent',
            artifact: { path, bytes: data.length, mime: 'application/octet-stream', sha256: createHash('sha256').update(data).digest('hex') } });
        expect(result.status).toBe('unknown'); expect(result.error.code).toBe('rate_limited'); expect(consulted).toBe(0);
    } finally { await rm(directory, { recursive: true, force: true }); }
});

test('changed revision blocks sharing before mutation and suppresses a raced link after mutation', async () => {
    for (const when of ['before', 'after']) {
        const stub = sharingContext();
        if (when === 'before') stub.changeBefore(); else stub.changeAfter();
        const result = await share(stub.ctx, stub.request);
        expect(result.status).toBe('changed');
        expect(result.linkAvailable).toBe(false); expect(result.receipt.directUrl).toBe('');
        expect(stub.mutations()).toBe(when === 'before' ? 0 : 1);
        expect(result.receipt.objectReference).toEqual(stub.request.receipt.objectReference);
    }
});
test('sharing failures and denied postchecks retain unknown outcomes without exposing an old link', async () => {
    for (const action of ['create', 'revoke', 'inspect']) {
        const stub = sharingContext(); stub.request.options.action = action;
        stub.request.receipt.directUrl = 'https://synthetic.invalid/old#capability';
        stub.failSharing(new RateLimitedError('synthetic-private-provider-detail'));
        const result = await share(stub.ctx, stub.request);
        expect(result.status).toBe('unknown'); expect(result.error.code).toBe('rate_limited');
        expect(result.linkAvailable).toBe(false); expect(result.receipt.directUrl).toBe('');
        expect(stub.mutations()).toBe(action === 'inspect' ? 0 : 1);
        expect(JSON.stringify(result)).not.toContain('synthetic-private-provider-detail');
    }
    const denied = sharingContext(); denied.denyAfter();
    const result = await share(denied.ctx, denied.request);
    expect(denied.mutations()).toBe(1); expect(result.status).toBe('unknown');
    expect(result.error.code).toBe('permission_denied'); expect(result.receipt.directUrl).toBe('');
});
test('sharing preserves protection options and expired inspection never creates a replacement link', async () => {
    const stub = sharingContext();
    const result = await share(stub.ctx, stub.request);
    expect(result.status).toBe('shared'); expect(stub.mutations()).toBe(1); expect(stub.reads()).toBe(2);
    const options = stub.calls.find(call => call.method === 'share')!.args[1] as { urlAccess: { expiration: Date; role: MemberRole } };
    expect(options.urlAccess.role).toBe(MemberRole.Viewer);
    expect(options.urlAccess.expiration.getTime() - Date.now()).toBeGreaterThan(2 * 86400000 - 10000);
    stub.request.options.action = 'inspect'; stub.expire();
    const expired = await share(stub.ctx, stub.request);
    expect(expired.status).toBe('private'); expect(expired.receipt.directUrl).toBe(''); expect(stub.mutations()).toBe(1);
});
test('a file over the plan limit is not reported as a full drive', () => {
    const result = serviceFailure(new ValidationError('synthetic-private-provider-detail', 200003));
    expect(result.code).toBe('destination_file_too_large_for_plan');
    expect(result.message).toContain('Proton plan');
    expect(result.message).not.toContain('capacity');
    expect(JSON.stringify(result)).not.toContain('synthetic-private-provider-detail');
    expect(serviceFailure(new ValidationError('synthetic-private-provider-detail', 200001)).code).toBe('quota_exceeded');
});
test('service classification accepts typed account errors and never trusts raw exception properties', () => {
    for (const [httpCode, code] of [[401, 'authentication_required'], [403, 'permission_denied'], [429, 'rate_limited']] as const) {
        const result = serviceFailure(new AccountApiError('synthetic-private-provider-detail', { httpCode, debug: { secret: 'synthetic-secret' } }));
        expect(result.code).toBe(code); expect(result.details).toEqual({});
        expect(JSON.stringify(result)).not.toContain('synthetic-');
    }
    expect(serviceFailure({ statusCode: 401, code: 2011, message: 'synthetic-secret' }).code).toBe('operation_failed');
});
test('official account refresh handles expired access, revoked refresh and refresh rate limiting', async () => {
    const originalFetch = globalThis.fetch;
    try {
        for (const refreshStatus of [200, 401, 429]) {
            let token = 'synthetic-expired', signedOut = 0, saved = 0;
            const calls: string[] = [];
            const credentials = { uid: 'synthetic-uid', get accessToken() { return token; }, refreshToken: 'synthetic-refresh',
                on: () => {}, signOut: async () => { ++signedOut; token = ''; },
                setSessionInfo: async (value: { accessToken: string }) => { ++saved; token = value.accessToken; } };
            globalThis.fetch = (async (input: RequestInfo | URL) => {
                const request = input as Request;
                const refresh = new URL(request.url).pathname.includes('/auth/v4/refresh');
                calls.push(refresh ? 'refresh' : 'resource');
                if (refresh) return Response.json(refreshStatus === 200 ? { UID: 'synthetic-uid', AccessToken: 'synthetic-new', RefreshToken: 'synthetic-new-refresh' } : { Code: 0 }, { status: refreshStatus });
                return Response.json({}, { status: request.headers.get('Authorization') === 'Bearer synthetic-new' ? 200 : 401 });
            }) as typeof fetch;
            const logger = { debug() {}, info() {}, warn() {}, error() {} };
            const client = new ApiClient({ baseUrl: 'https://synthetic.invalid', appVersion: 'external-drive-put_gd@0.1.1-alpha', credentials: credentials as any, logger });
            const response = await client.authenticatedRequest('https://synthetic.invalid/drive/v2/test', { retry: 0, throwHttpErrors: false });
            expect(response.status).toBe(refreshStatus === 200 ? 200 : 401);
            expect(calls).toEqual(refreshStatus === 200 ? ['resource', 'refresh', 'resource'] : ['resource', 'refresh']);
            expect(saved).toBe(refreshStatus === 200 ? 1 : 0);
            expect(signedOut).toBe(refreshStatus === 401 ? 1 : 0);
            if (refreshStatus === 429) expect(token).toBe('synthetic-expired');
        }
    } finally { globalThis.fetch = originalFetch; }
});


test('revoked refresh cleared by account foundation still reports reconnect guidance', async () => {
    for (const operation of [checkName, stat]) {
        const stub = context(); let loggedIn = true;
        stub.ctx.auth.isLoggedIn = () => loggedIn;
        (stub.ctx.sdk as any).getMyFilesRootFolder = async () => {
            loggedIn = false; // The real account refresh path clears rejected credentials.
            throw new ServerError('synthetic-private-provider-detail');
        };
        const result: JsonObject = await operation(stub.ctx, statRequest());
        expect(result.status).toBe(operation === stat ? 'unknown' : 'inconclusive');
        expect(result.error.code).toBe('authentication_required');
    }
    const stub = sharingContext(); let loggedIn = true;
    stub.ctx.auth.isLoggedIn = () => loggedIn;
    (stub.ctx.sdk as any).shareNode = async () => {
        loggedIn = false;
        throw new ServerError('synthetic-private-provider-detail');
    };
    const result = await share(stub.ctx, stub.request);
    expect(result.status).toBe('unknown'); expect(result.error.code).toBe('authentication_required');
    expect(result.receipt.directUrl).toBe('');
});

test('pinned SDK honors Retry-After before recovery and cancellation stops repeated 429 requests', async () => {
    const { DriveAPIService } = await import('@protontech/drive-sdk/internal/apiService/apiService');
    const logger = { debug() {}, info() {}, warn() {}, error() {} };
    const telemetry = { getLogger: () => logger, recordMetric() {} };
    const calls: number[] = [];
    const client = new DriveAPIService(telemetry as any, {} as any, {
        fetchJson: async () => {
            calls.push(Date.now());
            return calls.length < 3
                ? Response.json({ Code: 0 }, { status: 429, headers: { 'retry-after': '1' } })
                : Response.json({ Code: 1000 });
        },
    } as any, 'https://synthetic.invalid', 'en');
    const recovered = await client.get('drive/v2/synthetic') as { Code: number };
    expect(recovered.Code).toBe(1000);
    expect(calls.length).toBe(3);
    expect(calls[1] - calls[0]).toBeGreaterThanOrEqual(950);
    expect(calls[2] - calls[1]).toBeGreaterThanOrEqual(950);

    const cancellation = new AbortController(); let attempts = 0;
    const limited = new DriveAPIService(telemetry as any, {} as any, {
        fetchJson: async () => {
            ++attempts; cancellation.abort();
            return Response.json({ Code: 0 }, { status: 429, headers: { 'retry-after': '1' } });
        },
    } as any, 'https://synthetic.invalid', 'en');
    await expect(limited.get('drive/v2/synthetic', cancellation.signal)).rejects.toThrow('Request aborted');
    expect(attempts).toBe(1);
});

test('a refused keychain session write reports keychain_unavailable without provider detail (#252)', async () => {
    const refused = () => Promise.reject(new Error('Object does not exist at path "/org/freedesktop/secrets/collection/login"'));
    // The official credentials object saves once both the key password and the session exist, as Connect does.
    const credentials = new Credentials({ load: async () => null, remove: async () => {},
        save: () => saveToKeychain(refused) }, { debug() {}, info() {}, warn() {}, error() {} } as any);
    await credentials.setUserKeyPassword('synthetic-key-password');
    const error = await credentials.setSessionInfo({ uid: 'synthetic-uid', accessToken: 'synthetic', refreshToken: 'synthetic' }).catch(e => e);
    expect(error).toBeInstanceOf(ProtocolError);
    for (const fallback of ['operation_failed', 'operation_unknown']) {
        const result = serviceFailure(error, fallback);
        expect(result.code).toBe('keychain_unavailable');
        expect(JSON.stringify(result)).not.toContain('/org/freedesktop');
    }
    let saved = 0;
    await saveToKeychain(async () => { ++saved; });
    expect(saved).toBe(1);
});

test('a session shares only good My files and root share answers between its clients', async () => {
    const calls: string[] = [];
    let status = 200, myFiles: unknown = { Share: { ShareID: 'root-share' } }, hold: Promise<void> | undefined;
    const inner = {
        fetchBlob: async () => new Response('blob'),
        fetchJson: async (options: { url: string; method: string; signal?: AbortSignal }) => {
            const path = new URL(options.url).pathname; calls.push(options.method + ' ' + path + (options.signal ? ' signalled' : ''));
            const serial = calls.length;
            if (path.endsWith('/root-share')) await hold;
            const body = path.endsWith('/my-files') ? myFiles : { path, serial };
            return new Response(JSON.stringify(body), { status, headers: { 'x-synthetic': String(serial) } });
        },
    };
    const reads = new SharedAccountReads(inner as any);
    const signal = new AbortController().signal;
    const get = (path: string, method = 'GET') => reads.fetchJson({ url: 'https://synthetic.invalid/' + path, method, signal } as any);
    // Before My files names the root share, no share answer is kept: any of them may change.
    await get('drive/shares/root-share'); await get('drive/shares/root-share');
    expect(calls.length).toBe(2);
    // A refused or malformed My files answer is passed on and asked again.
    status = 500; expect((await get('drive/v2/shares/my-files')).status).toBe(500);
    status = 200; myFiles = { Share: {} }; expect(await (await get('drive/v2/shares/my-files')).json()).toEqual({ Share: {} });
    expect(calls.length).toBe(4);
    // A good one is kept, and the root share it names is read at once, free of the request's signal.
    myFiles = { Share: { ShareID: 'root-share' } };
    let release = () => {}; hold = new Promise(resolve => { release = resolve; });
    const first = await get('drive/v2/shares/my-files'), second = await get('drive/v2/shares/my-files');
    expect(calls.slice(4)).toEqual(['GET /drive/v2/shares/my-files signalled', 'GET /drive/shares/root-share']);
    expect(await second.json()).toEqual(await first.json()); expect(second.headers.get('x-synthetic')).toBe('5');
    // A client that asks while that read is under way waits for it instead of asking again.
    const waiting = get('drive/shares/root-share'); release();
    const root = await (await waiting).json();
    expect(root).toEqual({ path: '/drive/shares/root-share', serial: 6 });
    expect(await (await get('drive/shares/root-share')).json()).toEqual(root);
    expect(calls.length).toBe(6);
    // Another share, another method and every other read still reach the service each time.
    for (let i = 0; i < 2; i++) { await get('drive/shares/file-share'); await get('drive/shares/root-share', 'DELETE'); await get('drive/v2/volumes/v/links', 'POST'); }
    expect(calls.length).toBe(12);
});

function srpStub() {
    const calls: string[] = [];
    let salts = 0, failing = false;
    const inner = {
        generateKeySalt: () => { calls.push('salt'); return 'salt-' + ++salts; },
        computeKeyPassword: async (password: string, salt: string) => { calls.push('key ' + password + ' ' + salt); if (failing) throw new Error('synthetic'); return 'key(' + password + ',' + salt + ')'; },
        getSrpVerifier: async (password: string) => { calls.push('verifier ' + password); if (failing) throw new Error('synthetic'); return { modulusId: 'm', version: 4, salt: 's', verifier: 'v(' + password + ')' }; },
        getSrp: async () => { calls.push('srp'); return { expectedServerProof: '', clientProof: '', clientEphemeral: '' }; },
    };
    return { inner, calls, fail: (value: boolean) => { failing = value; } };
}
test('a prepared link answers the SDK for exactly the password and salt it hashed, once', async () => {
    const stub = srpStub();
    let now = 0;
    const link = new LinkAhead(stub.inner as any, () => now);
    link.prepare('drawn');
    expect(stub.calls).toEqual(['salt', 'key drawn salt-1', 'verifier drawn']);
    // Before the SDK takes the password, every call is the account code's own.
    expect(link.generateKeySalt()).toBe('salt-2');
    expect(await link.computeKeyPassword('drawn', 'salt-1')).toBe('key(drawn,salt-1)');
    expect(stub.calls.length).toBe(5);
    expect(link.take()).toBe('drawn');
    // The salt it was hashed with is given once, and the two answers are not computed again.
    expect(link.generateKeySalt()).toBe('salt-1'); expect(link.generateKeySalt()).toBe('salt-3');
    expect(await link.computeKeyPassword('drawn', 'salt-1')).toBe('key(drawn,salt-1)');
    expect((await link.getSrpVerifier('drawn')).verifier).toBe('v(drawn)');
    expect(stub.calls.length).toBe(6);
    // A password with the user's own added, or another salt, is hashed in line.
    expect(await link.computeKeyPassword('drawn-own', 'salt-1')).toBe('key(drawn-own,salt-1)');
    expect(await link.computeKeyPassword('drawn', 'salt-9')).toBe('key(drawn,salt-9)');
    expect((await link.getSrpVerifier('drawn-own')).verifier).toBe('v(drawn-own)');
    expect(stub.calls.length).toBe(9);
    // It is handed over once, and nothing of it is left after `drop`.
    expect(link.take()).toBeUndefined(); expect(link.prepared).toBe(false);
    link.prepare('second'); link.drop();
    expect(link.take()).toBeUndefined();
    expect((await link.getSrpVerifier('second')).verifier).toBe('v(second)');
});
test('a prepared link is not used once its modulus is old, and a failed preparation is made again in line', async () => {
    const stub = srpStub();
    let now = 0;
    const link = new LinkAhead(stub.inner as any, () => now);
    link.prepare('old'); now = LINK_AHEAD_MS + 1;
    expect(link.take()).toBeUndefined(); expect(link.prepared).toBe(false);
    stub.fail(true); link.prepare('failed'); await Promise.resolve();
    stub.fail(false); now += 1;
    expect(link.take()).toBe('failed');
    const salt = link.generateKeySalt();
    expect(await link.computeKeyPassword('failed', salt)).toBe('key(failed,' + salt + ')');
    expect((await link.getSrpVerifier('failed')).verifier).toBe('v(failed)');
});
test('the share session key is wrapped ahead for the prepared password only, and only that wrap is reused', async () => {
    const stub = srpStub();
    const link = new LinkAhead(stub.inner as any, () => 0);
    const wraps: string[] = [];
    const wrap = (key: any, password: string) => { wraps.push(key.id + ' ' + password); return Promise.resolve({ keyPacket: new Uint8Array([wraps.length]) }); };
    const inline = (key: any, password: string) => () => wrap(key, password);
    const share = { id: 'share-key' } as any, other = { id: 'other-key' } as any;
    // Nothing prepared: no key is remembered and nothing is wrapped ahead.
    link.sessionKeyMade(share); link.wrapAhead(wrap);
    expect(wraps).toEqual([]);
    link.prepare('drawn'); link.sessionKeyMade(other); link.sessionKeyMade(share); link.wrapAhead(wrap);
    await new Promise(resolve => setTimeout(resolve, 0));
    expect(wraps).toEqual(['share-key key(drawn,salt-1)']);
    // A key made after the wrap began does not replace the one being wrapped.
    link.sessionKeyMade(other); link.wrapAhead(wrap);
    // Before the SDK has taken the password the wrap is not handed out.
    expect((await link.wrapped(share, 'key(drawn,salt-1)', inline(share, 'key(drawn,salt-1)'))).keyPacket[0]).toBe(2);
    link.take();
    expect((await link.wrapped(share, 'key(drawn,salt-1)', inline(share, 'unused'))).keyPacket[0]).toBe(1);
    expect((await link.wrapped(other, 'key(drawn,salt-1)', inline(other, 'key(drawn,salt-1)'))).keyPacket[0]).toBe(3);
    expect((await link.wrapped(share, 'another', inline(share, 'another'))).keyPacket[0]).toBe(4);
    expect(wraps.length).toBe(4);
});
test('the OpenPGP module reports session keys and asks for the wrap made ahead', async () => {
    const stub = srpStub();
    const link = new LinkAhead(stub.inner as any, () => 0);
    const asked: string[] = [];
    const proxy = { generateSessionKey: async () => ({ data: new Uint8Array(32), algorithm: 'aes256' }),
        encryptSessionKey: async (options: { passwords?: string[] }) => { asked.push(options.passwords?.[0] ?? ''); return new Uint8Array([asked.length]); } };
    const pgp = new LinkOpenPGP(proxy as any, link);
    link.prepare('drawn');
    const key = await pgp.generateSessionKey([], { enableAeadWithEncryptionKeys: false });
    pgp.wrapAhead(); await new Promise(resolve => setTimeout(resolve, 0));
    expect(asked).toEqual(['key(drawn,salt-1)']);
    link.take();
    expect((await pgp.encryptSessionKeyWithPassword(key as any, 'key(drawn,salt-1)')).keyPacket[0]).toBe(1);
    expect((await pgp.encryptSessionKeyWithPassword(key as any, 'another')).keyPacket[0]).toBe(2);
    expect(asked).toEqual(['key(drawn,salt-1)', 'another']);
});
test('the pinned SDK draws a link password where the helper takes it over', async () => {
    const sharing = initSharingModule({ getLogger: () => ({}) } as any, {} as any, {} as any, {} as any, {} as any, {} as any, {} as any);
    const stub = srpStub();
    const link = new LinkAhead(stub.inner as any, () => 0);
    const draw = linkPasswordAhead({ sharing }, link);
    expect(draw).toBeDefined();
    const prepare = async () => { link.prepare(await draw!()); };
    const generator = (sharing.management as any).cryptoService;
    // Unprepared, the SDK's own generator answers: twelve characters of its alphabet.
    const plain = await generator.generatePublicLinkPassword();
    expect(plain).toMatch(/^[A-Za-z0-9]{12}$/); expect(link.prepared).toBe(false);
    await prepare();
    const drawn = stub.calls.find(call => call.startsWith('verifier '))!.slice('verifier '.length);
    expect(drawn).toMatch(/^[A-Za-z0-9]{12}$/); expect(drawn).not.toBe(plain);
    expect(await generator.generatePublicLinkPassword()).toBe(drawn);
    expect(await generator.generatePublicLinkPassword()).not.toBe(drawn);
    // An SDK without the generator there leaves links unprepared instead of failing.
    expect(linkPasswordAhead({ sharing: { management: {} } }, link)).toBeUndefined();
    expect(linkPasswordAhead({}, link)).toBeUndefined();
});
test('a link preparation ended before it began, or before its password was drawn, leaves nothing', async () => {
    const stub = srpStub();
    const link = new LinkAhead(stub.inner as any, () => 0);
    const pause = (ms: number) => new Promise(resolve => setTimeout(resolve, ms));
    let draws = 0, release: (password: string) => void = () => {};
    const draw = () => { ++draws; return new Promise<string>(resolve => { release = resolve; }); };
    const account = linkPreparation(link, 10);
    const preparation = { start: () => account.start(draw), unused: account.unused };
    // Ended before its start came due: no password is drawn.
    preparation.start(); preparation.unused();
    await pause(30);
    expect(draws).toBe(0); expect(link.prepared).toBe(false);
    // Ended while the password was being drawn: it is not hashed or kept.
    preparation.start(); await pause(30);
    expect(draws).toBe(1);
    preparation.unused(); release('late'); await pause(5);
    expect(link.prepared).toBe(false); expect(stub.calls).toEqual([]);
    // A start made after an earlier one supersedes it, and only its password is prepared.
    preparation.start(); await pause(30);
    const first = release;
    preparation.start(); await pause(30);
    first('superseded'); release('current'); await pause(5);
    expect(link.prepared).toBe(true); expect(link.take()).toBe('current');
    expect(stub.calls.filter(call => call.startsWith('verifier '))).toEqual(['verifier current']);
    // A draw that fails prepares nothing, and an SDK without the generator starts nothing.
    preparation.unused();
    account.start(() => Promise.reject(new Error('synthetic'))); await pause(30);
    expect(link.prepared).toBe(false);
    account.start(undefined); await pause(30);
    expect(link.prepared).toBe(false);
    // Another client's cleanup ends a preparation the first client started: there is one per account.
    const other = { unused: account.unused };
    preparation.start(); other.unused(); await pause(30);
    expect(draws).toBe(3); expect(link.prepared).toBe(false);
});
test('transfers and share creation are reported to their listeners, and nothing else is', async () => {
    const inner = { fetchBlob: async () => new Response('blob'), fetchJson: async () => new Response('{}') };
    const reads = new SharedAccountReads(inner as any);
    const seen: string[] = [];
    reads.onTransfer = host => { seen.push('transfer ' + host); };
    reads.onShareCreate = () => { seen.push('share'); };
    await reads.fetchBlob({ url: 'https://fra-storage.proton.me/storage/blocks', method: 'POST', body: new Blob() } as any);
    await reads.fetchBlob({ url: 'https://fra-storage.proton.me/storage/blocks/x', method: 'GET' } as any);
    await reads.fetchJson({ url: 'https://drive-api.proton.me/drive/v2/volumes/v/files/small', method: 'POST', body: new FormData() } as any);
    await reads.fetchJson({ url: 'https://drive-api.proton.me/drive/v2/volumes/v/files', method: 'POST', json: {} } as any);
    await reads.fetchJson({ url: 'https://drive-api.proton.me/drive/volumes/v/shares', method: 'POST', json: {} } as any);
    await reads.fetchJson({ url: 'https://drive-api.proton.me/drive/volumes/v/shares', method: 'GET' } as any);
    await reads.fetchJson({ url: 'https://drive-api.proton.me/drive/shares/s/urls', method: 'POST', json: {} } as any);
    expect(seen).toEqual(['transfer fra-storage.proton.me', 'transfer drive-api.proton.me', 'share']);
    // A listener that throws never fails the transfer it heard of.
    reads.onTransfer = () => { throw new Error('synthetic'); };
    expect(await (await reads.fetchBlob({ url: 'https://fra-storage.proton.me/storage/blocks', method: 'POST' } as any)).text()).toBe('blob');
});
test('only a Proton storage host is named as a transfer host or accepted as a hint', () => {
    expect(transferHost('fra-storage.proton.me')).toBe('fra-storage.proton.me');
    for (const value of ['drive-api.proton.me', 'proton.me', 'storage.proton.me.example.org', 'a.b.proton.me', 'FRA-storage.proton.me',
        'fra-storage.proton.me:8443', 'user@fra-storage.proton.me', 'fra-storage.proton.me/path', '', undefined, 7, null])
        expect(transferHost(value)).toBeUndefined();
});
async function uploadWith(fields: JsonObject, transfers: string[], bytes = 65536) {
    const directory = await mkdtemp(join(tmpdir(), 'putgd-link-ahead-'));
    const path = join(directory, 'synthetic.bin'), data = Buffer.alloc(bytes, 112);
    await writeFile(path, data, { mode: 0o600 });
    const fetched: string[] = [], direct = globalThis.fetch;
    (globalThis as any).fetch = async (url: string) => { fetched.push(url); return new Response(''); };
    try {
        const stub = context();
        (stub.ctx.sdk as any).getFileUploader = async () => ({
            uploadFromStream: async (stream: ReadableStream) => {
                const reader = stream.getReader();
                while (!(await reader.read()).done) {}
                for (const host of transfers) stub.transfer(host);
                return { completion: async () => ({ nodeUid: 'file-node', nodeRevisionUid: 'file-revision' }) };
            },
        });
        const result = await upload(stub.ctx, { ...statRequest(), method: 'upload', condition: 'absent', ...fields,
            artifact: { path, bytes: data.length, mime: 'application/octet-stream', sha256: createHash('sha256').update(data).digest('hex') } });
        return { result, calls: stub.calls.map(call => call.method), fetched, watching: stub.watching() };
    } finally { (globalThis as any).fetch = direct; await rm(directory, { recursive: true, force: true }); }
}
test('an upload a link follows prepares that link once, when its bytes go out', async () => {
    const followed = await uploadWith({ linkFollows: true }, ['drive-api.proton.me', 'drive-api.proton.me']);
    expect(followed.result.status).toBe('uploaded');
    expect(followed.calls.filter(call => call === 'prepare-link').length).toBe(1);
    expect(followed.watching).toBe(false);
    // The single-request upload goes to the API host: no storage host is named.
    expect(followed.result.transferHost).toBeUndefined();
    const alone = await uploadWith({}, ['drive-api.proton.me']);
    expect(alone.calls).not.toContain('prepare-link');
    const stub = statRequest();
    const withPassword = await uploadWith({ linkFollows: true, destination: { ...stub.destination,
        settings: { ...stub.destination.settings, sharePasswordReference: 'putgd/share-password' } } }, ['drive-api.proton.me']);
    expect(withPassword.calls).not.toContain('prepare-link');
});
test('a block upload names its storage host, and opens a hinted one before its bytes are ready', async () => {
    const big = 200 * 1024;
    const first = await uploadWith({ linkFollows: true }, ['fra-storage.proton.me'], big);
    expect(first.result.transferHost).toBe('fra-storage.proton.me'); expect(first.fetched).toEqual([]);
    expect(first.calls.filter(call => call === 'prepare-link').length).toBe(1);
    const hinted = await uploadWith({ transferHostHint: 'fra-storage.proton.me' }, ['fra-storage.proton.me'], big);
    expect(hinted.fetched).toEqual(['https://fra-storage.proton.me/']);
    // A hint that is not a Proton storage host is ignored, and a small file opens nothing.
    expect((await uploadWith({ transferHostHint: 'storage.example.org' }, ['fra-storage.proton.me'], big)).fetched).toEqual([]);
    expect((await uploadWith({ transferHostHint: 'drive-api.proton.me' }, ['fra-storage.proton.me'], big)).fetched).toEqual([]);
    expect((await uploadWith({ transferHostHint: 'fra-storage.proton.me' }, ['drive-api.proton.me'])).fetched).toEqual([]);
    // A file of more than one block opens nothing: its blocks start together.
    expect((await uploadWith({ transferHostHint: 'fra-storage.proton.me' }, ['fra-storage.proton.me'], 4 * 1024 * 1024 + 1)).fetched).toEqual([]);
    expect((await uploadWith({ transferHostHint: 'fra-storage.proton.me' }, ['fra-storage.proton.me'], 4 * 1024 * 1024)).fetched).toEqual(['https://fra-storage.proton.me/']);
    // A host the service names that is not Proton's is not passed on.
    expect((await uploadWith({}, ['storage.example.org'], big)).result.transferHost).toBeUndefined();
});
test('a failed upload ends its link preparation', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'putgd-link-failed-'));
    const path = join(directory, 'synthetic.bin'), data = Buffer.alloc(32, 112);
    await writeFile(path, data, { mode: 0o600 });
    try {
        const stub = context();
        (stub.ctx.sdk as any).getFileUploader = async () => ({
            uploadFromStream: async () => { stub.transfer('drive-api.proton.me'); throw new ServerError('synthetic'); },
        });
        const result = await upload(stub.ctx, { ...statRequest(), method: 'upload', condition: 'absent', linkFollows: true,
            artifact: { path, bytes: data.length, mime: 'application/octet-stream', sha256: createHash('sha256').update(data).digest('hex') } });
        expect(result.status).toBe('unknown');
        const calls = stub.calls.map(call => call.method);
        expect(calls.indexOf('link-unused')).toBeGreaterThan(calls.indexOf('prepare-link'));
        expect(stub.watching()).toBe(false);
    } finally { await rm(directory, { recursive: true, force: true }); }
});
test('a link prepared ahead reads no modulus of its own, and its preparation ends with the request', async () => {
    const prepared = sharingContext(); prepared.prepared(true);
    expect((await share(prepared.ctx, prepared.request)).status).toBe('shared');
    const calls = prepared.calls.map(call => call.method === 'link-being-made' ? 'making ' + call.args[0] : call.method);
    expect(calls).not.toContain('modulus');
    expect(calls.indexOf('making true')).toBeLessThan(calls.indexOf('share'));
    expect(calls.slice(calls.indexOf('share')).filter(call => call === 'making false' || call === 'link-unused')).toEqual(['making false', 'link-unused']);
    // Unprepared, the modulus is still read beside the share's creation, and both reads of the file are made.
    const plain = sharingContext();
    expect((await share(plain.ctx, plain.request)).status).toBe('shared');
    expect(plain.calls.map(call => call.method)).toContain('modulus'); expect(plain.reads()).toBe(2);
    expect(prepared.reads()).toBe(2);
});
test('a token refresh keeps the user data load of the same session, and of no other', async () => {
    const listeners: (() => void)[] = [];
    let uid: string | undefined = 'session', loggedIn = true;
    const credentials = { on: (_: 'sessionInfoChanged', callback: () => void) => { listeners.push(callback); },
        isLoggedIn: () => loggedIn, get uid() { return uid; }, getUserKeyPassword: () => 'synthetic' };
    const attach = keepUserDataAcrossRefresh(credentials);
    let users = 0;
    const api = { users: () => { ++users; return new Promise(() => {}); }, addresses: async () => ({ Addresses: [] }) };
    // The pinned account code keeps its load in `userDataPromise` and forgets it on every session change.
    const addresses = new Addresses(api as any, credentials as any, {} as any, { debug: () => {}, error: () => {} } as any);
    attach(addresses);
    const changed = () => { for (const listener of listeners) listener(); };
    void addresses.getOwnPrimaryAddress().catch(() => {});
    await Promise.resolve();
    const loading = (addresses as any).userDataPromise;
    expect(loading).toBeInstanceOf(Promise); expect(users).toBe(1);
    changed();
    expect((addresses as any).userDataPromise).toBe(loading);
    void addresses.getOwnPrimaryAddress().catch(() => {}); await Promise.resolve();
    expect(users).toBe(1);
    // Another account's session, or a signed-out one, keeps nothing.
    uid = 'another'; changed();
    expect((addresses as any).userDataPromise).toBeUndefined();
    void addresses.getOwnPrimaryAddress().catch(() => {}); await Promise.resolve();
    expect(users).toBe(2);
    loggedIn = false; changed();
    expect((addresses as any).userDataPromise).toBeUndefined();
});

// A synthetic service: every connection has a number, and answers after its own delay.
type Seen = { connection: number; method: string; path: string; body: string; headers: http2.IncomingHttpHeaders };
const servers: http2.Http2Server[] = [];
afterEach(() => { for (const server of servers.splice(0)) server.close(); });
async function service(delay: (connection: number) => number, status: (seen: Seen) => number = () => 200) {
    const seen: Seen[] = [], sessions: http2.ServerHttp2Session[] = [];
    const server = http2.createServer();
    servers.push(server);
    server.on('session', session => { sessions.push(session); session.on('error', () => {}); });
    server.on('stream', (stream, headers) => {
        const connection = sessions.indexOf(stream.session as http2.ServerHttp2Session) + 1, path = String(headers[':path']);
        stream.on('error', () => {});
        // A stream that ends without an answer.
        if (path === '/lost') { stream.close(http2.constants.NGHTTP2_INTERNAL_ERROR); return; }
        let body = '';
        stream.on('data', chunk => { body += chunk; });
        stream.on('end', () => {
            const request = { connection, method: String(headers[':method']), path, body, headers };
            seen.push(request);
            setTimeout(() => {
                const out = stream as http2.ServerHttp2Stream;
                if (out.destroyed || out.closed) return;
                const code = status(request), answer = JSON.stringify({ connection, path });
                if (path === '/gzip') { out.respond({ ':status': code, 'content-encoding': 'gzip', 'x-connection': String(connection) }); out.end(zlib.gzipSync(answer)); return; }
                if (path === '/coded') { out.respond({ ':status': code, 'content-encoding': 'synthetic' }); out.end(answer); return; }
                out.respond({ ':status': code, 'content-type': 'application/json', 'x-connection': String(connection) });
                out.end(code === 204 ? undefined : answer);
            }, delay(connection));
        });
    });
    await new Promise<void>(resolve => server.listen(0, '127.0.0.1', resolve));
    const origin = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
    return { origin, seen, sessions, open: () => sessions.filter(session => !session.closed && !session.destroyed).length };
}
function directStub() {
    const calls: string[] = [];
    const direct = (async (input: Parameters<typeof fetch>[0], init?: Parameters<typeof fetch>[1]) => {
        const request = new Request(input as RequestInfo, init);
        calls.push(request.method + ' ' + new URL(request.url).pathname + (request.body ? ' ' + await request.text() : ''));
        return new Response('direct');
    }) as typeof fetch;
    return { direct, calls };
}
const settle = (ms = 60) => new Promise(resolve => setTimeout(resolve, ms));
const connectionOf = (response: Response) => Number(response.headers.get('x-connection'));

test('the first read is sent on every candidate, and the connection that answers first carries the rest', async () => {
    const backend = await service(connection => connection === 3 ? 5 : 80);
    const { direct, calls } = directStub();
    const api = fastestConnectionFetch(backend.origin, direct, { candidates: 4 });
    // Requests made while the choice is under way wait for it instead of opening connections of their own.
    const [first, beside] = await Promise.all([api(backend.origin + '/core/v4/addresses?Page=0', { headers: { 'x-pm-uid': 'synthetic' } }),
        api(backend.origin + '/core/v4/users')]);
    expect(first.status).toBe(200); expect(connectionOf(first)).toBe(3);
    expect(await first.json()).toEqual({ connection: 3, path: '/core/v4/addresses?Page=0' });
    expect(connectionOf(beside)).toBe(3);
    await settle();
    // The other candidates are closed. Only the read was ever sent more than once.
    expect(backend.open()).toBe(1); expect(backend.sessions.length).toBe(4);
    expect(backend.seen.filter(request => request.path.startsWith('/core/v4/addresses')).every(request => request.method === 'GET')).toBe(true);
    // Every later request, with its body and fields, is a stream on the kept connection.
    const posted = await api(backend.origin + '/drive/v2/volumes/v/links', { method: 'POST', body: JSON.stringify({ LinkIDs: ['a'] }),
        headers: { 'content-type': 'application/json', authorization: 'Bearer synthetic' } });
    expect(connectionOf(posted)).toBe(3);
    const sent = backend.seen.find(request => request.method === 'POST')!;
    expect(sent.body).toBe('{"LinkIDs":["a"]}'); expect(sent.headers.authorization).toBe('Bearer synthetic');
    expect(sent.headers['content-type']).toBe('application/json'); expect(sent.headers['accept-encoding']).toBe('gzip');
    const form = new FormData(); form.set('Metadata', new Blob(['{}'], { type: 'application/json' }), 'Metadata'); form.set('ContentBlock', new Blob([new Uint8Array(200000)]), 'ContentBlock');
    expect(connectionOf(await api(backend.origin + '/drive/v2/volumes/v/files/small', { method: 'POST', body: form }))).toBe(3);
    const multipart = backend.seen.at(-1)!;
    expect(String(multipart.headers['content-type'])).toStartWith('multipart/form-data; boundary='); expect(multipart.body.length).toBeGreaterThan(200000);
    // A compressed answer is read as the ordinary client reads it, and an empty one stays empty.
    const packed = await api(backend.origin + '/gzip');
    expect(await packed.json()).toEqual({ connection: 3, path: '/gzip' }); expect(packed.headers.get('content-encoding')).toBeNull();
    expect(backend.sessions.length).toBe(4); expect(calls).toEqual([]);
});
test('requests to other origins and a first request that is not a read go to the ordinary client', async () => {
    const backend = await service(() => 5);
    const { direct, calls } = directStub();
    const api = fastestConnectionFetch(backend.origin, direct, { candidates: 3 });
    expect(await (await api('https://fra-storage.proton.me/storage/blocks', { method: 'POST', body: 'block' })).text()).toBe('direct');
    // It is handed on as it came: the very request object, never read or copied here.
    const handed: unknown[] = [];
    const passing = fastestConnectionFetch(backend.origin, (async (input: unknown, init?: unknown) => { handed.push(input, init); return new Response('direct'); }) as typeof fetch);
    const block = new Request('https://zrh-storage.proton.me/storage/blocks', { method: 'POST', body: new Uint8Array(1024) }), options = { keepalive: true };
    await passing(block, options);
    expect(handed[0]).toBe(block); expect(handed[1]).toBe(options); expect(block.bodyUsed).toBe(false);
    // A request that changes something is never sent more than once: it chooses nothing.
    expect(await (await api(backend.origin + '/auth/v4/refresh', { method: 'POST', body: '{}' })).text()).toBe('direct');
    expect(backend.sessions.length).toBe(0);
    expect(calls).toEqual(['POST /storage/blocks block', 'POST /auth/v4/refresh {}']);
    expect(connectionOf(await api(backend.origin + '/core/v4/users'))).toBeGreaterThan(0);
    expect(connectionOf(await api(backend.origin + '/auth/v4/refresh', { method: 'POST', body: '{}' }))).toBeGreaterThan(0);
    expect(calls.length).toBe(2);
});
test('an expired token still chooses a connection, and later answers are returned as the service gave them', async () => {
    const backend = await service(connection => connection === 1 ? 5 : 80, seen => seen.path === '/expired' ? 401 : seen.path === '/empty' ? 204 : seen.path === '/missing' ? 404 : 200);
    const api = fastestConnectionFetch(backend.origin, directStub().direct, { candidates: 2 });
    const refused = await api(backend.origin + '/expired');
    expect(refused.status).toBe(401); expect(refused.ok).toBe(false);
    const empty = await api(backend.origin + '/empty');
    expect(empty.status).toBe(204); expect(await empty.text()).toBe('');
    expect(connectionOf(empty)).toBe(connectionOf(refused));
    expect((await api(backend.origin + '/missing')).status).toBe(404);
});
test('the ordinary client takes over for good when no candidate connects, answers in time or answers as the API does', async () => {
    const { direct, calls } = directStub();
    // Nothing listens there.
    const refused = fastestConnectionFetch('http://127.0.0.1:9', direct, { candidates: 3 });
    expect(await (await refused('http://127.0.0.1:9/core/v4/users')).text()).toBe('direct');
    expect(await (await refused('http://127.0.0.1:9/core/v4/users')).text()).toBe('direct');
    const slow = await service(() => 400);
    const late = fastestConnectionFetch(slow.origin, direct, { candidates: 2, chooseWithinMs: 50 });
    expect(await (await late(slow.origin + '/core/v4/users')).text()).toBe('direct');
    await settle();
    expect(slow.open()).toBe(0);
    expect(await (await late(slow.origin + '/core/v4/users')).text()).toBe('direct');
    // An answer that is neither a success nor an expired token chooses nothing: the service
    // may not want these connections, and the ordinary client is asked the same read.
    const changed = await service(() => 5, () => 503);
    const unwelcome = fastestConnectionFetch(changed.origin, direct, { candidates: 2 });
    expect(await (await unwelcome(changed.origin + '/core/v4/users')).text()).toBe('direct');
    expect(await (await unwelcome(changed.origin + '/drive/v2/volumes/v/links', { method: 'POST', body: '{}' })).text()).toBe('direct');
    await settle();
    expect(changed.open()).toBe(0); expect(calls.length).toBe(6);
    // So does an answer in a coding the helper cannot read.
    const coded = await service(() => 5);
    const unreadable = fastestConnectionFetch(coded.origin, direct, { candidates: 2 });
    expect(await (await unreadable(coded.origin + '/coded')).text()).toBe('direct');
});
test('a failed request retires the kept connection, and is sent again only when that is safe', async () => {
    const { direct, calls } = directStub();
    const start = async () => {
        const backend = await service(() => 5);
        const api = fastestConnectionFetch(backend.origin, direct, { candidates: 2 });
        expect(connectionOf(await api(backend.origin + '/core/v4/users'))).toBeGreaterThan(0);
        return { backend, api };
    };
    // A read that fails is asked of the ordinary client, as is everything after it.
    let { backend, api } = await start();
    expect(await (await api(backend.origin + '/lost')).text()).toBe('direct');
    expect(await (await api(backend.origin + '/core/v4/users')).text()).toBe('direct');
    expect(calls).toEqual(['GET /lost', 'GET /core/v4/users']);
    // A change lost after it was sent may have happened: it fails as a lost connection does, and is not repeated.
    ({ backend, api } = await start());
    const before = calls.length;
    let failure: unknown;
    try { await api(backend.origin + '/lost', { method: 'POST', body: '{}' }); } catch (error) { failure = error; }
    expect(failure).toBeInstanceOf(TypeError); expect((failure as Error).message).toBe('fetch failed');
    expect(calls.length).toBe(before);
    expect(await (await api(backend.origin + '/core/v4/users')).text()).toBe('direct');
    // A connection the service closed carries nothing more.
    ({ backend, api } = await start());
    await settle();
    for (const session of backend.sessions) session.destroy();
    await settle();
    expect(await (await api(backend.origin + '/drive/v2/volumes/v/links', { method: 'POST', body: '{}' })).text()).toBe('direct');
});
test('a first connection that is not on a fast backend is replaced by one that is', async () => {
    // The first round's two connections answer slowly; the second round's fourth connection is fast.
    const backend = await service(connection => connection === 4 ? 5 : SLOW_ANSWER_MS + 80);
    const { direct, calls } = directStub();
    const api = fastestConnectionFetch(backend.origin, direct, { candidates: 2 });
    const first = await api(backend.origin + '/core/v4/users');
    expect([1, 2]).toContain(connectionOf(first));
    // Requests go on with the first connection while a better one is looked for.
    expect(connectionOf(await api(backend.origin + '/drive/v2/shares/my-files'))).toBe(connectionOf(first));
    await settle(SLOW_ANSWER_MS + 250);
    expect(connectionOf(await api(backend.origin + '/drive/v2/shares/my-files'))).toBe(4);
    await settle();
    expect(backend.open()).toBe(1); expect(backend.sessions.length).toBe(4); expect(calls).toEqual([]);
    // When the second round finds nothing faster, the first connection stays.
    const even = await service(() => SLOW_ANSWER_MS + 80);
    const same = fastestConnectionFetch(even.origin, direct, { candidates: 2 });
    const kept = connectionOf(await same(even.origin + '/core/v4/users'));
    await settle(2 * SLOW_ANSWER_MS + 350);
    expect(connectionOf(await same(even.origin + '/core/v4/users'))).toBe(kept);
    expect(even.open()).toBe(1); expect(even.sessions.length).toBe(4);
    // A fast first connection looks no further.
    const quick = await service(() => 5);
    const fine = fastestConnectionFetch(quick.origin, direct, { candidates: 2 });
    await fine(quick.origin + '/core/v4/users'); await settle(150);
    expect(quick.sessions.length).toBe(2);
});
test('a cancelled request ends its stream and rejects as the ordinary client would', async () => {
    const slow = await service(() => 300);
    const waiting = fastestConnectionFetch(slow.origin, directStub().direct, { candidates: 1 });
    await waiting(slow.origin + '/core/v4/users');
    const controller = new AbortController();
    const pending = waiting(slow.origin + '/drive/v2/volumes/v/links', { method: 'POST', body: '{}', signal: controller.signal });
    setTimeout(() => controller.abort(), 30);
    let failure: unknown;
    try { await pending; } catch (error) { failure = error; }
    expect((failure as Error)?.name).toBe('AbortError');
    // The connection outlives the cancelled request.
    expect((await waiting(slow.origin + '/core/v4/users')).status).toBe(200);
    const already = new AbortController(); already.abort();
    let refused: unknown;
    try { await waiting(slow.origin + '/core/v4/users', { signal: already.signal }); } catch (error) { refused = error; }
    expect((refused as Error)?.name).toBe('AbortError');
});
test('a read cancelled while the choice is under way keeps no connection', async () => {
    const backend = await service(() => 200);
    const { direct, calls } = directStub();
    const api = fastestConnectionFetch(backend.origin, direct, { candidates: 3 });
    const controller = new AbortController();
    const pending = api(backend.origin + '/core/v4/users', { signal: controller.signal });
    setTimeout(() => controller.abort(), 40);
    let failure: unknown;
    try { await pending; } catch (error) { failure = error; }
    expect((failure as Error)?.name).toBe('AbortError');
    await settle();
    expect(backend.open()).toBe(0);
    expect(await (await api(backend.origin + '/core/v4/users')).text()).toBe('direct');
    expect(calls.length).toBe(1);
});

// A connection whose streams end as the test says, to exercise what a real service rarely does.
function scriptedConnection(end: (stream: any, session: any) => void) {
    const listeners = (target: any) => { const map = new Map<string, ((...args: any[]) => void)[]>();
        target.on = target.once = (name: string, run: (...args: any[]) => void) => { map.set(name, [...(map.get(name) ?? []), run]); return target; };
        target.emit = (name: string, ...args: any[]) => { for (const run of map.get(name) ?? []) run(...args); }; return target; };
    let streams = 0;
    const session: any = listeners({ closed: false, destroyed: false, destroy() { session.destroyed = true; }, close() { session.closed = true; },
        ping(done: (error: Error | null, ms: number) => void) { done(null, 0); },
        request(headers: Record<string, string>) {
            const stream: any = listeners({ id: 2 * streams++ + 1, rstCode: 0, path: headers[':path'], close() {}, end() { queueMicrotask(() => end(stream, session)); } });
            if (!headers[':method'] || headers[':method'] === 'GET') queueMicrotask(() => end(stream, session));
            return stream;
        } });
    queueMicrotask(() => session.emit('connect'));
    return session;
}
const answered = (stream: any) => { stream.emit('response', { ':status': 200 }); stream.emit('data', Buffer.from('{}')); stream.emit('end'); stream.emit('close'); };
test('a change the service says it never acted on is sent again on the ordinary client', async () => {
    for (const refusal of ['refused', 'http/1.1 required', 'shut down before it']) {
        const { direct, calls } = directStub();
        const api = fastestConnectionFetch('http://synthetic.invalid', direct, { candidates: 1, connect: () => scriptedConnection((stream, session) => {
            if (stream.path !== '/change') { answered(stream); return; }
            if (refusal === 'refused') stream.rstCode = http2.constants.NGHTTP2_REFUSED_STREAM;
            else if (refusal === 'http/1.1 required') stream.rstCode = http2.constants.NGHTTP2_HTTP_1_1_REQUIRED;
            else session.emit('goaway', 0, stream.id - 2);
            stream.emit('end'); stream.emit('close');
        }) });
        expect((await api('http://synthetic.invalid/core/v4/users')).status).toBe(200);
        expect(await (await api('http://synthetic.invalid/change', { method: 'POST', body: '{"Name":"x"}' })).text()).toBe('direct');
        expect(calls).toEqual(['POST /change {"Name":"x"}']);
        // The connection is retired with it.
        expect(await (await api('http://synthetic.invalid/core/v4/users')).text()).toBe('direct');
    }
    // A stream the service had already counted when it shut down may have been acted on: not repeated.
    const { direct, calls } = directStub();
    const api = fastestConnectionFetch('http://synthetic.invalid', direct, { candidates: 1, connect: () => scriptedConnection((stream, session) => {
        if (stream.path !== '/change') { answered(stream); return; }
        session.emit('goaway', 0, stream.id); stream.emit('end'); stream.emit('close');
    }) });
    await api('http://synthetic.invalid/core/v4/users');
    let failure: unknown;
    try { await api('http://synthetic.invalid/change', { method: 'POST', body: '{}' }); } catch (error) { failure = error; }
    expect(failure).toBeInstanceOf(TypeError); expect(calls).toEqual([]);
});

test('a connection is not replaced once the caller is about to change something', async () => {
    // As above, only the second round's fourth connection is fast; the caller settles before it is found.
    const backend = await service(connection => connection === 4 ? 5 : SLOW_ANSWER_MS + 80);
    const api = fastestConnectionFetch(backend.origin, directStub().direct, { candidates: 2 });
    const first = connectionOf(await api(backend.origin + '/core/v4/users'));
    api.settle();
    await settle(SLOW_ANSWER_MS + 300);
    expect(connectionOf(await api(backend.origin + '/drive/v2/volumes/v/files', { method: 'POST', body: '{}' }))).toBe(first);
    await settle();
    expect(backend.open()).toBe(1);
    // Settled before the first connection's ping came back: no second round is begun at all.
    const untouched = await service(() => SLOW_ANSWER_MS + 80);
    const early = fastestConnectionFetch(untouched.origin, directStub().direct, { candidates: 2 });
    const answered = early(untouched.origin + '/core/v4/users'); early.settle();
    await answered; await settle(SLOW_ANSWER_MS + 250);
    expect(untouched.sessions.length).toBe(2);
});
test('a file this process just uploaded is read again until the service shows it, and another revision is believed at once', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'putgd-reread-'));
    const path = join(directory, 'synthetic.bin'), data = Buffer.alloc(32, 112);
    await writeFile(path, data, { mode: 0o600 });
    const sha256 = createHash('sha256').update(data).digest('hex');
    // The service's early answers for a file it has just created (#1399): not returned at all, returned
    // without its active revision, or with its revision and none of that revision's metadata. And
    // a read that shows another revision of the file, which is a real change.
    type Shape = 'missing' | 'no-revision' | 'no-metadata' | 'other-revision';
    const prepared = (earlyReads: number, shape: Shape) => {
        const stub = sharingContext();
        const whole = (stub.ctx.sdk as any).getNode;
        let fileReads = 0;
        (stub.ctx.sdk as any).getNode = async (uid: string) => {
            const node = await whole(uid);
            if (uid === 'folder') return node;
            if (++fileReads <= earlyReads) {
                if (shape === 'missing') throw new Error('Synthetic node the batch does not return');
                if (shape === 'no-revision') return { ...node, activeRevision: undefined };
                const bare = { uid: node.activeRevision.uid, state: node.activeRevision.state, contentAuthor: node.activeRevision.contentAuthor };
                return { ...node, activeRevision: shape === 'no-metadata' ? bare : { ...bare, uid: 'someone-elses-revision' } };
            }
            return { ...node, activeRevision: { ...node.activeRevision, claimedSize: data.length,
                claimedAdditionalMetadata: { putgd: { publicationId: 'publication', sha256 } } } };
        };
        // A complete listing of the folder, without the file.
        (stub.ctx.sdk as any).iterateFolderChildrenNodeUids = async function* () {};
        (stub.ctx.sdk as any).getFileUploader = async () => ({ uploadFromStream: async (stream: ReadableStream) => {
            const reader = stream.getReader(); while (!(await reader.read()).done) {}
            return { completion: async () => ({ nodeUid: 'file-node', nodeRevisionUid: 'file-revision' }) };
        } });
        const request = { ...stub.request, receipt: { ...stub.request.receipt, objectReference: { ...stub.request.receipt.objectReference, sha256 } } };
        const uploaded = (publicationId = 'publication') => upload(stub.ctx, { ...statRequest(), method: 'upload', condition: 'absent', publicationId,
            artifact: { path, bytes: data.length, mime: 'application/octet-stream', sha256 } });
        return { stub, request, uploaded, fileReads: () => fileReads };
    };
    const pauses = [...REREAD_PAUSES_MS];
    REREAD_PAUSES_MS.fill(1);
    try {
        // Just uploaded here: each early answer is read again, the link is made, and the read after it is whole too.
        for (const shape of ['missing', 'no-revision', 'no-metadata'] as const) {
            const run = prepared(2, shape);
            expect((await run.uploaded()).status).toBe('uploaded');
            const shared = await share(run.stub.ctx, run.request);
            expect(shared.status).toBe('shared'); expect(shared.receipt.directUrl).toContain('#capability');
            expect(run.fileReads()).toBe(4); expect(run.stub.mutations()).toBe(1);
            // The upload and the link each settled the connection before changing anything.
            expect(run.stub.settledBeforeChange()).toBe(2);
        }
        // Another revision of the file is a change, believed at once: no link, nothing changed.
        let run = prepared(1, 'other-revision');
        expect((await run.uploaded()).status).toBe('uploaded');
        let result = await share(run.stub.ctx, run.request);
        expect(result.status).toBe('changed'); expect(result.linkAvailable).toBe(false); expect(result.receipt.directUrl).toBe('');
        expect(run.fileReads()).toBe(1); expect(run.stub.mutations()).toBe(0);
        // A file that stays unread gets no link, and is unknown rather than changed: six reads, no change.
        run = prepared(99, 'no-metadata');
        expect((await run.uploaded()).status).toBe('uploaded');
        result = await share(run.stub.ctx, run.request);
        expect(result.status).toBe('unknown'); expect(result.linkAvailable).toBe(false); expect(result.receipt.directUrl).toBe('');
        expect(run.fileReads()).toBe(1 + REREAD_PAUSES_MS.length); expect(run.stub.mutations()).toBe(0);
        // The last file this process uploaded was another: the first read stands, as for any older file.
        run = prepared(1, 'no-metadata');
        expect((await run.uploaded('an-earlier-publication')).status).toBe('uploaded');
        expect((await share(run.stub.ctx, run.request)).status).toBe('changed');
        expect(run.fileReads()).toBe(1); expect(run.stub.mutations()).toBe(0);
        // Another file's link is read once.
        run = prepared(1, 'missing');
        expect((await run.uploaded()).status).toBe('uploaded');
        const other = { ...run.request, receipt: { ...run.request.receipt, publicationId: 'another-publication' } };
        expect((await share(run.stub.ctx, other)).status).not.toBe('shared');
        expect(run.fileReads()).toBe(1);
    } finally { REREAD_PAUSES_MS.splice(0, REREAD_PAUSES_MS.length, ...pauses); await rm(directory, { recursive: true, force: true }); }
});
test('a keychain exchange the service refused is made again before the refusal is believed (#1418)', async () => {
    const pauses = [1, 1, 1];
    const script = (outcomes: (string | null | Error)[]) => { let calls = 0;
        return { calls: () => calls, exchange: async () => { const outcome = outcomes[Math.min(calls++, outcomes.length - 1)]; if (outcome instanceof Error) throw outcome; return outcome; } }; };
    const refused = new Error('Client public key size is invalid (code: 16)');
    let read = script([refused, 'entry']);
    expect(await keychain(read.exchange, pauses)).toBe('entry'); expect(read.calls()).toBe(2);
    read = script([refused, refused, refused, 'entry']);
    expect(await keychain(read.exchange, pauses)).toBe('entry'); expect(read.calls()).toBe(4);
    // A service that keeps refusing is reported, with its own error.
    read = script([refused]);
    let failure: unknown;
    try { await keychain(read.exchange, pauses); } catch (error) { failure = error; }
    expect(failure).toBe(refused); expect(read.calls()).toBe(4);
    // An answer is taken as it comes, an empty one included: asking this process again does not change it.
    read = script([null, 'entry']);
    expect(await keychain(read.exchange, pauses)).toBeNull(); expect(read.calls()).toBe(1);
    // A refreshed session's write is made again too; only a write that never succeeds is a keychain failure.
    let writes = 0;
    await saveToKeychain(async () => { if (++writes < 3) throw refused; });
    expect(writes).toBe(3);
    let lost: unknown;
    try { await saveToKeychain(async () => { ++writes; throw refused; }); } catch (error) { lost = error; }
    expect((lost as ProtocolError).code).toBe('keychain_unavailable'); expect(writes).toBe(7);
});
test('the first process to find no session says so once, and the one started after it believes what it finds (#1418)', () => {
    expect(sessionUnread(false, false)).toBe(true);
    expect(sessionUnread(false, true)).toBe(false);
    expect(sessionUnread(true, false)).toBe(false);
    expect(sessionUnread(true, true)).toBe(false);
    // The application sees a failure that changed nothing and that names itself.
    const answer = serviceFailure(new ProtocolError('session_unread'));
    expect(answer.code).toBe('session_unread'); expect(answer.details).toEqual({});
});
