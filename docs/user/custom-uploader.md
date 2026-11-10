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
- A stored value that the file repeats as literal text in the request URL or the
  deletion URL is replaced there by its name, `{secret:arguments/token}`. XBackBone
  ends its deletion URL with the upload token; put.gd records that URL with the name
  and fills the value in only when the deletion is sent.
- A long run of letters and digits (20 or more, with both) left in the path of the
  request URL or in the deletion URL is taken for a key and moves to **Other secret
  values** (`secrets/url-key`, `secrets/deletion-key`).
- A stored value may not appear in the link, which is shared and recorded in History.
  A file whose `URL` repeats one is refused, and the message names the row. A
  `ThumbnailURL` or `ErrorMessage` that repeats one is left out, with a note.

The values are held in the Settings window until **Store credentials** puts them in
the system keychain or, for this session only, in memory, exactly as S3 keys are. They
are never written to `config.json`, the Settings draft, the window's state checkpoint,
logs, History or a diagnostics package. A diagnostics package shows which rows a
destination has and where its request goes (scheme and host), never a value, a path or
a reply template's text.

Values under six characters, such as `true` or `json`, are stored as secrets like the
rest but are not replaced inside addresses: replacing every `1` or `json` would rewrite
the address. If a file repeats a value that short in its request URL or deletion URL,
that text stays in the address and so in `config.json`. Check the addresses of such a
file before importing it, and move the value to **Other secret values** by hand.

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
| `{responseurl}` | The address that answered, after redirects. |
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
  came back (the connection closed, a gateway answered 408, 502 or 504, or a 2xx reply
  had no link where the destination looks for one), History shows the upload as
  unknown and says that the file may be on the server. **Retry upload** sends the file
  again and may leave a second copy there. put.gd never repeats an upload by itself.
- **Deletion works by the deletion URL the upload returned.** Without one, History
  offers no deletion and says why.

A request that never reached the server (`http_connect_failed`) stored nothing. A
definite refusal (401, 403, 413, 429, or any other 4xx or 5xx except 408, 502 and 504,
which are unknown as above) is recorded as a plain
failure, with the server's message. put.gd takes the server at its word there: a server
that stores a file and then answers with an error leaves a copy put.gd does not know
of, so look on the server before retrying if a second copy would matter.

| Reply | Result |
| --- | --- |
| 2xx with a link | Uploaded. The link is one absolute `http(s)` address, or a path from the server's root. |
| 2xx without a link | Unknown: `http_response_unreadable`. Check the URL value under Response. |
| 401, 403 | `http_authentication_failed` |
| 413 | `http_too_large` |
| 429 | `http_rate_limited` |
| 408, 502, 504 | Unknown: `http_outcome_unknown` |
| Other 5xx | `http_server_error` |
| Other 4xx | `http_request_refused` |

Text from the server is shown on one line, bounded, with any stored value of the
destination removed.

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
  `http_delete_unconfirmed`, not "deleted".
- A lost reply leaves the deletion unknown; **Retry** asks the server again and finds
  the file gone.
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
rather than silently repeated. HTTP/1.1 is used, so "was it sent" has one answer.

## Verification

- **Parser:** the `http_uploader_rules` CTest suite is table tests of both placeholder
  generations, the JSON path and XPath subsets, `.sxcu` import and settings rules.
- **Scripted receiver:** `http_uploader_integration` runs the production provider and
  worker against `tests/fixtures/http_uploader_server.py` on loopback: reply styles,
  redirects, refusals, lost and late replies, deletion with confirmation and
  verification, Test connection, History and retry.
- **Real receivers:** [HTTP receivers](../lab/http-receivers.md) runs Zipline, Chibisafe
  and XBackBone in containers on a builder, lets each generate its own `.sxcu`, and
  drives a resident against them.
- **Secrets:** the suites, the Settings GUI test and the MCP resident test check that an
  imported token is in no configuration file, checkpoint, log, History record or
  diagnostics package.
