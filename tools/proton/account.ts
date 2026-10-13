// Account plumbing comes from Proton's pinned official CLI; initialization is put.gd's.
import { lstat, realpath } from 'node:fs/promises';
import path from 'node:path';
import { CryptoProxy } from '@protontech/crypto';
import { Api as CryptoApi } from '@protontech/crypto/proxy/endpoint/api.ts';
import { computeKeyPassword, generateKeySalt, getRandomSrpVerifier, getSrp } from '@protontech/crypto/srp';
import { ProtonDriveClient, OpenPGPCryptoWithCryptoProxy } from '@protontech/drive-sdk';
import { NullCache } from '@protontech/drive-sdk/cache';
import { Telemetry } from '@protontech/drive-sdk/telemetry';
import { ApiClient, initAccount } from 'proton-drive-sdk-account';
import { Credentials } from './upstream/cli/src/credentials/credentials';
import { SecretsSessionStore } from './upstream/cli/src/credentials/secretCredentialsStore';
import { parseStoredSnapshot } from './upstream/cli/src/credentials/parseCredentials';
import { configureCredentialsReference } from './upstream/cli/src/credentials/constants';
import { HTTPClient } from './upstream/cli/src/api/httpClient';
import { DriveAccountAdapter } from './upstream/cli/src/api/driveAccountAdapter';
import { getOrGenerateClientUid } from './upstream/cli/src/clientUid';
import { processDriveRequirementHeaders, SUPPORTED_REQUIREMENT_MASK_BY_SCOPE } from './upstream/cli/src/api/apiRequirements';
import { ProtocolError, type JsonObject } from './protocol';
export const APP_VERSION = 'external-drive-put_gd@0.1.1-alpha';
export const SDK_VERSION = 'js@0.22.2';
export const SECRET_SERVICE = 'ch.proton.drive/putgd';
export async function privateDataDirectory(): Promise<string> {
    const input = process.env.PUTGD_PROTON_DATA_DIR;
    if (!input || !path.isAbsolute(input)) throw new ProtocolError('unsafe_data_directory');
    const info = await lstat(input);
    if (!info.isDirectory() || info.isSymbolicLink() || info.uid !== process.getuid?.() || (info.mode & 0o077) ||
        await realpath(input) !== path.resolve(input)) throw new ProtocolError('unsafe_data_directory');
    return input;
}
export async function initialize(request: JsonObject) {
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
        save: snapshot => store.save(snapshot), remove: () => store.remove(),
    }, logger);
    const requirementFailure = () => { throw new ProtocolError('unsupported_request'); };
    const apiClient = new ApiClient({ baseUrl: 'drive-api.proton.me', appVersion: APP_VERSION, credentials, logger,
        headers: { 'x-pm-drive-sdk-version': SDK_VERSION }, afterResponseHooks: [(_request, _options, response) => {
            processDriveRequirementHeaders(response.headers, { clientSdkVersion: SDK_VERSION,
                supportedRequirementMasksByScope: SUPPORTED_REQUIREMENT_MASK_BY_SCOPE,
                onUnsupportedFeature: requirementFailure, onRequiredUpdate: requirementFailure,
                onSuggestedUpdate: () => {} });
        }] });
    let account;
    try {
        account = await initAccount({ authClientId: 'external-drive', apiClient, credentials, cryptoProxy: CryptoProxy,
            srpApi: { computeKeyPassword, generateKeySalt, getRandomSrpVerifier, getSrp }, logger,
            accountUrl: 'account.proton.me' });
    } catch { throw new ProtocolError('keychain_unavailable'); }
    const clientUid = await getOrGenerateClientUid({ appDir: dataDir, clientUidPrefix: 'putgd' } as any, logger);
    const sdk = new ProtonDriveClient({ config: { baseUrl: 'drive-api.proton.me', clientUid },
        httpClient: new HTTPClient(apiClient), telemetry, account: new DriveAccountAdapter(account.addresses),
        srpModule: account.srp, openPGPCryptoModule: new OpenPGPCryptoWithCryptoProxy(CryptoProxy),
        // No persisted entities, decrypted keys or names. Every read reaches current server state.
        entitiesCache: new NullCache(), cryptoCache: new NullCache(),
        featureFlagProvider: { isEnabled: async () => false } });
    return { sdk, auth: account.auth, accountReferenceExists };
}
