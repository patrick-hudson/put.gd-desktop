// Account plumbing comes from Proton's pinned official CLI; initialization is put.gd's.
import { lstat, realpath } from 'node:fs/promises';
import path from 'node:path';
import { CryptoProxy } from '@protontech/crypto';
import { Api as CryptoApi } from '@protontech/crypto/proxy/endpoint/api.ts';
import { computeKeyPassword, generateKeySalt, getRandomSrpVerifier, getSrp } from '@protontech/crypto/srp';
import { FeatureFlags, ProtonDriveClient, OpenPGPCryptoWithCryptoProxy, type ProtonDriveCryptoCache } from '@protontech/drive-sdk';
import { Telemetry } from '@protontech/drive-sdk/telemetry';
import { ApiClient, initAccount, type AccountApi } from 'proton-drive-sdk-account';
import { Credentials } from './upstream/cli/src/credentials/credentials';
import { SecretsSessionStore } from './upstream/cli/src/credentials/secretCredentialsStore';
import { parseStoredSnapshot } from './upstream/cli/src/credentials/parseCredentials';
import { configureCredentialsReference } from './upstream/cli/src/credentials/constants';
import { HTTPClient } from './upstream/cli/src/api/httpClient';
import { DriveAccountAdapter } from './upstream/cli/src/api/driveAccountAdapter';
import { getOrGenerateClientUid } from './upstream/cli/src/clientUid';
import { processDriveRequirementHeaders, SUPPORTED_REQUIREMENT_MASK_BY_SCOPE } from './upstream/cli/src/api/apiRequirements';
import { createServerTimeAfterResponseHook } from './upstream/cli/src/api/serverTimeHook';
import { ProtocolError, publicationClientUid, type JsonObject } from './protocol';
import { RequestCache } from './crypto-cache';
export const APP_VERSION = 'external-drive-put_gd@0.1.1-alpha';
export const SDK_VERSION = 'js@0.23.0';
export const SECRET_SERVICE = 'ch.proton.drive/putgd';
export async function privateDataDirectory(): Promise<string> {
    const input = process.env.PUTGD_PROTON_DATA_DIR;
    if (!input || !path.isAbsolute(input)) throw new ProtocolError('unsafe_data_directory');
    const info = await lstat(input);
    if (!info.isDirectory() || info.isSymbolicLink() || info.uid !== process.getuid?.() || (info.mode & 0o077) ||
        await realpath(input) !== path.resolve(input)) throw new ProtocolError('unsafe_data_directory');
    return input;
}
export function createCryptoCache(): ProtonDriveCryptoCache { return new RequestCache(); }
// A refused session write (no default keyring collection, or a locked one) is a
// keychain failure, not a generic operation failure (#252). libsecret stores
// through the Secret Service 'default' alias; the provider decides its target.
export async function saveToKeychain(save: () => Promise<void>): Promise<void> {
    try { await save(); }
    catch { throw new ProtocolError('keychain_unavailable'); }
}
export function accountResponseHooks(onUnsupported: () => never) {
    return [createServerTimeAfterResponseHook(), (_request: Request, _options: unknown, response: Response) => {
        processDriveRequirementHeaders(response.headers, { clientSdkVersion: SDK_VERSION,
            supportedRequirementMasksByScope: SUPPORTED_REQUIREMENT_MASK_BY_SCOPE,
            onUnsupportedFeature: onUnsupported, onRequiredUpdate: onUnsupported,
            onSuggestedUpdate: () => {} });
    }];
}
// Starts an account read now and answers the account code's next call for it with that
// read, so it overlaps the requests made before its answer is needed. The returned
// function restores the direct call when nothing asked.
function ahead(api: AccountApi, name: 'addresses' | 'modulus'): () => void {
    const direct = api[name].bind(api) as () => Promise<never>, early = direct();
    early.catch(() => {});
    const restore = () => { (api as any)[name] = direct; };
    (api as any)[name] = () => { restore(); return early; };
    return restore;
}
// Every Drive client first asks for My files, then for its root share. A session makes a
// client per publication, so the first good answer to each serves the process's later
// clients; they are the same account's, and no other read is kept.
type JsonReads = Pick<HTTPClient, 'fetchJson' | 'fetchBlob'>;
export class SharedAccountReads implements JsonReads {
    private readonly answers = new Map<string, { body: string; headers: [string, string][] }>();
    private rootShare: string | undefined;
    constructor(private readonly inner: JsonReads) {}
    fetchBlob(options: Parameters<HTTPClient['fetchBlob']>[0]) { return this.inner.fetchBlob(options); }
    async fetchJson(options: Parameters<HTTPClient['fetchJson']>[0]): Promise<Response> {
        const path = options.method === 'GET' ? new URL(options.url).pathname : '';
        const myFiles = path.endsWith('/drive/v2/shares/my-files');
        if (!myFiles && (this.rootShare === undefined || !path.endsWith('/drive/shares/' + this.rootShare))) return this.inner.fetchJson(options);
        const known = this.answers.get(path);
        if (known) return new Response(known.body, { status: 200, headers: known.headers });
        const response = await this.inner.fetchJson(options);
        if (response.status !== 200) return response;
        const body = await response.text(), headers = [...response.headers];
        try {
            const share = myFiles ? JSON.parse(body)?.Share?.ShareID : undefined;
            if (!myFiles || (typeof share === 'string' && /^[A-Za-z0-9_=-]+$/.test(share))) {
                if (myFiles) this.rootShare = share;
                this.answers.set(path, { body, headers });
            }
        } catch { /* An unreadable answer is passed on and never kept. */ }
        return new Response(body, { status: 200, headers });
    }
}
// The signed-in account: its session from the keychain and the API client that refreshes it.
export async function openAccount(request: JsonObject) {
    const dataDir = await privateDataDirectory();
    configureCredentialsReference(request.destination.credentials.reference);
    // Explicit empty handlers disable console, file logging, metrics and telemetry before auth.
    const telemetry = new Telemetry({ logHandlers: [], metricHandlers: [] });
    const logger = telemetry.getLogger('putgd');
    CryptoApi.init({});
    CryptoProxy.setEndpoint(new CryptoApi(), (endpoint) => endpoint.clearKeyStore());
    const store = new SecretsSessionStore(logger);
    let accountReferenceExists = false;
    const credentials = new Credentials({
        load: async () => {
            const raw = await Bun.secrets.get({ service: SECRET_SERVICE, name: request.destination.credentials.reference });
            accountReferenceExists = raw !== null;
            return parseStoredSnapshot(raw);
        },
        save: snapshot => saveToKeychain(() => store.save(snapshot)), remove: () => store.remove(),
    }, logger);
    const requirementFailure = () => { throw new ProtocolError('unsupported_request'); };
    const apiClient = new ApiClient({ baseUrl: 'drive-api.proton.me', appVersion: APP_VERSION, credentials, logger,
        headers: { 'x-pm-drive-sdk-version': SDK_VERSION }, afterResponseHooks: accountResponseHooks(requirementFailure) });
    let account;
    try {
        account = await initAccount({ authClientId: 'external-drive', apiClient, credentials, cryptoProxy: CryptoProxy,
            srpApi: { computeKeyPassword, generateKeySalt, getRandomSrpVerifier, getSrp }, logger,
            accountUrl: 'account.proton.me' });
    } catch { throw new ProtocolError('keychain_unavailable'); }
    let warmed = false;
    return { dataDir, telemetry, logger, http: new SharedAccountReads(new HTTPClient(apiClient)), account, referenceExists: () => accountReferenceExists,
        // Loads the user and address keys every request needs beside its first Drive read.
        // The account code asks for the user, then the addresses: both reads start at once.
        warm: () => {
            if (warmed) return;
            warmed = true;
            ahead(account.accountApi, 'addresses');
            void account.addresses.getOwnPrimaryAddress().catch(() => {});
        },
        // A new link's password verifier needs the SRP modulus: read it beside the share's creation.
        modulusAhead: () => ahead(account.accountApi, 'modulus') };
}
// A Drive client for the account. Uploads use their publication's client identity, so a
// retry may replace its own unfinished draft and nobody else's.
export async function openClient(opened: Awaited<ReturnType<typeof openAccount>>, request: JsonObject) {
    const { dataDir, telemetry, logger, http, account } = opened;
    const clientUid = publicationClientUid(request) ?? await getOrGenerateClientUid({ appDir: dataDir, clientUidPrefix: 'putgd' } as any, logger);
    const entities = new RequestCache<string>();
    const sdk = new ProtonDriveClient({ config: { baseUrl: 'drive-api.proton.me', clientUid },
        httpClient: http, telemetry, account: new DriveAccountAdapter(account.addresses),
        srpModule: account.srp, openPGPCryptoModule: new OpenPGPCryptoWithCryptoProxy(CryptoProxy),
        // No persisted entities, decrypted keys or names: both caches live in this process's
        // memory. Ownership and destination checks read past the entity cache (`fresh`).
        entitiesCache: entities, cryptoCache: createCryptoCache(),
        // One request carries a file under 128 KiB, as in Proton's own CLI at this pin.
        featureFlagProvider: { isEnabled: async flag => flag === FeatureFlags.DriveSmallFileUpload } });
    return { sdk, auth: account.auth, accountReferenceExists: opened.referenceExists(),
        fresh: <R>(read: () => Promise<R>) => entities.fresh(read), warm: opened.warm, modulusAhead: opened.modulusAhead };
}
export async function initialize(request: JsonObject) { return openClient(await openAccount(request), request); }
