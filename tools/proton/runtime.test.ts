import { expect, test } from 'bun:test';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { MemberRole, NodeType, RevisionState, NodeWithSameNameExistsValidationError, RateLimitedError, ServerError, ValidationError, type NodeEntity } from '@protontech/drive-sdk';
import { checkName, createFolder, destinationFolder, inspectAccount, stat, upload, share, serviceFailure } from './runtime';
import { ProtocolError, makeReceipt, type JsonObject } from './protocol';
import { saveToKeychain } from './account';
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
    expect(await inspectAccount(stub.ctx, setup)).toEqual({ accountRootUid: 'root', accountEmail: 'synthetic@example.invalid', folders: [{ uid: 'folder', name: 'Uploads' }] });
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
        expect(stub.calls[2].args).toEqual(['folder', 'photo.png', NodeType.File]);
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
