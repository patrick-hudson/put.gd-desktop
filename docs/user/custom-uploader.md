# Custom uploader (HTTP) destination

A custom uploader sends each upload to your own server, or to any service that publishes
a ShareX custom uploader file (`.sxcu`), with one HTTP request you describe (#1361). Its
storage service is `custom-http`. Zipline, Chibisafe and XBackBone are the services it
is tested against; any service with a `.sxcu` export for images, text or files should
import.

## Setting it up

1. **Destinations → Add destination → Custom uploader (HTTP)**.
2. **Import .sxcu…** and choose the file your service gave you, or paste its contents.
   The form fills from it. Without a file, fill the form by hand: see
   [Settings](#settings).
3. Under **Credentials**, choose **System keychain** or **This session only**, then
   store the values (**Save to keychain** or **Use for this session**). Until then the
   file's token exists only in the Settings window.
4. **Test upload and link** (Test connection). It uploads a small synthetic text file
   with your request (a PNG when the destination takes images only), opens the returned
   link, and deletes the file again when the reply carried a deletion URL. When it did
   not, the result says the test file stays on the server.

A plain `http://` address works for a loopback address without asking. Any other one
needs **Allow plain HTTP for this destination**, which appears beside the address:
plain HTTP sends the file and its token unencrypted.

## What an import does with secrets

A `.sxcu` file carries its token as plain JSON, in `Headers`, `Parameters`, `Arguments`
or `Data`. put.gd treats every such value as a secret:

- Each header, parameter and argument value becomes a **secret row**: its name is
  saved, its value is not. The only exceptions are values that are exactly `{filename}`
  or `{input}`. Clear **Secret** on a row to keep an ordinary value, such as a format
  name, in the configuration instead.
- The `Data` body of a JSON or XML uploader is one secret value.
- A credential that the file repeats as literal text in the request URL or the
  deletion URL is replaced there by its name, `{secret:arguments/token}`. XBackBone
  ends its deletion URL with the upload token; put.gd records that URL with the name
  and fills the value in only when the deletion is sent. An ordinary word that a row
  also holds, such as a folder name, stays as written (see
  [What counts as a credential](#what-counts-as-a-credential)).
- A run of letters and digits left after the host of the request URL or the deletion
  URL is taken for a key and moves to **Other secret values** (`secrets/url-key`,
  `secrets/deletion-key`): sixteen characters or more of letters and digits alone, as a
  hex key is, or twenty or more with `-` and `_` among them. The host ends at the path,
  or at `?` or `#` when the address has no path. No rule can tell a key from an album
  or a user ID of the same shape, so the import notes say when a run was moved: if it is
  an ID that the server's links repeat, write it back into the address and remove it
  from **Other secret values**. A shorter part that mixes letters and digits stays as
  written, and the notes say where it is (its position, never its text) so you can move
  it yourself.
- Imports are taken one at a time. A second one that arrives while the first is still
  being saved is refused with `import_busy`; a destination that could not be saved
  leaves no stored values behind. When the destination was written and only reading
  the configuration back failed, it exists: its values stay, and the error says so
  (`destinationWritten`).
- In keychain mode the values go to the entry `putgd/http/<ID>`. An import is refused
  with `credential_reference_in_use` when another destination already names that
  entry, so it never overwrites or removes another destination's credentials; a derived
  ID skips to the next free one. The import waits two minutes for the keychain. A
  prompt that nobody answers ends it with `credential_store_timeout` and lets the next
  import in; if the keychain answers later, what it wrote is removed again.
- A credential may not appear in the link, which is shared and recorded in History.
  A file whose `URL` repeats one is refused, and the message names the row. A
  `ThumbnailURL` or `ErrorMessage` that repeats one is left out, with a note.

The values are held in the Settings window until **Store credentials** puts them in
the system keychain or, for this session only, in memory, exactly as S3 keys are. They
are never written to `config.json`, the Settings draft, the window's state checkpoint,
logs, History or a diagnostics package. A diagnostics package shows which rows a
destination has and where its request goes (scheme and host), never a value, a path or
a reply template's text.

### What counts as a credential

Every imported value is stored as a secret, because a file does not say which of its
values is the token. That does not make every stored value a credential. An argument
`folder: screenshots` is stored like a token, yet a link `https://host/screenshots/a.png`
is an ordinary link. So when put.gd looks for a destination's secrets in an address or
in a server's message, it looks for what is credential-like:

- **The value of a row whose name says so**: a name that contains `auth` (not
  `author`), `token`, `key` (not `keyword`), `secret`, `password`, `cookie`, `session`,
  `signature` and the like. Every value under **Other secret values** counts too. The
  value counts from four characters, each of its words from eight, and the token after
  `Bearer`, `Basic`, `Digest` or `Token` by itself.
- **A key-like run inside any value**: sixteen or more letters and digits, or twenty or
  more with `-` and `_`, holding at least one letter and one digit. A key inside a
  stored JSON body or a cookie (`session=…; path=/`) is found by itself.
- **A value the stored text itself names as a credential**: `"password":"…"`,
  `token=…`, `<secret>…</secret>`, from four characters.

Ordinary words, switches such as `true` or `json`, and short settings are not looked
for. A credential that is none of the above, such as a six-letter password in a header
named `X-Upload`, is not recognised: store it under a name that says what it is, or
under **Other secret values**. In the other direction, a long ID that is no credential
(an album UUID in a header) is taken for a key; if the server's links repeat it, clear
**Secret** on its row.

Credentials shorter than four characters are stored like the others but are not hidden
in server messages or looked for in addresses: hiding every `1` or `on` would leave
nothing to read.

## Settings

| Field (form label) | Meaning |
| --- | --- |
| `requestMethod` (Method) | `POST`, `PUT`, `PATCH` or `GET`. A `GET` has no body. |
| `requestUrl` (Request URL) | `https://` address without a query, user name or fragment. `{filename}` and `{secret:…}` may fill part of the path; scheme and host are fixed text. |
| `allowInsecureHttp` (Allow plain HTTP for this destination) | Lets this destination use `http://` beyond loopback. Off by default. |
| `body` (Body) | `multipart` (form data with the file), `binary` (the file's bytes), `form` (URL-encoded fields), `json` or `xml` (the body text), or `none`. Only `binary`, and `multipart` with a file form name, carry the file. The others send text through `{input}`, so anything that is not text fails with `http_text_only`. |
| `fileFormName` (File form name) | The multipart field that carries the file. Empty sends the form fields only. |
| `headers`, `parameters`, `arguments` (Headers, URL parameters, Form fields) | Up to 32 rows each: `{name, secret: true}`, or `{name, secret: false, value}`. `Content-Type` can be set for a binary body only; `Host`, `Content-Length` and similar headers cannot be set. |
| `secrets` (Other secret values) | Up to 32 named values with no row of their own, for `{secret:secrets/name}` in the request URL or the deletion URL. |
| `data`, `dataSecret` (Body text) | The JSON or XML body. With `dataSecret` the text is a stored secret and `data` is empty. |
| `accepts` (Uploads) | Any of `image`, `text`, `file`. `file` takes everything, recordings included. |
| `connectTimeoutMs`, `requestTimeoutMs` (Connect timeout, Request timeout) | 1–120 s and 1 s–1 h (default 10 s and 600 s). The form shows whole seconds. |
| `responseUrl` (URL) | Where the reply holds the link. Empty means the whole reply is the link. |
| `thumbnailUrl` (Thumbnail URL) | Optional; recorded with the upload. |
| `deletionUrl` (Deletion URL) | Optional. Without one, uploads to this destination cannot be deleted from History. |
| `errorMessage` (Error message) | Optional; what to show from the reply when the server refuses an upload. |

Limits: a name is at most 256 characters, a value 8,192, the body text 65,536, a reply
template 4,096 and a `.sxcu` file 256 KB. The stored values of one destination together
are at most 128 KB, and at most 96 KB in one imported file.

## Placeholders

Request values (the request URL and row values) take:

| Placeholder | Stands for |
| --- | --- |
| `{filename}` | The file's name. |
| `{input}` | The text being uploaded, at most 16 MB. In a request that also carries the file it is empty for anything that is not text, as in ShareX. |
| `{random:a\|b\|c}` | One of the choices. |
| `{base64:text}` | The text in Base64. |
| `{secret:key}` | A stored value of this destination, by its credential-store name: `headers/<name>`, `parameters/<name>`, `arguments/<name>`, `secrets/<name>` or `data`. This placeholder is put.gd's own. |

The body text is JSON or XML with braces of its own, so only `{filename}` and `{input}`
are replaced in it, escaped for that format; everything else is sent as written. A body
that holds a key is stored as one secret value (**Secret** beside Body text).

Reply values (URL, Thumbnail URL, Deletion URL, Error message) take `{filename}`,
`{random}`, `{base64}` and:

| Placeholder | Reads |
| --- | --- |
| `{json:path}` | A value of the JSON reply. `{json:text\|path}` reads other text. |
| `{xml:xpath}` | A value of the XML reply. `{xml:text\|xpath}` reads other text. |
| `{regex:pattern\|group}` | A match in the reply; the group is a number or a name. `{regex:text\|pattern\|group}` reads other text. |
| `{header:name}` | A reply header. |
| `{response}` | The whole reply. |
| `{responseurl}` | The address a redirect led to; empty when there was no redirect. |
| `{secret:key}` | In the Deletion URL only, and on its own, not inside another placeholder. |

A backslash keeps the next character as written (`\{`), so a regular expression
doubles its backslashes: `{regex:id=(\\d+)|1}`. One parameter too many is refused.
A placeholder that finds nothing gives empty text.

**JSON paths** are an optional leading `$`, then `.name`, `['name']`, `["name"]` and
`[index]` steps: `files[0].url`, `$.data.link`, `data['a b']`. Wildcards, `..`, filters
and slices are not supported.

**XPath** is a subset read with Qt's XML stream reader, since Qt 6 has no XPath
engine: `/a/b`, `//a`, `*`, `a[2]`, `a[last()]`, `a[@k='v']`, and a final `@attribute`
or `text()`. Namespace prefixes are ignored. Axes, functions and other predicates are
not supported.

Older files import too. Files from ShareX 13.7.1 and earlier write placeholders as
`$json:url$`; they are converted. Files from 12.3.1 and earlier use `RequestType`,
`RegexList` and `ResponseType`; they are read as ShareX read them.

Not supported, and refused with a message that says so: `{select}`, `{prompt}`,
`{inputbox}` and `{outputbox}`, which ask a question during the upload; URL shorteners
and URL sharing services; methods other than the four above. ShareX name patterns such
as `%y` or `%ra` inside a value are sent as written; the import notes it.

## What put.gd can and cannot know

The server chooses each upload's name, so put.gd cannot look one up afterwards. That
changes three things compared with S3:

- **No name check and no overwrite.** Two uploads of the same file are two uploads.
- **A lost reply is an unknown outcome.** When the request was sent and no usable reply
  came back, History shows the upload as unknown and says that the file may be on the
  server. That covers a connection that closed, a reply that ended before it was
  complete, a gateway's 408, 502 or 504, any other 5xx that arrived after the whole
  request had been sent (a 500, a 503, Cloudflare's 524), and a 2xx reply with no usable
  link. **Retry upload** sends the file again and may leave a second copy there. put.gd
  never repeats an upload by itself.
- **A reply is whole by its own length.** put.gd counts the bytes of a reply and holds
  them against the `Content-Length` the server declared: a reply that is shorter is not
  a reply, even when the part that came holds a whole link. It asks for replies
  uncompressed (`Accept-Encoding: identity`), so that length is the one on the wire. A
  chunked reply declares no length; there put.gd goes by the connection, and one that
  ended without its last chunk is incomplete too. A reply with neither a length nor
  chunks ends when the connection does, and cannot be told from one that was cut.
- **A server that compresses its reply regardless.** put.gd does not unpack a reply.
  One that arrives compressed although it was asked for as it is still counts as whole
  or cut by its length on the wire, and a link in a reply header is used as it is. A
  link that was to be read from the compressed body cannot be: the upload is an unknown
  outcome (`http_response_unreadable`), and the message says that the server compressed
  its reply. Turn compression off for the upload address on the server.
- **Deletion works by the deletion URL the upload returned.** Without one, History
  offers no deletion and says why.

A request that never reached the server (`http_connect_failed`) stored nothing. A 4xx
other than 408 is a refusal and is recorded as a plain failure, with the server's
message; so is a failure before the request was complete. A server that answers 5xx
while the file is still on its way is such a failure: put.gd's HTTP client reads no
reply while it is sending, so it reports the connection that ended under the upload
(`http_upload_interrupted`), and nothing was stored. put.gd takes the server at its
word on a 4xx: a server that stores a file and then answers 4xx leaves a copy put.gd
does not know of.

| Reply | Result |
| --- | --- |
| 2xx with a link | Uploaded. The link is one absolute `http(s)` address, or a path from the server's root. |
| 2xx without a usable link | Unknown: `http_response_unreadable`. Check the URL value under Response. |
| 2xx whose link repeats a credential | Unknown: `http_link_holds_secret`. The link is not recorded or copied. |
| A 2xx reply shorter than its `Content-Length`, a chunked 2xx reply without its last chunk, a connection that closed or reset under a 2xx reply | Unknown: `http_outcome_unknown` |
| 401, 403 | `http_authentication_failed` |
| 413 | `http_too_large` |
| 429 | `http_rate_limited` |
| 408, 502, 504 | Unknown: `http_outcome_unknown` |
| Other 5xx, after the whole request was sent | Unknown: `http_outcome_unknown`, with the server's message |
| Other 5xx, before the request was complete | `http_upload_interrupted`, or `http_server_error` where the status was read |
| Other 4xx | `http_request_refused` |

The status line decides first. A 4xx other than 408 that answers the upload request
itself is a refusal even when the rest of its reply was cut short, and a 5xx follows
its own rows; whether a reply arrived whole matters for a 2xx, the only kind that could
otherwise count as an upload. One exception: after a 301, 302 or 303 the reply is
fetched from the address the server named, and any status but a 2xx from there, a 4xx
included, is an unknown outcome (`http_outcome_unknown`). The upload request had
already been answered, so the file may be stored.

Text from the server is shown on one line, bounded, with every credential of the
destination ([what counts](#what-counts-as-a-credential)) removed, however the server
spells it: as sent, percent-encoded, as PHP writes JSON, in base64, or with a control
character inside it. The line is made first, the credentials are removed from it, and
only then is it cut to length. A spelling that cannot be removed in place withholds
the whole text.

**Addresses a reply returns.** The link, the thumbnail and the deletion URL come from
the server, so each is checked before it is kept:

- An address that repeats a credential is never recorded, copied or shown. This
  applies after the address's host, in any of the spellings above; a credential under
  sixteen characters counts only as a whole word. A link that holds one makes the
  upload an unknown outcome (`http_link_holds_secret`): the file is probably on the
  server, and the message names the row. Retry would send the file again and end the
  same way, so change the destination first: if that value is not a credential, clear
  **Secret** on its row (or, for a value under **Other secret values**, write it back
  into the address). A thumbnail that holds one is dropped.
- A deletion URL that holds one is recorded with the value's name, `{secret:key}`,
  when the value stands there exactly as stored, and is not kept otherwise. A deletion
  URL that names a stored value, from its template or this way, must be on the request
  URL's own host: a stored value is never sent anywhere else, so for an upload whose
  deletion address is on another host, deletion is unavailable. History then says that
  a deletion URL came back and why it was not kept.
- An address with a user name in it (`https://shown@real/`) is not used: it shows one
  host and goes to another.
- An address on this computer (`localhost`, `127.0.0.1` in any spelling, `::1`,
  `::ffff:7f00:1`) is used only when the destination's own request URL is on this
  computer, so a public server cannot point Test connection or a deletion at a local
  service. Redirects follow the same rule. Before put.gd fetches an address a reply
  named, it also looks the host up: a name that resolves to this computer
  (`ip6-localhost`, the machine's own host name, a public name that answers
  `127.0.0.1`) or to one of its network addresses is refused the same way.
- put.gd fetches such an address (the link in Test connection, the link and the
  deletion URL in a deletion) under the destination's own transport rule: plain HTTP
  only when **Allow plain HTTP** is on, or to this computer. Test connection says so
  when it did not open a plain-HTTP link.
- `{responseurl}` is where a redirect led. Without a redirect it is empty: the
  request's own address, secret parameters included, is never a link.

**Redirects.** A 301, 302 or 303 is followed with a GET that carries no body; the
destination's headers and values are sent only to the host that was asked. A 307 or
308 asks for the same request again and is honoured on the same host only: put.gd does
not send the file or its credentials to another host (`http_redirect_refused`). A
redirect from `https://` to `http://` is never followed.

## Deletion

**Delete from destination…** in History prepares the deletion and shows its target.
Confirming it journals the intent, then sends one GET of the recorded deletion URL.

- Some servers answer with a confirmation page. When that page holds a form that posts
  back to the same host, put.gd submits it, as XBackBone needs.
- The deletion counts only when it is verified: put.gd fetches the link before and
  after, and a server that answers 200 while the link still serves the same bytes gets
  `http_delete_unconfirmed`, not "deleted". A check whose reply was cut short, before
  or after the deletion, is held against the other as far as it came: a link that still
  answers with the bytes it had is `http_delete_unconfirmed` as well, and so is one
  whose check brought none of the file (the message then says that the check shows
  nothing either way).
- A lost reply leaves the deletion unknown; **Retry** asks the server again and finds
  the file gone. A redirect that put.gd does not follow is unknown too: the server
  received the request. A deletion URL that names a stored value follows no redirect
  to another host, since the server could have written the value into the address it
  redirects to.
- Zipline and Chibisafe return no deletion URL, so their uploads are deleted on the
  server itself.

## The HTTP worker

All HTTP runs in `putgd-http-worker`, one process per request, so the resident loads no
network code for this destination. The resident writes one JSON request with the stored
values on the worker's standard input; nothing secret is on a command line or in the
environment. The worker answers with newline JSON: progress, a checkpoint before a
deletion, and one result. A closed input cancels.

The request body is sent from a device that can be read only once. Qt Network would
otherwise send a request again by itself when a connection closes before the reply;
with a body it cannot rewind, it reports an error instead, and the upload is unknown
rather than silently repeated. An upload that has no body (**Body** None, its text in
the address) is sent with an empty one for the same reason, so such a request carries
`Content-Length: 0`. Qt reports nothing about connecting or sending for a request like
that over plain HTTP. So that the destination's **Connect timeout** still holds, the
worker first asks the server for a connection of its own within that timeout, and makes
the request only when it got one: a server that gives none fails the upload as
`http_connect_failed`, and nothing was sent. A server that gives one is sent the
request, which then counts as sent, and from there only the request timeout applies.
Through an HTTP proxy (`http_proxy`) there is no server to ask, since the proxy takes
the request: there the check is left out, the connect timeout does not apply to such
an upload, and one that is stopped or times out is an unknown outcome. HTTP/1.1 is
used, so "was it sent" has one answer.

Whether a reply arrived whole is not left to what Qt reports either. When a connection
ends under a reply, Qt reports a closed connection or, when it was still finishing the
request it had written, a request it could not send again, after it has handed over
the status line and the bytes that came. The worker goes by the reply's own length
first, and takes any transport error on a successful status as an incomplete reply.

## Verification

- **Parser:** the `http_uploader_rules` CTest suite checks response values, `.sxcu`
  import, the settings rules and that stored values stay out of what is shown or
  recorded.
- **Scripted receiver:** `http_uploader_integration` runs the production provider and
  worker against `tests/fixtures/http_uploader_server.py` on loopback: the multipart,
  JSON and header reply styles, redirects, refusals, lost and late replies, deletion
  and its verification, History and retry.
- **Real receivers:** [HTTP receivers](../lab/http-receivers.md) runs Zipline, Chibisafe
  and XBackBone in containers on a builder, lets each generate its own `.sxcu`, and
  drives a resident against them.
- **Secrets:** the suites, the Settings GUI test and the MCP resident test check that an
  imported token is in no configuration file, checkpoint, log, History record or
  diagnostics package.
