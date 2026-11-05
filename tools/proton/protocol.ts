// put.gd's one-request protocol. No provider exception text crosses this boundary.
import { createHash } from 'node:crypto';
export const MAX_REQUEST_BYTES = 128 * 1024;
export const MAX_REQUEST_TIMEOUT_MS = 86400000;
export function requestTimeout(request: JsonObject): number {
    const timeout = request.method === 'connect' ? 600000 : request.requestTimeoutMs ?? request.destination.settings.requestTimeoutMs ?? 120000;
    if (!Number.isInteger(timeout) || timeout < 1000 || timeout > MAX_REQUEST_TIMEOUT_MS) throw new ProtocolError('invalid_request');
    return timeout;
}
export function publicationClientUid(request: JsonObject): string | undefined {
    if (request.method !== 'upload' && request.method !== 'reconcile') return undefined;
    return 'putgd-' + createHash('sha256').update(JSON.stringify([text(request.destination.credentials.reference), text(request.publicationId)])).digest('hex');
}
export const METHODS = new Set(['connect', 'inspectAccount', 'createFolder', 'storeSharePassword',
    'checkName', 'upload', 'stat', 'reconcile', 'share', 'remove']);
export type JsonObject = Record<string, any>;
export class ProtocolError extends Error {
    constructor(public readonly code: string) { super(code); }
}
export function object(value: unknown): JsonObject {
    if (!value || typeof value !== 'object' || Array.isArray(value)) throw new ProtocolError('invalid_request');
    return value as JsonObject;
}
export function text(value: unknown, max = 512): string {
    if (typeof value !== 'string' || !value || value.length > max || /[\x00-\x1f\x7f]/.test(value))
        throw new ProtocolError('invalid_request');
    return value;
}
export function name(value: unknown): string {
    const result = text(value, 255);
    if (result === '.' || result === '..' || /[\\/]/.test(result) || Buffer.byteLength(result) > 255)
        throw new ProtocolError('invalid_request');
    return result;
}
export function hash(value: unknown): string {
    if (typeof value !== 'string' || !/^[a-f0-9]{64}$/.test(value)) throw new ProtocolError('invalid_request');
    return value;
}
export function days(value: unknown): number {
    if (!Number.isInteger(value) || Number(value) < 0 || Number(value) > 365) throw new ProtocolError('invalid_request');
    return Number(value);
}
export function parseRequest(bytes: Uint8Array): JsonObject {
    if (!bytes.length || bytes.length > MAX_REQUEST_BYTES) throw new ProtocolError('invalid_request');
    let result;
    try { result = object(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes))); }
    catch { throw new ProtocolError('invalid_request'); }
    if (result.schemaVersion !== 1 || !METHODS.has(result.method)) throw new ProtocolError('unsupported_request');
    const destination = object(result.destination);
    if (destination.providerId !== 'proton-drive') throw new ProtocolError('invalid_destination');
    if (Object.keys(destination).some(key => !['providerId', 'profileId', 'profileRevision', 'settings', 'credentials'].includes(key)))
        throw new ProtocolError('invalid_destination');
    text(destination.profileId); text(destination.profileRevision);
    const settings = object(destination.settings);
    if (Object.keys(settings).some(key => !['accountRootUid', 'folderUid', 'shareByDefault', 'linkExpirationDays',
        'sharePasswordReference', 'requestTimeoutMs', 'folderName'].includes(key))) throw new ProtocolError('invalid_destination');
    for (const field of ['accountRootUid', 'folderUid'])
        if (settings[field] !== undefined && (typeof settings[field] !== 'string' || settings[field].length > 512 ||
            /[\x00-\x1f\x7f]/.test(settings[field]))) throw new ProtocolError('invalid_destination');
    if (settings.folderName !== undefined) {
        text(settings.folderName, 255);
        if (Buffer.byteLength(settings.folderName) > 255 || !settings.folderUid) throw new ProtocolError('invalid_destination');
    }
    if (settings.shareByDefault !== undefined && typeof settings.shareByDefault !== 'boolean') throw new ProtocolError('invalid_destination');
    if (settings.linkExpirationDays !== undefined) days(settings.linkExpirationDays);
    if (settings.sharePasswordReference !== undefined && settings.sharePasswordReference !== null) text(settings.sharePasswordReference, 256);
    const credentials = object(destination.credentials);
    if (credentials.mode !== 'keychain') throw new ProtocolError('keychain_required');
    if (Object.keys(credentials).some(key => !['mode', 'reference'].includes(key))) throw new ProtocolError('keychain_required');
    text(credentials.reference);
    if (result.method === 'storeSharePassword') {
        const options = object(result.options);
        text(options.reference);
        if (typeof options.password !== 'string' || !options.password || Buffer.byteLength(options.password) > 1024 ||
            /[\x00]/.test(options.password)) throw new ProtocolError('invalid_request');
    } else if (result.options?.password !== undefined) throw new ProtocolError('keychain_required');
    return result;
}
export function configured(request: JsonObject): { accountRootUid: string; folderUid: string } {
    const settings = request.destination.settings;
    return { accountRootUid: text(settings.accountRootUid), folderUid: text(settings.folderUid) };
}
export function reference(request: JsonObject, nodeUid?: string, revisionUid?: string): JsonObject {
    return { ...configured(request), name: name(request.candidate),
        ...(nodeUid ? { nodeUid } : {}), ...(revisionUid ? { revisionUid } : {}) };
}
export function makeReceipt(request: JsonObject, nodeUid: string, revisionUid: string): JsonObject {
    return { schemaVersion: 1, destination: request.destination,
        objectReference: { ...reference(request, text(nodeUid), text(revisionUid)), sha256: hash(request.artifact.sha256) },
        concurrencyToken: revisionUid, publicationId: text(request.publicationId), directUrl: '' };
}
export function receipt(request: JsonObject): JsonObject {
    const value = object(request.receipt);
    if (value.schemaVersion !== 1) throw new ProtocolError('invalid_receipt');
    const ref = object(value.objectReference);
    const expected = configured(request);
    const destination = object(value.destination);
    if (destination.providerId !== 'proton-drive' || destination.profileId !== request.destination.profileId ||
        destination.profileRevision !== request.destination.profileRevision ||
        destination.credentials?.reference !== request.destination.credentials.reference ||
        destination.settings?.accountRootUid !== expected.accountRootUid ||
        destination.settings?.folderUid !== expected.folderUid || ref.accountRootUid !== expected.accountRootUid ||
        ref.folderUid !== expected.folderUid) throw new ProtocolError('invalid_receipt');
    name(ref.name); text(ref.nodeUid); text(ref.revisionUid); hash(ref.sha256); text(value.publicationId);
    if (value.concurrencyToken !== ref.revisionUid) throw new ProtocolError('invalid_receipt');
    if (value.directUrl !== undefined && (typeof value.directUrl !== 'string' || value.directUrl.length > 8192 ||
        /[\x00-\x1f\x7f]/.test(value.directUrl))) throw new ProtocolError('invalid_receipt');
    // Canonicalize retained input; unknown fields in an incoming receipt cannot echo secrets.
    return { schemaVersion: 1, destination: request.destination,
        objectReference: { ...expected, name: ref.name, nodeUid: ref.nodeUid, revisionUid: ref.revisionUid, sha256: ref.sha256 },
        concurrencyToken: ref.revisionUid, publicationId: value.publicationId, directUrl: value.directUrl ?? '' };
}
// Pure identity check used by stat and sharing. Decryption failure is never ownership evidence.
export function classifyNode(node: JsonObject, ref: JsonObject, publicationId: string): 'own' | 'foreign' | 'unknown' {
    if (node.name?.ok !== true || node.keyAuthor?.ok !== true || node.nameAuthor?.ok !== true ||
        node.activeRevision?.contentAuthor?.ok !== true || node.errors?.length) return 'unknown';
    if (node.uid !== ref.nodeUid || node.parentUid !== ref.folderUid || node.name.value !== ref.name ||
        node.type !== 'file' || node.trashTime || node.activeRevision?.uid !== ref.revisionUid) return 'foreign';
    const marker = node.activeRevision?.claimedAdditionalMetadata?.putgd;
    if (!marker || marker.publicationId !== publicationId || marker.sha256 !== ref.sha256) return 'foreign';
    return 'own';
}
// A fresh check after sharing prevents returning a capability for a changed revision.
export function shareOutcome(proof: JsonObject, action: string, directUrl: string, ownership: string,
    expirationTime?: Date, now = Date.now()): JsonObject {
    if (expirationTime !== undefined) {
        if (!(expirationTime instanceof Date) || !Number.isFinite(expirationTime.getTime())) ownership = 'unknown';
        else if (expirationTime.getTime() <= now) directUrl = '';
    }
    if (ownership !== 'own') return { status: ownership === 'foreign' || ownership === 'missing' ? 'changed' : 'unknown',
        receipt: { ...proof, directUrl: '' }, linkAvailable: false, progress: { schemaVersion: 1 }, error: {} };
    // The service's confirmed expiry, kept after it passes so the app can say the link expired (#253).
    const expiresAt = expirationTime && action !== 'revoke' ? { expiresAt: expirationTime.toISOString() } : {};
    return { status: directUrl ? 'shared' : action === 'revoke' ? 'revoked' : 'private',
        receipt: { ...proof, directUrl }, linkAvailable: !!directUrl, ...expiresAt, progress: { schemaVersion: 1 }, error: {} };
}
export function connectAction(loggedIn: boolean, referenceExists: boolean): 'inspect' | 'authenticate' | 'new_reference_required' {
    return loggedIn ? 'inspect' : referenceExists ? 'new_reference_required' : 'authenticate';
}
export function failure(code: string): JsonObject {
    const messages: Record<string, string> = {
        permission_denied: 'Proton denied access. Check this account and folder permissions before retrying.',
        quota_exceeded: 'Proton reports a storage or sharing limit. Check account capacity and limits before retrying.',
        destination_file_too_large_for_plan: 'This file is larger than your Proton plan allows for a single file. Upload a smaller file, or upgrade the Proton plan, then Retry.',
        rate_limited: 'Proton limited requests. Wait before explicitly inspecting or retrying the saved operation.',
        invalid_request: 'The Proton request is invalid.', unsupported_request: 'The Proton request is unsupported.',
        invalid_destination: 'The Proton destination is invalid.', keychain_required: 'A private keychain reference is required.',
        invalid_receipt: 'The Proton receipt does not match this destination.', authentication_required: 'The Proton session is unavailable. Reconnect this account before explicitly inspecting or retrying the saved operation.',
        account_changed: 'The connected Proton account has changed.', destination_unavailable: 'The selected Proton folder is unavailable.',
        unsupported_safe_removal: 'Proton does not support exact guarded removal.', unsupported_condition: 'Proton supports new files only.',
        unsafe_data_directory: 'The Proton runtime requires a private data directory.', reference_in_use: 'The keychain reference is already in use.',
        keychain_unavailable: 'The private keychain is unavailable.', artifact_changed: 'The staged artifact has changed.',
        account_reference_in_use: 'Use a new account reference to connect a replacement account.',
        upload_draft_conflict: 'An unfinished upload reserves this name. Retry can use another name under the configured rename policy.',
        operation_failed: 'The Proton operation could not be completed.', operation_unknown: 'The Proton operation outcome is unknown.',
        folder_creation_unknown: 'The folder creation outcome is unknown.', request_timeout: 'Proton exceeded the configured request deadline; reconcile its saved identity before retrying.',
    };
    return { code: code in messages ? code : 'operation_failed', message: messages[code] ?? messages.operation_failed, details: code === 'request_timeout' ? { reason: 'deadline' } : {} };
}
