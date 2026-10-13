import { expect, test } from 'bun:test';
import { MAX_REQUEST_BYTES, ProtocolError, classifyNode, connectAction, failure, makeReceipt, parseRequest, receipt, shareOutcome } from './protocol';
import { MemberRole, NodeType, RevisionState, type NodeEntity } from '@protontech/drive-sdk';
const destination = { providerId: 'proton-drive', profileId: 'profile', profileRevision: 'revision',
    settings: { accountRootUid: 'root', folderUid: 'folder' }, credentials: { mode: 'keychain', reference: 'private-ref' } };
const request = { schemaVersion: 1, method: 'upload', destination, candidate: 'photo.png', publicationId: 'publication',
    artifact: { sha256: 'a'.repeat(64) } };
const encode = (value: unknown) => Buffer.from(JSON.stringify(value));
function sdkNode(): NodeEntity {
    return { uid: 'file-node', parentUid: 'folder', type: NodeType.File, name: { ok: true, value: 'photo.png' },
        keyAuthor: { ok: true, value: 'synthetic@example.invalid' }, nameAuthor: { ok: true, value: 'synthetic@example.invalid' },
        directRole: MemberRole.Admin, ownedBy: { email: 'synthetic@example.invalid' }, isShared: false, isSharedByUrl: false,
        creationTime: new Date(0), modificationTime: new Date(0), treeEventScopeId: 'scope',
        activeRevision: { uid: 'file-revision', state: RevisionState.Active,
            creationTime: new Date(0), storageSize: 1, isImported: false,
            contentAuthor: { ok: true, value: 'synthetic@example.invalid' },
            claimedAdditionalMetadata: { putgd: { publicationId: 'publication', sha256: 'a'.repeat(64) } } } };
}
test('single bounded JSON and strict UTF-8, provider and keychain references', () => {
    expect(parseRequest(encode(request))).toEqual(request);
    for (const data of [Buffer.from('{}{}'), Buffer.from([0xff]), new Uint8Array(MAX_REQUEST_BYTES + 1),
        encode({ ...request, schemaVersion: 2 }), encode({ ...request, destination: { ...destination, credentials: { mode: 'unsafe_file' } } })])
        expect(() => parseRequest(data)).toThrow(ProtocolError);
});
test('raw passwords accepted only by storeSharePassword', () => {
    expect(() => parseRequest(encode({ ...request, options: { password: 'test-only' } }))).toThrow();
    expect(() => parseRequest(encode({ ...request, destination: { ...destination,
        credentials: { mode: 'keychain', reference: 'private-ref', password: 'test-only' } } }))).toThrow();
    expect(() => parseRequest(encode({ ...request, destination: { ...destination,
        settings: { ...destination.settings, password: 'test-only' } } }))).toThrow();
    expect(parseRequest(encode({ ...request, method: 'storeSharePassword', options: { reference: 'password-ref', password: 'test-only' } })).method)
        .toBe('storeSharePassword');
});
test('upload receipt uses exact completion identities and opaque checksum', () => {
    const proof = makeReceipt(request, 'file-node', 'file-revision');
    expect(proof.objectReference).toEqual({ accountRootUid: 'root', folderUid: 'folder', name: 'photo.png',
        nodeUid: 'file-node', revisionUid: 'file-revision', sha256: 'a'.repeat(64) });
    expect(receipt({ ...request, receipt: proof })).toEqual(proof);
    expect(receipt({ ...request, receipt: { ...proof, password: 'synthetic-secret',
        objectReference: { ...proof.objectReference, password: 'synthetic-secret' } } })).toEqual(proof);
    expect(() => receipt({ ...request, receipt: { ...proof, concurrencyToken: 'other' } })).toThrow();
    expect(() => receipt({ ...request, destination: { ...destination, profileRevision: 'new' }, receipt: proof })).toThrow();
});
test('ownership requires exact identity, revision and encrypted publication marker', () => {
    const proof = makeReceipt(request, 'file-node', 'file-revision');
    const node = sdkNode();
    expect(classifyNode(node, proof.objectReference, 'publication')).toBe('own');
    expect(classifyNode({ ...node, activeRevision: { ...node.activeRevision, uid: 'new-revision' } }, proof.objectReference, 'publication')).toBe('foreign');
    expect(classifyNode({ ...node, parentUid: 'elsewhere' }, proof.objectReference, 'publication')).toBe('foreign');
    expect(classifyNode(node, proof.objectReference, 'other')).toBe('foreign');
    expect(classifyNode({ ...node, errors: ['decryption'] }, proof.objectReference, 'publication')).toBe('unknown');
    expect(classifyNode({ ...node, name: { ok: false } }, proof.objectReference, 'publication')).toBe('unknown');
});
test('decrypted SDK markers without verified name/revision signatures are unknown', () => {
    const proof = makeReceipt(request, 'file-node', 'file-revision');
    const node = sdkNode();
    // SDK decryptRevision preserves claimed metadata on failed signature verification,
    // without necessarily populating node.errors.
    const rejectedAuthor = { ok: false, error: { claimedAuthor: 'synthetic@example.invalid', error: 'Signature verification failed' } };
    expect(classifyNode({ ...node, activeRevision: { ...node.activeRevision, contentAuthor: rejectedAuthor } }, proof.objectReference, 'publication')).toBe('unknown');
    expect(classifyNode({ ...node, activeRevision: { ...node.activeRevision, contentAuthor: undefined } }, proof.objectReference, 'publication')).toBe('unknown');
    expect(classifyNode({ ...node, nameAuthor: rejectedAuthor }, proof.objectReference, 'publication')).toBe('unknown');
    expect(classifyNode({ ...node, nameAuthor: undefined }, proof.objectReference, 'publication')).toBe('unknown');
    expect(classifyNode({ ...node, keyAuthor: rejectedAuthor }, proof.objectReference, 'publication')).toBe('unknown');
    expect(classifyNode({ ...node, keyAuthor: undefined }, proof.objectReference, 'publication')).toBe('unknown');
});
test('fresh ownership failure after sharing suppresses every URL without rollback', () => {
    const proof = { ...makeReceipt(request, 'file-node', 'file-revision'), directUrl: 'https://synthetic.invalid/old#capability' };
    const url = 'https://synthetic.invalid/shared#intact-capability';
    for (const action of ['create', 'revoke', 'inspect']) {
        for (const ownership of ['foreign', 'missing', 'unknown']) {
            const outcome = shareOutcome(proof, action, url, ownership);
            expect(outcome.status).toBe(ownership === 'unknown' ? 'unknown' : 'changed');
            expect(outcome.receipt.directUrl).toBe('');
        }
    }
    expect(shareOutcome(proof, 'create', url, 'own').receipt.directUrl).toBe(url);
    expect(shareOutcome(proof, 'revoke', '', 'own').status).toBe('revoked');
});
test('reconnect preserves existing credentials and requires new reference for replacement', () => {
    expect(connectAction(true, true)).toBe('inspect');
    expect(connectAction(false, true)).toBe('new_reference_required');
    expect(connectAction(false, false)).toBe('authenticate');
});
test('only active unexpired SDK URL access is usable', () => {
    const proof = makeReceipt(request, 'file-node', 'file-revision');
    const url = 'https://synthetic.invalid/shared#intact-capability';
    const future = shareOutcome(proof, 'inspect', url, 'own', new Date(2000), 1000);
    expect(future.linkAvailable).toBe(true);
    expect(future.receipt.directUrl).toBe(url);
    for (const expiration of [new Date(999), new Date(1000)]) {
        const expired = shareOutcome(proof, 'inspect', url, 'own', expiration, 1000);
        expect(expired.status).toBe('private');
        expect(expired.linkAvailable).toBe(false);
        expect(expired.receipt.directUrl).toBe('');
    }
    expect(shareOutcome(proof, 'inspect', url, 'own', new Date(NaN), 1000).status).toBe('unknown');
});
test('sanitized errors never copy provider exception text', () => {
    const result = failure('https://secret.invalid/private-name?token=secret');
    expect(result).toEqual({ code: 'operation_failed', message: 'The Proton operation could not be completed.', details: {} });
});
