import { constants } from 'node:fs';
import { open } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { Readable } from 'node:stream';
import { MemberRole, NodeType, NodeWithSameNameExistsValidationError, RateLimitedError, ServerError, ValidationError, type NodeEntity, type ProtonDriveClient } from '@protontech/drive-sdk';
import { APP_VERSION, SDK_VERSION, SECRET_SERVICE, initialize, keychain, openAccount, openClient, privateDataDirectory } from './account';
import { MAX_REQUEST_BYTES, ProtocolError, classifyNode, configured, days, failure, hash, makeReceipt,
    name, object, parseRequest, publicationClientUid, receipt, reference, revisionWithoutMetadata, shareOutcome, connectAction, text, requestTimeout, type JsonObject } from './protocol';
import { AccountApiError } from 'proton-drive-sdk-account';
import { writeJson } from './stdout';
import { createProgressReporter } from './progress';

type Context = Awaited<ReturnType<typeof initialize>>;
// The SDK sends a file under this size in one request, and a larger one in blocks.
const SINGLE_REQUEST_BYTES = 120 * 1024;
// The SDK's block size. A link is prepared beside an upload of one block at most: a longer
// transfer outlasts it.
const ONE_BLOCK_BYTES = 4 * 1024 * 1024;
let controller = new AbortController();
let mutationStarted = false;
let finalWrite: Promise<void> | undefined;
let activeRequest: JsonObject | undefined;
let deadlineExpired = false;
let uploadProgress: ReturnType<typeof createProgressReporter> | undefined;
let sessions = false;
function writeFinal(value: JsonObject): Promise<void> {
    return finalWrite ??= (async () => { await uploadProgress?.finish(); await writeJson(value); })();
}
const success = (result: JsonObject) => ({ ok: true, result });
const status = (value: string, fields: JsonObject = {}) => ({ status: value, error: {}, ...fields });
export function serviceFailure(error: unknown, fallback = 'operation_failed', authenticated?: boolean): JsonObject {
    if (error instanceof ProtocolError) return failure(error.code);
    if (authenticated === false) return failure('authentication_required');
    const http = error instanceof AccountApiError ? error.httpCode : error instanceof ServerError ? error.statusCode : undefined;
    if (http === 401) return failure('authentication_required');
    if (http === 403) return failure('permission_denied');
    if (http === 429 || error instanceof RateLimitedError) return failure('rate_limited');
    // Public ValidationError codes from the pinned SDK's API error factory.
    if (error instanceof ValidationError) {
        if ([2011, 2026].includes(error.code ?? 0)) return failure('permission_denied');
        // 200003 (MAX_FILE_SIZE_FOR_FREE_USER) is the plan's per-file limit, not a full drive.
        if (error.code === 200003) return failure('destination_file_too_large_for_plan');
        if ([200001, 200002, 200004, 200100, 200101, 200600, 200601, 200602, 200800].includes(error.code ?? 0))
            return failure('quota_exceeded');
    }
    return failure(fallback);
}

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
    ctx.warm();
    let root: NodeEntity;
    try { root = await ctx.sdk.getMyFilesRootFolder(); }
    catch (error) {
        if (!ctx.auth.isLoggedIn()) throw new ProtocolError('authentication_required');
        throw error;
    }
    if (configuredRequired && root.uid !== text(request.destination.settings.accountRootUid)) throw new ProtocolError('account_changed');
    return root;
}
// One batched read from the service, never from this request's cache (see `fresh`).
// A node the service does not return is absent from the result.
async function readNodes(ctx: Context, uids: string[]): Promise<Map<string, NodeEntity>> {
    const nodes = new Map<string, NodeEntity>();
    await ctx.fresh(async () => {
        for await (const node of ctx.sdk.iterateNodes([...new Set(uids)], controller.signal))
            if (!('missingUid' in node)) nodes.set(node.uid, node);
    });
    return nodes;
}
// The configured folder, checked against the account, with any `further` nodes: one
// batch for all of them instead of a request each.
export async function readDestination(ctx: Context, request: JsonObject, further: string[] = []) {
    if (!ctx.auth.isLoggedIn()) throw new ProtocolError('authentication_required');
    const { accountRootUid, folderUid } = configured(request);
    ctx.warm();
    let nodes = new Map<string, NodeEntity>(), failed: unknown;
    try { nodes = await readNodes(ctx, [accountRootUid, folderUid, ...further]); }
    catch (error) { failed = error; }
    // The service names My files, not the configured UID: another root is another account.
    const root = await accountRoot(ctx, request);
    if (failed) throw failed;
    const folder = nodes.get(folderUid);
    if (!nodes.has(root.uid) || !folder || folder.type !== NodeType.Folder || folder.trashTime || folder.errors?.length ||
        !folder.name.ok || (folder.uid !== root.uid && folder.parentUid !== root.uid)) throw new ProtocolError('destination_unavailable');
    return { folder, nodes };
}
export async function destinationFolder(ctx: Context, request: JsonObject) {
    return (await readDestination(ctx, request)).folder;
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
    const accountEmail = root.ownedBy?.email;
    return { accountRootUid: root.uid, ...(accountEmail ? { accountEmail } : {}), folders };
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
// In a session, the destination read made for a name check also serves the one upload that
// follows it within ten seconds. The two are halves of one creation: the service decides a
// collision, and an upload already outlasts its own destination read by its whole transfer.
let checkedDestination: { key: string; at: number; folder: NodeEntity } | undefined;
// Destinations this session has read: their folder keys are in the process, so a name
// query can run beside the destination read instead of after it.
const readDestinations = new Set<string>();
const destinationKey = (request: JsonObject) => JSON.stringify([request.destination.credentials.reference, configured(request)]);
export async function checkName(ctx: Context, request: JsonObject) {
    try {
        const candidate = name(request.candidate);
        const beside = sessions && readDestinations.has(destinationKey(request))
            ? ctx.sdk.getAvailableName(configured(request).folderUid, candidate, NodeType.File) : undefined;
        beside?.catch(() => {});
        const folder = await destinationFolder(ctx, request);
        if (sessions) { checkedDestination = { key: destinationKey(request), at: performance.now(), folder }; readDestinations.add(destinationKey(request)); }
        const available = await (beside ?? ctx.sdk.getAvailableName(folder.uid, candidate, NodeType.File));
        if (typeof available !== 'string' || !available) return status('inconclusive');
        return status(available === candidate ? 'absent' : 'present', {
            objectReference: reference(request), concurrencyToken: '' });
    } catch (error) { return status('inconclusive', { error: serviceFailure(error, 'operation_failed', ctx.auth.isLoggedIn()) }); }
}
// One read of the file a receipt names. `early` marks an answer the service gave without an error
// that may only predate the upload: the file not returned at all, or returned with its own
// revision and none of that revision's metadata (`revisionWithoutMetadata`).
async function readFile(ctx: Context, request: JsonObject): Promise<{ checked: JsonObject; early: boolean }> {
    const proof = receipt(request), ref = proof.objectReference;
    try {
        const node = (await readDestination(ctx, request, [ref.nodeUid])).nodes.get(ref.nodeUid);
        if (!node) {
            // An unanswered lookup is not absence. Only a complete folder list proves missing here.
            for await (const uid of ctx.sdk.iterateFolderChildrenNodeUids(ref.folderUid, undefined, controller.signal))
                if (uid === ref.nodeUid) return { checked: status('unknown'), early: true };
            return { checked: status('missing'), early: true };
        }
        const ownership = classifyNode(node, ref, proof.publicationId);
        return { checked: status(ownership, ownership === 'own' ? { receipt: proof } : {}),
            early: ownership === 'unknown' || (ownership === 'foreign' && revisionWithoutMetadata(node, ref)) };
    } catch (error) { return { checked: status('unknown', { error: serviceFailure(error, 'operation_unknown', ctx.auth.isLoggedIn()) }), early: false }; }
}
export async function stat(ctx: Context, request: JsonObject) { return (await readFile(ctx, request)).checked; }
async function reconcile(ctx: Context, request: JsonObject) {
    hash(request.artifact?.sha256); text(request.publicationId);
    try {
        const node = await findCandidate(ctx, request);
        if (!node) return status('missing');
        const ref = { ...reference(request, node.uid, node.activeRevision?.uid), sha256: request.artifact.sha256 };
        const ownership = classifyNode(node, ref, request.publicationId);
        if (ownership !== 'own' || !node.activeRevision) return status(ownership);
        return status('own', { receipt: makeReceipt(request, node.uid, node.activeRevision.uid) });
    } catch (error) { return status('unknown', { error: serviceFailure(error, 'operation_unknown', ctx.auth.isLoggedIn()) }); }
}
// The hashed availability query counts completed files and unfinished drafts, so a free
// name proves neither exists. Any doubt, including a failed query, keeps the outcome unknown.
async function nameStillFree(ctx: Context, folderUid: string, candidate: string): Promise<boolean> {
    try { return await ctx.sdk.getAvailableName(folderUid, candidate, NodeType.File) === candidate; }
    catch { return false; }
}
const API_HOST = 'drive-api.proton.me';
// A file this process has just uploaded can read back as not yet there, with no error from the
// service. Seen twice in desktop acceptance, about one publish in four hundred: in #1414 a read
// 0.4 s after the upload came back `unknown`; in #1399 (m74, nightly 26.1113.4757) the reads
// before the link took 2.6 s, the three reads again included, which fits four reads that each
// had to list the folder, and so ended `missing`: the link was reported `changed` and never
// made, and an inspection later found the file put.gd's own. On d69 none of 196 uploads read
// back that way, on the kept connection or on six new ones each. The pinned SDK leaves a file
// the service returns without its active revision out of a batch read as a draft (missing), and
// reads a revision returned without its extended attributes as having no metadata, which
// `classifyNode` calls foreign without a read again. Neither is another file. So for the file
// this process has just uploaded, and only while the service answers without an error, such a
// read is made again, with pauses that double, for about ten seconds at most. A read that shows
// another revision, name, folder or trash is believed at once. A file that stays unread gets no
// link and is reported `unknown`, which the application inspects again on Retry, never
// `changed`, which says the file was replaced and ends the link step.
let uploadedHere: { key: string; nodeUid: string; revisionUid: string; publicationId: string; at: number } | undefined;
export const REREAD_PAUSES_MS = [250, 500, 1000, 2000, 4000];
const JUST_UPLOADED_MS = 30000;
async function ownershipAfterUpload(ctx: Context, request: JsonObject, justUploaded: boolean) {
    let { checked, early } = await readFile(ctx, request);
    if (!justUploaded) return checked;
    for (const pause of REREAD_PAUSES_MS) {
        if (!early || controller.signal.aborted) break;
        await new Promise(resolve => setTimeout(resolve, pause));
        ({ checked, early } = await readFile(ctx, request));
    }
    return early ? status('unknown') : checked;
}
// A file over the single-request limit goes to a storage host named only by the request
// before its bytes. The application remembers the host its last upload used and names it in
// the next one; the connection is then opened while the upload is being set up, instead of
// when its bytes are ready. The request carries nothing of the account or the file. Only a
// file of one block gets it: with several blocks sent side by side, the block on the opened
// connection started ahead of the others and the late ones crawled (8.6 to 10.7 s for 8 MiB
// with it, 4.5 to 6.6 s without, on the lab's uplink).
export function transferHost(value: unknown): string | undefined {
    return typeof value === 'string' && /^[a-z0-9-]{1,48}\.proton\.me$/.test(value) && value !== API_HOST ? value : undefined;
}
function openTransferConnection(host: string) {
    // A GET whose answer is read to its end: the HTTP client keeps that connection for the
    // request that follows, which it does not after a HEAD.
    void fetch(`https://${host}/`, { signal: AbortSignal.timeout(5000) })
        .then(response => response.arrayBuffer()).catch(() => {});
}
export async function upload(ctx: Context, request: JsonObject): Promise<JsonObject> {
    if (request.condition !== 'absent') throw new ProtocolError('unsupported_condition');
    const artifact = object(request.artifact);
    name(request.candidate); hash(artifact.sha256); text(request.publicationId);
    text(artifact.path, 8192); text(artifact.mime, 255);
    if (!artifact.path.startsWith('/') || !Number.isSafeInteger(artifact.bytes) || artifact.bytes < 0)
        throw new ProtocolError('invalid_request');
    uploadProgress = createProgressReporter();
    await uploadProgress.phase('preparing');
    const checked = checkedDestination;
    checkedDestination = undefined;
    if (!ctx.auth.isLoggedIn()) throw new ProtocolError('authentication_required');
    const folder = checked && checked.key === destinationKey(request) && performance.now() - checked.at < 10000
        ? checked.folder : await destinationFolder(ctx, request);
    const file = await open(artifact.path, constants.O_RDONLY | constants.O_NOFOLLOW);
    try {
        const initial = await file.stat();
        if (!initial.isFile() || initial.size !== artifact.bytes) throw new ProtocolError('artifact_changed');
        const sha1 = createHash('sha1'), sha256 = createHash('sha256');
        for await (const chunk of file.createReadStream({ autoClose: false, start: 0 })) { sha1.update(chunk); sha256.update(chunk); }
        const after = await file.stat();
        if (sha256.digest('hex') !== artifact.sha256 || after.size !== initial.size || after.mtimeMs !== initial.mtimeMs)
            throw new ProtocolError('artifact_changed');
        mutationStarted = true; ctx.connectionSettled();
        let sentTo: string | undefined;
        // The link that follows this upload needs its password hashed: that starts when the
        // file's bytes go out, and is ready when they have arrived.
        let prepare = request.linkFollows === true && !request.destination.settings.sharePasswordReference &&
            artifact.bytes <= ONE_BLOCK_BYTES;
        ctx.watchTransfers(host => {
            if (host !== API_HOST) sentTo = host;
            if (prepare) { prepare = false; ctx.prepareLink(); }
        });
        const hinted = artifact.bytes > SINGLE_REQUEST_BYTES && artifact.bytes <= ONE_BLOCK_BYTES ? transferHost(request.transferHostHint) : undefined;
        if (hinted) openTransferConnection(hinted);
        try {
            const uploader = await ctx.sdk.getFileUploader(folder.uid, request.candidate, {
                mediaType: artifact.mime, expectedSize: artifact.bytes, expectedSha1: sha1.digest('hex'),
                additionalMetadata: { putgd: { publicationId: request.publicationId, sha256: artifact.sha256 } },
            }, controller.signal);
            // Node and Bun declare slightly different web-stream extensions; SDK needs standard ReadableStream only.
            const stream = Readable.toWeb(file.createReadStream({ autoClose: false, start: 0 })) as unknown as ReadableStream;
            await uploadProgress.phase('uploading');
            // The pinned SDK counts encrypted block/thumbnail traffic, may
            // roll back retries, and has no matching total. Do not divide by
            // plaintext artifact bytes or infer publication from its counter.
            const pending = await uploader.uploadFromStream(stream, [], bytes => uploadProgress?.report(bytes));
            const completed = await pending.completion();
            const made = makeReceipt(request, completed.nodeUid, completed.nodeRevisionUid);
            uploadedHere = { key: destinationKey(request), nodeUid: completed.nodeUid, revisionUid: completed.nodeRevisionUid,
                publicationId: request.publicationId, at: performance.now() };
            const usedHost = transferHost(sentTo);
            return status('uploaded', { receipt: made, ...(usedHost ? { transferHost: usedHost } : {}) });
        } catch (error) {
            ctx.linkUnused();
            if (error instanceof NodeWithSameNameExistsValidationError) return status('collision', { error: error.isUnfinishedUpload ? failure('upload_draft_conflict') : {} });
            if (deadlineExpired) return status('unknown', { error: deadlineError() });
            const refused = serviceFailure(error, 'operation_unknown', ctx.auth.isLoggedIn());
            // A plan or quota refusal settles as failed only when the name is still free afterwards (#201).
            if (['destination_file_too_large_for_plan', 'quota_exceeded'].includes(refused.code) && await nameStillFree(ctx, folder.uid, request.candidate))
                return status('failed', { error: refused });
            return status('unknown', { error: refused });
        }
    } finally { ctx.watchTransfers(); try { await uploadProgress.finish(); } finally { await file.close(); } }
}
export async function share(ctx: Context, request: JsonObject) {
    const proof = receipt(request), options = object(request.options);
    if (!['create', 'revoke', 'inspect'].includes(options.action)) throw new ProtocolError('invalid_request');
    const settings = proof.destination.settings;
    const expirationDays = days(options.expirationDays ?? settings.linkExpirationDays ?? 0);
    const passwordReference = options.passwordReference === undefined ? settings.sharePasswordReference : options.passwordReference;
    if (passwordReference !== null && passwordReference !== undefined) text(passwordReference);
    const here = uploadedHere, ref = proof.objectReference;
    const justUploaded = options.action === 'create' && !!here && here.key === destinationKey(request) && here.nodeUid === ref.nodeUid &&
        here.revisionUid === ref.revisionUid && here.publicationId === proof.publicationId && performance.now() - here.at < JUST_UPLOADED_MS;
    const checked = await ownershipAfterUpload(ctx, request, justUploaded);
    if (checked.status !== 'own') return { ...shareOutcome(proof, options.action, '', checked.status), error: checked.error };
    let customPassword: string | undefined;
    if (options.action === 'create' && passwordReference) {
        try {
            const raw = await keychain(() => Bun.secrets.get({ service: SECRET_SERVICE, name: passwordReference }));
            const stored = raw ? JSON.parse(raw) : undefined;
            if (stored?.schemaVersion !== 1 || stored?.kind !== 'share-password' || typeof stored?.value !== 'string')
                throw new ProtocolError('keychain_unavailable');
            customPassword = stored.value;
        }
        catch { throw new ProtocolError('keychain_unavailable'); }
        if (!customPassword) throw new ProtocolError('keychain_unavailable');
    }
    let modulusUnused = () => {};
    try {
        const uid = proof.objectReference.nodeUid;
        let info;
        if (options.action === 'inspect') info = await ctx.sdk.getSharingInfo(uid);
        else {
            mutationStarted = true; ctx.connectionSettled();
            // A link prepared during the upload has its modulus already.
            if (options.action === 'create' && !(ctx.linkPrepared() && !customPassword)) modulusUnused = ctx.modulusAhead();
            ctx.linkBeingMade(options.action === 'create');
            info = options.action === 'revoke' ? await ctx.sdk.unshareNode(uid, { urlAccess: 'remove' }) :
                await ctx.sdk.shareNode(uid, { urlAccess: { role: MemberRole.Viewer, customPassword,
                    expiration: expirationDays ? new Date(Date.now() + expirationDays * 86400000) : undefined } });
        }
        // URL is returned intact by the SDK, including its capability fragment. Never reconstruct it.
        const directUrl = info?.urlAccess?.url ?? '';
        const after = await ownershipAfterUpload(ctx, request, justUploaded);
        return { ...shareOutcome(proof, options.action, directUrl, after.status, info?.urlAccess?.expirationTime), error: after.error };
    } catch (error) {
        const result = shareOutcome(proof, options.action, '', 'unknown');
        result.error = deadlineExpired ? deadlineError() : serviceFailure(error, 'operation_unknown', ctx.auth.isLoggedIn());
        return result;
    } finally { customPassword = undefined; modulusUnused(); ctx.linkBeingMade(false); ctx.linkUnused(); }
}
export async function createFolder(ctx: Context, request: JsonObject): Promise<JsonObject> {
    const folderName = name(request.options?.name ?? request.candidate);
    const root = await accountRoot(ctx, request, !!request.destination.settings.accountRootUid);
    mutationStarted = true; ctx.connectionSettled();
    try {
        const folder = await ctx.sdk.createFolder(root.uid, folderName);
        return { uid: folder.uid, name: folderName, accountRootUid: root.uid };
    } catch { throw new ProtocolError('folder_creation_unknown'); }
}
// `--session` answers one account's requests, a line each, until its input closes: the
// provider holds one for an operation's name check, upload and link. The signed-in account
// and one Drive client are kept between them; every request still makes its own destination
// and ownership reads from the service, and has its own deadline and cancellation.
const SESSION_METHODS = new Set(['warm', 'checkName', 'upload', 'stat', 'reconcile', 'share']);
let session: { reference: string; account: Awaited<ReturnType<typeof openAccount>>; clientUid?: string; ctx: Context } | undefined;
// A process whose keychain read found no session cannot tell an account that was never
// connected from an exchange with the secret service that went wrong for this process, and
// the second stays wrong however often this process asks (#1418). So the first process to
// find none says `session_unread` and changes nothing; the application starts another, which
// is told so (`sessionRetry`) and believes what it finds. A read the service kept refusing is
// treated the same way.
export function sessionUnread(found: boolean, told: boolean): boolean { return !found && !told; }
// This process was started after one that found no session, and has been told so.
let believesItsKeychain = false;
async function opened(request: JsonObject) {
    if (!sessions) return initialize(request);
    const reference = request.destination.credentials.reference, clientUid = publicationClientUid(request);
    if (session && session.reference !== reference) throw new ProtocolError('invalid_request');
    // Only a request with another publication's client identity needs another client.
    if (session && (clientUid === undefined || clientUid === session.clientUid)) return session.ctx;
    const account = session?.account ?? await openAccount(request);
    session = { reference, account, clientUid, ctx: await openClient(account, request) };
    return session.ctx;
}
async function context(request: JsonObject) {
    believesItsKeychain ||= request.sessionRetry === true;
    let ctx: Context;
    try { ctx = await opened(request); }
    catch (error) {
        if (error instanceof ProtocolError && error.code === 'keychain_unavailable' && sessionUnread(false, believesItsKeychain)) throw new ProtocolError('session_unread');
        throw error;
    }
    if (sessionUnread(ctx.accountReferenceExists, believesItsKeychain)) throw new ProtocolError('session_unread');
    return ctx;
}
async function* requestLines(): AsyncGenerator<Buffer> {
    let pending: Buffer = Buffer.alloc(0);
    for await (const chunk of process.stdin) {
        pending = Buffer.concat([pending, Buffer.from(chunk)]);
        for (;;) {
            const end = pending.indexOf(10), line = end < 0 ? pending : pending.subarray(0, end);
            // A request past the bound is refused, and nothing after it is read.
            if (line.length > MAX_REQUEST_BYTES) { yield line; return; }
            if (end < 0) break;
            yield line; pending = pending.subarray(end + 1);
        }
    }
}
async function run(request: JsonObject): Promise<JsonObject> {
    if (request.method === 'remove') throw new ProtocolError('unsupported_safe_removal');
    if (request.method === 'storeSharePassword') {
        await privateDataDirectory();
        const options = request.options;
        try {
            if (options.reference === request.destination.credentials.reference) throw new ProtocolError('reference_in_use');
            const serialized = JSON.stringify({ schemaVersion: 1, kind: 'share-password', value: options.password });
            const stored = await keychain(() => Bun.secrets.get({ service: SECRET_SERVICE, name: options.reference }));
            if (stored !== null && stored !== undefined && stored !== serialized) throw new ProtocolError('reference_in_use');
            if (stored === null || stored === undefined)
                await keychain(() => Bun.secrets.set({ service: SECRET_SERVICE, name: options.reference, value: serialized }));
            options.password = undefined;
            return { reference: options.reference };
        } catch (error) {
            if (error instanceof ProtocolError) throw error;
            throw new ProtocolError('keychain_unavailable');
        } finally { options.password = undefined; }
    }
    if (request.method === 'warm' && !sessions) throw new ProtocolError('unsupported_request');
    const ctx = await context(request);
    // What an upload prepared serves the link that follows it, and nothing else.
    if (request.method !== 'share') { ctx.linkUnused(); uploadedHere = undefined; }
    // A session started ahead of its first request signs in, opens its connections and reads
    // the account. It changes nothing, and the requests after it still make their own reads.
    if (request.method === 'warm') {
        if (!request.destination.settings.folderUid) await accountRoot(ctx, request, !!request.destination.settings.accountRootUid);
        else { await destinationFolder(ctx, request); readDestinations.add(destinationKey(request)); }
        return {};
    }
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
function deadlineError(): JsonObject {
    const error = failure('request_timeout');
    error.details = { reason: 'deadline', timeoutMs: activeRequest ? requestTimeout(activeRequest) : 120000,
        method: activeRequest?.method ?? 'request' };
    return error;
}
async function timeout() {
    deadlineExpired = true;
    controller.abort();
    const method = activeRequest?.method;
    if (mutationStarted && (method === 'upload' || method === 'share')) {
        const result = method === 'share' ? shareOutcome(receipt(activeRequest!), activeRequest!.options.action, '', 'unknown') : status('unknown');
        result.error = deadlineError();
        await writeFinal(success(result));
    } else await writeFinal({ ok: false, error: deadlineError() });
    process.exit(0);
}
// Reads, runs and answers one request. `wait` bounds the read; the request's own deadline
// bounds the rest. A passed deadline answers and ends the process (see `timeout`).
async function serve(read: () => Promise<JsonObject>, wait?: number) {
    let timer = wait === undefined ? undefined : setTimeout(timeout, wait);
    try {
        activeRequest = await read();
        clearTimeout(timer);
        timer = setTimeout(timeout, requestTimeout(activeRequest));
        const result = await run(activeRequest);
        clearTimeout(timer);
        await writeFinal(success(result));
    } catch (error) {
        clearTimeout(timer);
        await writeFinal({ ok: false, error: deadlineExpired ? deadlineError() : serviceFailure(error, mutationStarted ? 'operation_unknown' : 'operation_failed') });
    } finally { clearTimeout(timer); controller.abort(); }
}
if (import.meta.main) {
if (process.argv.length === 3 && process.argv[2] === '--version') {
    await writeJson({ schemaVersion: 1, runtime: 'putgd-proton-runtime', appVersion: APP_VERSION,
        sdkVersion: SDK_VERSION, bunVersion: Bun.version, protocolVersion: 1 });
} else if (process.argv.length === 3 && process.argv[2] === '--stdio') {
    await serve(readRequest, 10000);
    process.exit(0);
} else if (process.argv.length === 3 && process.argv[2] === '--session') {
    sessions = true;
    for await (const line of requestLines()) {
        controller = new AbortController(); mutationStarted = false; finalWrite = undefined;
        activeRequest = undefined; deadlineExpired = false; uploadProgress = undefined;
        await serve(async () => {
            const request = parseRequest(line);
            if (!SESSION_METHODS.has(request.method)) throw new ProtocolError('unsupported_request');
            return request;
        });
    }
    process.exit(0);
} else {
    await writeFinal({ ok: false, error: failure('unsupported_request') }); process.exitCode = 2;
}
}
