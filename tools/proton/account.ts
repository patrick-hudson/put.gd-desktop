// Account plumbing comes from Proton's pinned official CLI; initialization is put.gd's.
import { lstat, realpath } from 'node:fs/promises';
import path from 'node:path';
import http2 from 'node:http2';
import zlib from 'node:zlib';
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
// A keychain read or write is an exchange with the desktop's secret service, and a new client
// can lose it through no fault of the entry (#1418): on KDE's service about one new process
// in a hundred had a read refused ("Client public key size is invalid") or answered with
// nothing, the entry being there all along. A refused exchange made again in the same
// process succeeds, so it is made again a few times before the refusal is believed. An
// empty answer is not helped by that, it stays empty for the life of the process: see
// `sessionUnread` in runtime.ts for what is done about it.
export const KEYCHAIN_PAUSES_MS = [30, 90, 240];
export async function keychain<T>(exchange: () => Promise<T>, pauses: readonly number[] = KEYCHAIN_PAUSES_MS): Promise<T> {
    for (let attempt = 0; ; attempt++) {
        try { return await exchange(); }
        catch (error) { if (attempt >= pauses.length) throw error; }
        await new Promise(resolve => setTimeout(resolve, pauses[attempt]));
    }
}
// A refused session write (no default keyring collection, or a locked one) is a
// keychain failure, not a generic operation failure (#252). libsecret stores
// through the Secret Service 'default' alias; the provider decides its target.
// A session that was refreshed and not saved is lost: Proton has replaced its refresh token.
export async function saveToKeychain(save: () => Promise<void>): Promise<void> {
    try { await keychain(save); }
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
// Every Drive client asks for My files first and for its root share before it signs or
// shares anything. A session makes a client per publication, so the first good answer to
// each serves the process's later clients; they are the same account's, and no other read
// is kept. The root share is asked for as soon as My files names it, beside whatever the
// client does next, instead of in line before an upload.
type JsonReads = Pick<HTTPClient, 'fetchJson' | 'fetchBlob'>;
type KeptAnswer = { body: string; headers: [string, string][] };
export class SharedAccountReads implements JsonReads {
    private readonly answers = new Map<string, Promise<KeptAnswer | undefined>>();
    private rootShare: string | undefined;
    // Called with the host a file's bytes are about to be sent to: once per transfer request.
    onTransfer: ((host: string) => void) | undefined;
    // Called when a request that creates a share is made.
    onShareCreate: (() => void) | undefined;
    constructor(private readonly inner: JsonReads) {}
    private transfer(url: string) {
        try { this.onTransfer?.(new URL(url).host); } catch { /* Never part of the transfer. */ }
    }
    fetchBlob(options: Parameters<HTTPClient['fetchBlob']>[0]) {
        if (options.method !== 'GET') this.transfer(options.url);
        return this.inner.fetchBlob(options);
    }
    async fetchJson(options: Parameters<HTTPClient['fetchJson']>[0]): Promise<Response> {
        // A file small enough for one request travels as that request's body.
        if (options.body !== undefined && options.json === undefined && options.method !== 'GET') this.transfer(options.url);
        if (options.method === 'POST' && this.onShareCreate && /\/drive\/volumes\/[^/]+\/shares$/.test(new URL(options.url).pathname)) this.onShareCreate();
        const path = options.method === 'GET' ? new URL(options.url).pathname : '';
        const myFiles = path.endsWith('/drive/v2/shares/my-files');
        if (!myFiles && (this.rootShare === undefined || !path.endsWith('/drive/shares/' + this.rootShare))) return this.inner.fetchJson(options);
        const answer = (kept: KeptAnswer) => new Response(kept.body, { status: 200, headers: kept.headers });
        const under = this.answers.get(path);
        if (under) {
            // Another caller's read: its good answer serves this one, anything else is asked again.
            const kept = await under.catch(() => undefined);
            return kept ? answer(kept) : this.inner.fetchJson(options);
        }
        let passed: Response | undefined;
        const reading = (async () => {
            const response = await this.inner.fetchJson(options);
            if (response.status !== 200) { passed = response; return undefined; }
            const kept = { body: await response.text(), headers: [...response.headers] };
            if (!myFiles) return kept;
            let share: unknown;
            try { share = JSON.parse(kept.body)?.Share?.ShareID; } catch { /* Passed on unread below. */ }
            if (typeof share !== 'string' || !/^[A-Za-z0-9_=-]+$/.test(share)) { passed = answer(kept); return undefined; }
            this.rootShare = share;
            // Its own read, outliving the request that happened to start it.
            void this.fetchJson({ ...options, url: options.url.replace(/\/drive\/v2\/shares\/my-files$/, '/drive/shares/' + share), signal: undefined }).catch(() => {});
            return kept;
        })();
        this.answers.set(path, reading);
        try {
            const kept = await reading;
            if (kept) return answer(kept);
        } catch (error) { this.answers.delete(path); throw error; }
        this.answers.delete(path);
        return passed!;
    }
}
// A new link costs about half a second of local work before its request can be sent: two
// bcrypt hashes of its password, a read of the SRP modulus, and the share's session key
// wrapped under the password by an iterated hash of 65 MB. Done in line, nothing is in
// transit meanwhile. `LinkAhead` moves that work beside requests that are:
//  - `prepare` hashes a password drawn ahead while an upload's bytes are on their way;
//  - `wrapAhead` wraps the share's session key while the share's own request is in transit.
// The SDK's next link is handed that password (`take`) and salt, and gets these answers for
// exactly those inputs. Anything else (a password of the user's own added to it, a link made
// later, a failed preparation) is computed in line, as without it. Nothing outlives the link.
type SrpModule = Pick<Awaited<ReturnType<typeof initAccount>>['srp'], 'getSrp' | 'getSrpVerifier' | 'computeKeyPassword' | 'generateKeySalt'>;
type Wrapped = Awaited<ReturnType<OpenPGPCryptoWithCryptoProxy['encryptSessionKeyWithPassword']>>;
type WrapKey = Parameters<OpenPGPCryptoWithCryptoProxy['encryptSessionKeyWithPassword']>[0];
// A prepared password is used within this long of its modulus being read.
export const LINK_AHEAD_MS = 10000;
// Long enough for a request just handed to the HTTP client to be on its way.
const AFTER_DISPATCH_MS = 25;
export class LinkAhead {
    private ahead?: { password: string; salt: string; at: number; taken: boolean; salted: boolean;
        keyPassword: Promise<string>; verifier: ReturnType<SrpModule['getSrpVerifier']>;
        sessionKey?: WrapKey; wrap?: Promise<{ keyPassword: string; wrapped: Promise<Wrapped> }> };
    constructor(private readonly inner: SrpModule, private readonly now: () => number = () => performance.now()) {}
    prepare(password: string): void {
        const salt = this.inner.generateKeySalt();
        const keyPassword = this.inner.computeKeyPassword(password, salt), verifier = this.inner.getSrpVerifier(password);
        keyPassword.catch(() => {}); verifier.catch(() => {});
        this.ahead = { password, salt, at: this.now(), taken: false, salted: false, keyPassword, verifier };
    }
    get prepared(): boolean { return !!this.ahead; }
    drop(): void { this.ahead = undefined; }
    // The prepared password, once, while its modulus is recent.
    take(): string | undefined {
        const ahead = this.ahead;
        if (!ahead || ahead.taken || this.now() - ahead.at > LINK_AHEAD_MS) { this.ahead = undefined; return undefined; }
        ahead.taken = true;
        return ahead.password;
    }
    // The session key made last while a link is prepared: a share's, when its request follows.
    sessionKeyMade(key: WrapKey): void { if (this.ahead && !this.ahead.wrap) this.ahead.sessionKey = key; }
    wrapAhead(wrap: (key: WrapKey, keyPassword: string) => Promise<Wrapped>): void {
        const ahead = this.ahead, key = ahead?.sessionKey;
        if (!ahead || !key || ahead.wrap || this.now() - ahead.at > LINK_AHEAD_MS) return;
        ahead.wrap = ahead.keyPassword.then(keyPassword => {
            const wrapped = wrap(key, keyPassword);
            wrapped.catch(() => {});
            return { keyPassword, wrapped };
        });
        ahead.wrap.catch(() => {});
    }
    // The wrap made ahead, for the very key and password it was made with.
    async wrapped(key: WrapKey, keyPassword: string, wrap: () => Promise<Wrapped>): Promise<Wrapped> {
        const ahead = this.ahead;
        if (!ahead?.taken || !ahead.wrap || key !== ahead.sessionKey) return wrap();
        try {
            const made = await ahead.wrap;
            if (made.keyPassword === keyPassword) return await made.wrapped;
        } catch { /* Made in line below. */ }
        return wrap();
    }
    generateKeySalt(): string {
        const ahead = this.ahead;
        if (!ahead?.taken || ahead.salted) return this.inner.generateKeySalt();
        ahead.salted = true;
        return ahead.salt;
    }
    computeKeyPassword(password: string, salt: string): Promise<string> {
        const ahead = this.ahead;
        if (!ahead?.taken || password !== ahead.password || salt !== ahead.salt) return this.inner.computeKeyPassword(password, salt);
        return ahead.keyPassword.catch(() => this.inner.computeKeyPassword(password, salt));
    }
    getSrpVerifier(password: string): ReturnType<SrpModule['getSrpVerifier']> {
        const ahead = this.ahead;
        if (!ahead?.taken || password !== ahead.password) return this.inner.getSrpVerifier(password);
        return ahead.verifier.catch(() => this.inner.getSrpVerifier(password));
    }
    getSrp(...args: Parameters<SrpModule['getSrp']>) { return this.inner.getSrp(...args); }
}
// The Drive client's OpenPGP module, telling `LinkAhead` of the session keys it makes and
// asking it for the wrap it may have made ahead.
export class LinkOpenPGP extends OpenPGPCryptoWithCryptoProxy {
    constructor(proxy: ConstructorParameters<typeof OpenPGPCryptoWithCryptoProxy>[0], private readonly link: LinkAhead) { super(proxy); }
    override async generateSessionKey(...args: Parameters<OpenPGPCryptoWithCryptoProxy['generateSessionKey']>) {
        const key = await super.generateSessionKey(...args);
        this.link.sessionKeyMade(key as WrapKey);
        return key;
    }
    override encryptSessionKeyWithPassword(sessionKey: WrapKey, password: string) {
        return this.link.wrapped(sessionKey, password, () => super.encryptSessionKeyWithPassword(sessionKey, password));
    }
    wrapAhead(): void { this.link.wrapAhead((key, keyPassword) => super.encryptSessionKeyWithPassword(key, keyPassword)); }
}
// The SDK draws a link's password inside its sharing code. Its own generator is asked ahead
// for the password `prepare` hashes (the returned function draws one), and hands that one
// over when the link is made. An SDK without the generator where this pin has it makes links
// as before, unprepared.
export function linkPasswordAhead(sdk: unknown, link: LinkAhead): (() => Promise<string>) | undefined {
    const sharing = (sdk as any)?.sharing?.management?.cryptoService;
    if (typeof sharing?.generatePublicLinkPassword !== 'function') return undefined;
    const draw: () => Promise<string> = sharing.generatePublicLinkPassword.bind(sharing);
    sharing.generatePublicLinkPassword = async () => link.take() ?? draw();
    return draw;
}
// An account's link preparation, one upload's at a time, whichever Drive client made it.
// It starts a moment after the request being made has left:
// the hashing holds this thread, and must not hold that request back. `unused` ends it for
// good: a start it overtook never draws a password, and a password drawn before it is not
// hashed or kept after it.
export function linkPreparation(link: LinkAhead, afterMs = AFTER_DISPATCH_MS) {
    let current = 0;
    return {
        start: (draw: (() => Promise<string>) | undefined): void => {
            if (!draw) return;
            const mine = ++current;
            setTimeout(() => {
                if (mine !== current) return;
                void draw().then(password => { if (mine === current) link.prepare(password); }, () => {});
            }, afterMs);
        },
        unused: (): void => { ++current; link.drop(); },
    };
}
// A token refresh makes the account code forget the user and address keys it has loaded,
// although the refreshed session is the same account's: a load under way at the refresh was
// then made a second time, two more requests in sequence. That load is put back instead.
// Call this before the account code is set up, and the returned function with its addresses
// after: the two listeners then run before and after the account code's own. A session of
// another account, or none, keeps nothing.
type SessionEvents = { on(event: 'sessionInfoChanged', callback: () => void): void; isLoggedIn(): boolean; readonly uid: string | undefined };
export function keepUserDataAcrossRefresh(credentials: SessionEvents): (addresses: object) => void {
    let loaded: { userDataPromise?: unknown } | undefined, kept: unknown, uid: string | undefined;
    credentials.on('sessionInfoChanged', () => { kept = loaded?.userDataPromise; });
    return addresses => {
        loaded = addresses as { userDataPromise?: unknown };
        uid = credentials.uid;
        credentials.on('sessionInfoChanged', () => {
            const same = credentials.isLoggedIn() && uid !== undefined && credentials.uid === uid;
            if (same && kept && loaded && !loaded.userDataPromise) loaded.userDataPromise = kept;
            kept = undefined; uid = credentials.uid;
        });
    };
}
// Proton's API answers each connection from one of several backends and stays with it for
// the connection's life. Measured from the lab on 2026-11-11, one read took 0.17 s on a
// quarter of connections and 0.35 to 0.6 s on the rest, again and again, with the same
// network round trip on all of them. The HTTP client's pool gives no say in which connection
// carries a request, so an upload's dozen requests in sequence were as slow as the
// connections they happened to get.
//
// `fastestConnectionFetch` wraps `fetch` for one origin. Its first GET there opens a few
// HTTP/2 connections and is sent on each of them; the connection that answers first is kept,
// the others are closed, and every later request to that origin is a stream on the kept one.
// Only a GET is sent more than once: it changes nothing however often the service sees it.
//
// This is a shortcut and never a requirement. Requests to other origins go to the ordinary
// client. So does everything else, for the rest of the process, as soon as anything is not
// as expected: no candidate connects or answers in time, the first answer is not an ordinary
// API answer, or a request on the kept connection fails. A request that failed there is sent
// again on the ordinary client when that is safe: a GET, or one the service says it never
// processed. Any other failure is reported as the ordinary client reports a lost connection.
export type Connect = (origin: string) => http2.ClientHttp2Session;
type ConnectionOptions = { candidates?: number; chooseWithinMs?: number; pingEveryMs?: number; connect?: Connect };

// Hop-by-hop fields HTTP/2 forbids, and those the stream supplies itself.
const NOT_SENT = new Set(['host', 'connection', 'keep-alive', 'proxy-connection', 'transfer-encoding', 'upgrade', 'te', 'accept-encoding']);
// The service's own time on a request, beyond the network round trip, above which a
// connection is not on a fast backend: the fast ones measured 0.04 s, the others 0.2 s and more.
export const SLOW_ANSWER_MS = 100;
// `unprocessed`: the service did not act on the request, so sending it again is safe.
class StreamFailure extends TypeError {
    constructor(readonly unprocessed: boolean, cause?: unknown) { super('fetch failed', { cause }); }
}
function abortError(signal: AbortSignal): unknown {
    return signal.reason ?? new DOMException('The operation was aborted.', 'AbortError');
}
function decoded(body: Buffer, encoding: string): Buffer {
    if (!encoding || encoding === 'identity') return body;
    if (encoding === 'gzip' || encoding === 'x-gzip') return zlib.gunzipSync(body);
    if (encoding === 'deflate') return zlib.inflateSync(body);
    if (encoding === 'br') return zlib.brotliDecompressSync(body);
    throw new Error('unsupported content coding');
}
// One request as one stream. The answer's body is read to its end before it is returned:
// these are the API's JSON answers, never file content.
// What a stream needs of a request. It is read before the request's body is: a form's content
// type, with its boundary, is known to the request only until then.
type Outgoing = { method: string; url: string; fields: [string, string][] };
function outgoing(request: Request): Outgoing {
    const fields: [string, string][] = [];
    request.headers.forEach((value, name) => { fields.push([name, value]); });
    const contentType = request.headers.get('content-type');
    if (contentType && !fields.some(([name]) => name === 'content-type')) fields.push(['content-type', contentType]);
    return { method: request.method, url: request.url, fields };
}
// `lastActedOn` names the last stream the service says it acted on before it shut the connection down.
function send(session: http2.ClientHttp2Session, request: Outgoing, body: Uint8Array | undefined, signal: AbortSignal | undefined,
    lastActedOn: () => number | undefined = () => undefined): Promise<Response> {
    return new Promise<Response>((resolve, reject) => {
        if (signal?.aborted) { reject(abortError(signal)); return; }
        const url = new URL(request.url);
        const headers: http2.OutgoingHttpHeaders = { ':method': request.method, ':path': url.pathname + url.search, ':scheme': url.protocol.slice(0, -1), ':authority': url.host };
        for (const [name, value] of request.fields) if (!NOT_SENT.has(name)) headers[name] = value;
        headers['user-agent'] ??= `Bun/${Bun.version}`;
        headers['accept'] ??= '*/*';
        headers['accept-encoding'] = 'gzip';
        let stream: http2.ClientHttp2Stream;
        try { stream = session.request(headers, { endStream: !body }); }
        catch (error) { reject(new StreamFailure(true, error)); return; }
        let status = 0, settled = false;
        const answer = new Headers(), chunks: Buffer[] = [];
        // The service did not act on a stream it refused, one it wants on HTTP/1.1, or one
        // opened after the last it names when it shuts the connection down.
        const failure = (cause?: unknown) => {
            const last = lastActedOn();
            return new StreamFailure(!status && ([http2.constants.NGHTTP2_REFUSED_STREAM, http2.constants.NGHTTP2_HTTP_1_1_REQUIRED].includes(stream.rstCode) ||
                (last !== undefined && typeof stream.id === 'number' && stream.id > last)), cause);
        };
        const aborted = () => { stream.close(http2.constants.NGHTTP2_CANCEL); finish(() => reject(abortError(signal!))); };
        const finish = (settle: () => void) => { if (settled) return; settled = true; signal?.removeEventListener('abort', aborted); settle(); };
        signal?.addEventListener('abort', aborted, { once: true });
        stream.on('response', fields => {
            status = Number(fields[':status']);
            for (const [name, value] of Object.entries(fields)) {
                if (name.startsWith(':') || value === undefined) continue;
                for (const item of Array.isArray(value) ? value : [value]) answer.append(name, String(item));
            }
        });
        stream.on('data', (chunk: Buffer) => { chunks.push(chunk); });
        stream.on('end', () => { if (status) finish(() => {
            try {
                if (!Number.isInteger(status) || status < 200 || status > 599) throw new Error('no status');
                const content = decoded(Buffer.concat(chunks), (answer.get('content-encoding') ?? '').trim().toLowerCase());
                answer.delete('content-encoding'); answer.delete('content-length');
                const empty = status === 204 || status === 205 || status === 304 || request.method === 'HEAD';
                resolve(new Response(empty ? null : new Uint8Array(content), { status, headers: answer }));
            } catch (error) { reject(new StreamFailure(false, error)); }
        }); });
        // A stream that ended without an answer is judged when it closes, by how it was reset.
        stream.on('error', error => finish(() => reject(failure(error))));
        stream.on('close', () => finish(() => reject(failure())));
        if (body) stream.end(body);
    });
}

// `settle` ends the looking for a faster connection: from then on the kept one is not replaced.
export type ChosenConnectionFetch = typeof fetch & { settle(): void };
export function fastestConnectionFetch(origin: string, direct: typeof fetch, options: ConnectionOptions = {}): ChosenConnectionFetch {
    const candidates = options.candidates ?? 6, chooseWithinMs = options.chooseWithinMs ?? 4000, pingEveryMs = options.pingEveryMs ?? 20000;
    const connect = options.connect ?? (target => http2.connect(target));
    // undefined: not chosen yet. null: the ordinary client carries this origin from now on.
    let kept: http2.ClientHttp2Session | null | undefined;
    let choosing: Promise<void> | undefined;
    let settled = false;
    const abandon = (session: http2.ClientHttp2Session) => { if (kept === session) kept = null; };
    const shutDownAfter = new WeakMap<http2.ClientHttp2Session, number>();
    const keep = (session: http2.ClientHttp2Session) => {
        kept = session;
        // An idle connection is closed by the service or a router between; a ping keeps it
        // while the helper waits for its operation's next request. It never keeps the process alive.
        const pings = setInterval(() => { if (kept === session && !session.closed && !session.destroyed) session.ping(() => {}); }, pingEveryMs);
        (pings as { unref?: () => void }).unref?.();
        (session as unknown as { unref?: () => void }).unref?.();
        const over = () => { clearInterval(pings); abandon(session); };
        session.on('close', over); session.on('error', over);
        session.on('goaway', (_code: number, lastStream: number) => { if (Number.isInteger(lastStream)) shutDownAfter.set(session, lastStream); over(); });
    };
    type Won = { session: http2.ClientHttp2Session; response: Response; answerMs: number };
    // Sends one GET on each of `count` new connections. The first answer `accept` takes wins:
    // its connection is returned open and every other one is closed. Undefined when none won.
    const race = (request: Request, count: number, accept: (response: Response, answerMs: number) => boolean, signal?: AbortSignal) =>
        new Promise<Won | undefined>(resolve => {
            const sessions: (http2.ClientHttp2Session | undefined)[] = [], races: AbortController[] = [];
            const read = outgoing(request);
            let pending = count, done = false;
            const settle = (won?: Won) => {
                if (done) return;
                done = true; clearTimeout(timer); signal?.removeEventListener('abort', cancelled);
                sessions.forEach((session, index) => { if (session !== won?.session) { races[index].abort(); session?.destroy(); } });
                resolve(won);
            };
            const out = () => { if (--pending === 0) settle(); };
            const cancelled = () => settle();
            const timer = setTimeout(settle, chooseWithinMs);
            if (signal?.aborted) { settle(); return; }
            signal?.addEventListener('abort', cancelled, { once: true });
            for (let index = 0; index < count; index++) {
                const candidate = new AbortController();
                races.push(candidate);
                let session: http2.ClientHttp2Session | undefined;
                try { session = connect(origin); } catch { /* A candidate that never answered. */ }
                sessions.push(session);
                if (!session) { out(); continue; }
                const opened = session;
                let connectedAt = performance.now();
                opened.on('error', () => {});
                opened.once('connect', () => { connectedAt = performance.now(); });
                send(opened, read, undefined, candidate.signal).then(response => {
                    const answerMs = performance.now() - connectedAt;
                    if (accept(response, answerMs)) settle({ session: opened, response, answerMs }); else { opened.destroy(); out(); }
                }, out);
            }
        });
    // A first connection that is not on a fast backend is replaced, once, by one that is, if a
    // second round finds one. The first keeps carrying requests meanwhile, and ends when its own
    // are done. Once the caller is about to change something (`settle`) nothing is replaced: a
    // change, and the reads that depend on it, all travel on one connection to one backend.
    const improve = (first: Won, request: Request) => {
        const session = first.session;
        if (first.response.status !== 200) return;
        try {
            session.ping((error, roundTrip) => {
                if (error || settled || kept !== session || first.answerMs - roundTrip <= SLOW_ANSWER_MS) return;
                void race(request, candidates, (response, answerMs) => response.status === 200 && answerMs - roundTrip <= SLOW_ANSWER_MS).then(better => {
                    if (!better) return;
                    if (settled || kept !== session) { better.session.destroy(); return; }
                    keep(better.session);
                    session.close();
                }, () => {});
            });
        } catch { /* It stays as chosen. */ }
    };
    // The first GET decides. An ordinary API answer is a 200, or a 401 for an expired token;
    // anything else leaves the origin to the ordinary client, which is asked the same GET.
    const choose = async (request: Request, signal: AbortSignal): Promise<Response> => {
        let decided!: () => void;
        choosing = new Promise<void>(resolve => { decided = resolve; });
        const again = request.clone();
        let won: Won | undefined;
        try { won = await race(request, candidates, response => response.status === 200 || response.status === 401, signal); }
        finally { if (won) keep(won.session); else kept = null; decided(); }
        if (signal.aborted) { if (won) { abandon(won.session); won.session.destroy(); } throw abortError(signal); }
        if (!won) return direct(request, { signal });
        improve(won, again);
        return won.response;
    };
    const chosen = (async (input: Parameters<typeof fetch>[0], init?: Parameters<typeof fetch>[1]) => {
        // A request for anywhere else is handed on as it came, its body untouched.
        let elsewhere = true;
        try { elsewhere = new URL(typeof input === 'string' ? input : input instanceof URL ? input.href : (input as Request).url).origin !== origin; }
        catch { /* The ordinary client says what is wrong with it. */ }
        if (elsewhere) return direct(input, init);
        const request = new Request(input as RequestInfo, init);
        if (choosing) await choosing;
        const signal = request.signal;
        if (kept === undefined && request.method === 'GET') return choose(request, signal);
        const session = kept;
        if (!session || session.closed || session.destroyed) return direct(request, { signal });
        const stream = outgoing(request);
        const body = request.body ? new Uint8Array(await request.arrayBuffer()) : undefined;
        const ordinary = () => direct(request.url, { method: request.method, headers: stream.fields, body, signal });
        try {
            const response = await send(session, stream, body, signal, () => shutDownAfter.get(session));
            // The service saying this connection or protocol is not for this request.
            if ([421, 426, 505].includes(response.status)) { abandon(session); if (request.method === 'GET') return ordinary(); }
            return response;
        } catch (error) {
            if (!(error instanceof StreamFailure)) throw error;
            // One failure retires the connection: nothing later depends on it.
            abandon(session);
            if (request.method === 'GET' || error.unprocessed) return ordinary();
            throw error;
        }
    }) as ChosenConnectionFetch;
    chosen.settle = () => { settled = true; };
    return chosen;
}
// Every API request of this process travels on one connection chosen that way. The account
// and Drive code reach the API through `fetch`, so that is where the choice is made, once
// per process.
const API_ORIGIN = 'https://drive-api.proton.me';
let apiFetch: ChosenConnectionFetch | undefined;
export function useFastestApiConnection(): void {
    if (apiFetch) return;
    // The ordinary client reaches the service through a configured proxy; a connection of
    // the helper's own would go around it.
    if (['HTTPS_PROXY', 'https_proxy', 'HTTP_PROXY', 'http_proxy', 'ALL_PROXY', 'all_proxy'].some(name => process.env[name])) return;
    apiFetch = fastestConnectionFetch(API_ORIGIN, globalThis.fetch.bind(globalThis));
    globalThis.fetch = apiFetch;
}
// The signed-in account: its session from the keychain and the API client that refreshes it.
export async function openAccount(request: JsonObject) {
    const dataDir = await privateDataDirectory();
    useFastestApiConnection();
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
            const raw = await keychain(() => Bun.secrets.get({ service: SECRET_SERVICE, name: request.destination.credentials.reference }));
            accountReferenceExists = raw !== null;
            return parseStoredSnapshot(raw);
        },
        save: snapshot => saveToKeychain(() => store.save(snapshot)), remove: () => store.remove(),
    }, logger);
    const userDataKept = keepUserDataAcrossRefresh(credentials);
    const requirementFailure = () => { throw new ProtocolError('unsupported_request'); };
    const apiClient = new ApiClient({ baseUrl: 'drive-api.proton.me', appVersion: APP_VERSION, credentials, logger,
        headers: { 'x-pm-drive-sdk-version': SDK_VERSION }, afterResponseHooks: accountResponseHooks(requirementFailure) });
    let account;
    try {
        account = await initAccount({ authClientId: 'external-drive', apiClient, credentials, cryptoProxy: CryptoProxy,
            srpApi: { computeKeyPassword, generateKeySalt, getRandomSrpVerifier, getSrp }, logger,
            accountUrl: 'account.proton.me' });
    } catch { throw new ProtocolError('keychain_unavailable'); }
    userDataKept(account.addresses);
    let warmed = false;
    const link = new LinkAhead(account.srp), preparation = linkPreparation(link);
    // No persisted entities, decrypted keys or names: both caches live in this process's
    // memory, shared by its Drive clients. Ownership and destination checks read past the
    // entity cache (`fresh`).
    return { dataDir, telemetry, logger, http: new SharedAccountReads(new HTTPClient(apiClient)), account, referenceExists: () => accountReferenceExists,
        entities: new RequestCache<string>(), keys: createCryptoCache(), link, preparation,
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
    const { dataDir, telemetry, logger, http, account, entities, keys, link, preparation } = opened;
    const clientUid = publicationClientUid(request) ?? await getOrGenerateClientUid({ appDir: dataDir, clientUidPrefix: 'putgd' } as any, logger);
    const pgp = new LinkOpenPGP(CryptoProxy, link);
    const sdk = new ProtonDriveClient({ config: { baseUrl: 'drive-api.proton.me', clientUid },
        httpClient: http, telemetry, account: new DriveAccountAdapter(account.addresses),
        srpModule: link, openPGPCryptoModule: pgp,
        entitiesCache: entities, cryptoCache: keys,
        // One request carries a file under 128 KiB, as in Proton's own CLI at this pin.
        featureFlagProvider: { isEnabled: async flag => flag === FeatureFlags.DriveSmallFileUpload } });
    const drawLinkPassword = linkPasswordAhead(sdk, link);
    return { sdk, auth: account.auth, accountReferenceExists: opened.referenceExists(),
        fresh: <R>(read: () => Promise<R>) => entities.fresh(read), warm: opened.warm, modulusAhead: opened.modulusAhead,
        // Hears of each request that carries a file's bytes, with the host it goes to.
        watchTransfers: (seen?: (host: string) => void) => { http.onTransfer = seen; },
        // Starts the next link's password work (see LinkAhead and linkPreparation).
        prepareLink: () => preparation.start(drawLinkPassword),
        // While a link is being made, its share's session key is wrapped beside the share's request.
        linkBeingMade: (making: boolean) => { http.onShareCreate = making ? () => setTimeout(() => pgp.wrapAhead(), AFTER_DISPATCH_MS) : undefined; },
        linkPrepared: () => link.prepared,
        // Ends a preparation no link used: nothing of it is kept, and none still to come is made.
        linkUnused: preparation.unused,
        // Called before a request that changes something: the API connection stays as it is from here.
        connectionSettled: () => { apiFetch?.settle(); } };
}
export async function initialize(request: JsonObject) { return openClient(await openAccount(request), request); }
