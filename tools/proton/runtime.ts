import { constants } from 'node:fs';
import { open } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { Readable } from 'node:stream';
import { MemberRole, NodeType, NodeWithSameNameExistsValidationError, type NodeEntity, type ProtonDriveClient } from '@protontech/drive-sdk';
import { APP_VERSION, SDK_VERSION, SECRET_SERVICE, initialize, privateDataDirectory } from './account';
import { MAX_REQUEST_BYTES, ProtocolError, classifyNode, configured, days, failure, hash, makeReceipt,
    name, object, parseRequest, receipt, reference, shareOutcome, connectAction, text, type JsonObject } from './protocol';
import { writeJson } from './stdout';

type Context = { sdk: ProtonDriveClient; auth: Awaited<ReturnType<typeof initialize>>['auth'] };
const controller = new AbortController();
let mutationStarted = false;
let finalWrite: Promise<void> | undefined;
let activeRequest: JsonObject | undefined;
function writeFinal(value: JsonObject): Promise<void> {
    return finalWrite ??= writeJson(value);
}
const success = (result: JsonObject) => ({ ok: true, result });
const status = (value: string, fields: JsonObject = {}) => ({ status: value, ...fields, error: {} });
async function readRequest(): Promise<JsonObject> {
    const chunks: Buffer[] = [];
    let count = 0;
    for await (const chunk of process.stdin) {
        const buffer = Buffer.from(chunk); count += buffer.length;
        if (count > MAX_REQUEST_BYTES) throw new ProtocolError('invalid_request');
        chunks.push(buffer);
    }
    return parseRequest(Buffer.concat(chunks));
}
export async function accountRoot(ctx: Context, request: JsonObject, configuredRequired = true) {
    if (!ctx.auth.isLoggedIn()) throw new ProtocolError('authentication_required');
    const root = await ctx.sdk.getMyFilesRootFolder();
    if (configuredRequired && root.uid !== text(request.destination.settings.accountRootUid)) throw new ProtocolError('account_changed');
    return root;
}
export async function destinationFolder(ctx: Context, request: JsonObject) {
    const root = await accountRoot(ctx, request);
    const { folderUid } = configured(request);
    const folder = folderUid === root.uid ? root : await ctx.sdk.getNode(folderUid);
    if (folder.type !== NodeType.Folder || folder.trashTime || folder.errors?.length || !folder.name.ok ||
        (folder.uid !== root.uid && folder.parentUid !== root.uid)) throw new ProtocolError('destination_unavailable');
    return folder;
}
export async function inspectAccount(ctx: Context, request: JsonObject): Promise<JsonObject> {
    const root = await accountRoot(ctx, request, !!request.destination.settings.accountRootUid);
    const folders: JsonObject[] = [];
    for await (const node of ctx.sdk.iterateFolderChildren(root.uid, { type: NodeType.Folder }, controller.signal)) {
        if (!node.name.ok || node.errors?.length) throw new ProtocolError('destination_unavailable');
        name(node.name.value); text(node.uid);
        if (!node.trashTime) folders.push({ uid: node.uid, name: node.name.value });
        if (folders.length > 10000) throw new ProtocolError('destination_unavailable');
    }
    return { accountRootUid: root.uid, folders };
}
async function findCandidate(ctx: Context, request: JsonObject): Promise<NodeEntity | undefined> {
    const folder = await destinationFolder(ctx, request);
    const candidate = name(request.candidate);
    let found: NodeEntity | undefined;
    let count = 0;
    for await (const node of ctx.sdk.iterateFolderChildren(folder.uid, undefined, controller.signal)) {
        if (!node.name.ok || node.errors?.length || ++count > 100000) throw new ProtocolError('destination_unavailable');
        if (node.name.value === candidate && !node.trashTime) {
            if (found) throw new ProtocolError('destination_unavailable');
            found = node;
        }
    }
    return found;
}
export async function checkName(ctx: Context, request: JsonObject) {
    try {
        const folder = await destinationFolder(ctx, request);
        const candidate = name(request.candidate);
        const available = await ctx.sdk.getAvailableName(folder.uid, candidate);
        if (typeof available !== 'string' || !available) return status('inconclusive');
        return status(available === candidate ? 'absent' : 'present', {
            objectReference: reference(request), concurrencyToken: '' });
    } catch { return status('inconclusive'); }
}
export async function stat(ctx: Context, request: JsonObject) {
    const proof = receipt(request), ref = proof.objectReference;
    try {
        await destinationFolder(ctx, request);
        let node: NodeEntity;
        try { node = await ctx.sdk.getNode(ref.nodeUid); }
        catch {
            // A denied lookup is not absence. Only a complete folder list proves missing here.
            for await (const uid of ctx.sdk.iterateFolderChildrenNodeUids(ref.folderUid, undefined, controller.signal))
                if (uid === ref.nodeUid) return status('unknown');
            return status('missing');
        }
        return status(classifyNode(node, ref, proof.publicationId), { ...(classifyNode(node, ref, proof.publicationId) === 'own' ? { receipt: proof } : {}) });
    } catch { return status('unknown'); }
}
async function reconcile(ctx: Context, request: JsonObject) {
    hash(request.artifact?.sha256); text(request.publicationId);
    try {
        const node = await findCandidate(ctx, request);
        if (!node) return status('missing');
        const ref = { ...reference(request, node.uid, node.activeRevision?.uid), sha256: request.artifact.sha256 };
        const ownership = classifyNode(node, ref, request.publicationId);
        if (ownership !== 'own' || !node.activeRevision) return status(ownership);
        return status('own', { receipt: makeReceipt(request, node.uid, node.activeRevision.uid) });
    } catch { return status('unknown'); }
}
async function upload(ctx: Context, request: JsonObject) {
    if (request.condition !== 'absent') throw new ProtocolError('unsupported_condition');
    const artifact = object(request.artifact);
    name(request.candidate); hash(artifact.sha256); text(request.publicationId);
    text(artifact.path, 8192); text(artifact.mime, 255);
    if (!artifact.path.startsWith('/') || !Number.isSafeInteger(artifact.bytes) || artifact.bytes < 0)
        throw new ProtocolError('invalid_request');
    const folder = await destinationFolder(ctx, request);
    const file = await open(artifact.path, constants.O_RDONLY | constants.O_NOFOLLOW);
    try {
        const initial = await file.stat();
        if (!initial.isFile() || initial.size !== artifact.bytes) throw new ProtocolError('artifact_changed');
        const sha1 = createHash('sha1'), sha256 = createHash('sha256');
        for await (const chunk of file.createReadStream({ autoClose: false, start: 0 })) { sha1.update(chunk); sha256.update(chunk); }
        const after = await file.stat();
        if (sha256.digest('hex') !== artifact.sha256 || after.size !== initial.size || after.mtimeMs !== initial.mtimeMs)
            throw new ProtocolError('artifact_changed');
        mutationStarted = true;
        try {
            const uploader = await ctx.sdk.getFileUploader(folder.uid, request.candidate, {
                mediaType: artifact.mime, expectedSize: artifact.bytes, expectedSha1: sha1.digest('hex'),
                additionalMetadata: { putgd: { publicationId: request.publicationId, sha256: artifact.sha256 } },
            }, controller.signal);
            // Node and Bun declare slightly different web-stream extensions; SDK needs standard ReadableStream only.
            const stream = Readable.toWeb(file.createReadStream({ autoClose: false, start: 0 })) as unknown as ReadableStream;
            const pending = await uploader.uploadFromStream(stream, []);
            const completed = await pending.completion();
            return status('uploaded', { receipt: makeReceipt(request, completed.nodeUid, completed.nodeRevisionUid) });
        } catch (error) {
            if (error instanceof NodeWithSameNameExistsValidationError) return status('collision');
            return status('unknown', { error: failure('operation_unknown') });
        }
    } finally { await file.close(); }
}
async function share(ctx: Context, request: JsonObject) {
    const proof = receipt(request), options = object(request.options);
    if (!['create', 'revoke', 'inspect'].includes(options.action)) throw new ProtocolError('invalid_request');
    const settings = proof.destination.settings;
    const expirationDays = days(options.expirationDays ?? settings.linkExpirationDays ?? 0);
    const passwordReference = options.passwordReference === undefined ? settings.sharePasswordReference : options.passwordReference;
    if (passwordReference !== null && passwordReference !== undefined) text(passwordReference);
    const checked = await stat(ctx, request);
    if (checked.status !== 'own') return shareOutcome(proof, options.action, '', checked.status);
    let customPassword: string | undefined;
    if (options.action === 'create' && passwordReference) {
        try {
            const raw = await Bun.secrets.get({ service: SECRET_SERVICE, name: passwordReference });
            const stored = raw ? JSON.parse(raw) : undefined;
            if (stored?.schemaVersion !== 1 || stored?.kind !== 'share-password' || typeof stored?.value !== 'string')
                throw new ProtocolError('keychain_unavailable');
            customPassword = stored.value;
        }
        catch { throw new ProtocolError('keychain_unavailable'); }
        if (!customPassword) throw new ProtocolError('keychain_unavailable');
    }
    try {
        const uid = proof.objectReference.nodeUid;
        let info;
        if (options.action === 'inspect') info = await ctx.sdk.getSharingInfo(uid);
        else {
            mutationStarted = true;
            info = options.action === 'revoke' ? await ctx.sdk.unshareNode(uid, { urlAccess: 'remove' }) :
                await ctx.sdk.shareNode(uid, { urlAccess: { role: MemberRole.Viewer, customPassword,
                    expiration: expirationDays ? new Date(Date.now() + expirationDays * 86400000) : undefined } });
        }
        // URL is returned intact by the SDK, including its capability fragment. Never reconstruct it.
        const directUrl = info?.urlAccess?.url ?? '';
        const after = await stat(ctx, request);
        return shareOutcome(proof, options.action, directUrl, after.status, info?.urlAccess?.expirationTime);
    } catch {
        return shareOutcome(proof, options.action, '', 'unknown');
    } finally { customPassword = undefined; }
}
export async function createFolder(ctx: Context, request: JsonObject): Promise<JsonObject> {
    const folderName = name(request.options?.name ?? request.candidate);
    const root = await accountRoot(ctx, request, !!request.destination.settings.accountRootUid);
    mutationStarted = true;
    try {
        const folder = await ctx.sdk.createFolder(root.uid, folderName);
        return { uid: folder.uid, name: folderName, accountRootUid: root.uid };
    } catch { throw new ProtocolError('folder_creation_unknown'); }
}
async function run(request: JsonObject): Promise<JsonObject> {
    if (request.method === 'remove') throw new ProtocolError('unsupported_safe_removal');
    if (request.method === 'storeSharePassword') {
        await privateDataDirectory();
        const options = request.options;
        try {
            if (options.reference === request.destination.credentials.reference) throw new ProtocolError('reference_in_use');
            const serialized = JSON.stringify({ schemaVersion: 1, kind: 'share-password', value: options.password });
            const stored = await Bun.secrets.get({ service: SECRET_SERVICE, name: options.reference });
            if (stored !== null && stored !== undefined && stored !== serialized) throw new ProtocolError('reference_in_use');
            if (stored === null || stored === undefined)
                await Bun.secrets.set({ service: SECRET_SERVICE, name: options.reference, value: serialized });
            options.password = undefined;
            return { reference: options.reference };
        } catch (error) {
            if (error instanceof ProtocolError) throw error;
            throw new ProtocolError('keychain_unavailable');
        } finally { options.password = undefined; }
    }
    const ctx = await initialize(request);
    if (request.method === 'connect') {
        // Reconnect never replaces a valid session in place. A new login uses a new reference.
        const action = connectAction(ctx.auth.isLoggedIn(), ctx.accountReferenceExists);
        if (action === 'inspect') return inspectAccount(ctx, request);
        if (action === 'new_reference_required') throw new ProtocolError('account_reference_in_use');
        await ctx.auth.authViaWeb((url) => writeJson({ event: 'auth', url }), controller.signal);
        return inspectAccount(ctx, request);
    }
    if (request.method === 'inspectAccount') return inspectAccount(ctx, request);
    if (request.method === 'createFolder') return createFolder(ctx, request);
    if (request.method === 'checkName') return checkName(ctx, request);
    if (request.method === 'upload') return upload(ctx, request);
    if (request.method === 'stat') return stat(ctx, request);
    if (request.method === 'reconcile') return reconcile(ctx, request);
    if (request.method === 'share') return share(ctx, request);
    throw new ProtocolError('unsupported_request');
}
async function timeout() {
    controller.abort();
    const method = activeRequest?.method;
    if (mutationStarted && (method === 'upload' || method === 'share'))
        await writeFinal(success(method === 'share' ? shareOutcome(receipt(activeRequest!), activeRequest!.options.action, '', 'unknown') :
            status('unknown', { error: failure('operation_unknown') })));
    else await writeFinal({ ok: false, error: failure('request_timeout') });
    process.exit(0);
}
if (import.meta.main) {
if (process.argv.length === 3 && process.argv[2] === '--version') {
    await writeJson({ schemaVersion: 1, runtime: 'putgd-proton-runtime', appVersion: APP_VERSION,
        sdkVersion: SDK_VERSION, bunVersion: Bun.version, protocolVersion: 1 });
} else if (process.argv.length === 3 && process.argv[2] === '--stdio') {
    let timer = setTimeout(timeout, 10000);
    try {
        activeRequest = await readRequest();
        clearTimeout(timer);
        const timeoutMs = activeRequest.method === 'connect' ? 600000 : activeRequest.destination.settings.requestTimeoutMs ?? 120000;
        if (!Number.isInteger(timeoutMs) || timeoutMs < 1000 || timeoutMs > 600000) throw new ProtocolError('invalid_request');
        timer = setTimeout(timeout, timeoutMs);
        const result = await run(activeRequest);
        clearTimeout(timer);
        await writeFinal(success(result));
    } catch (error) {
        clearTimeout(timer);
        await writeFinal({ ok: false, error: failure(error instanceof ProtocolError ? error.code : mutationStarted ? 'operation_unknown' : 'operation_failed') });
    } finally { clearTimeout(timer); controller.abort(); }
    process.exit(0);
} else {
    await writeFinal({ ok: false, error: failure('unsupported_request') }); process.exitCode = 2;
}
}
