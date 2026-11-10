# HTTP receivers for the custom uploader

Real services to test the [custom uploader](../user/custom-uploader.md) against (#1361):
Zipline, Chibisafe and XBackBone, each in rootless Podman containers on one builder, each
generating its own `.sxcu` file. The recipe is `tools/lab/http_receivers/`.

| Receiver | Version | Port | Its `.sxcu` | Deletion URL |
| --- | --- | --- | --- | --- |
| Zipline | 4.8.3 | 18361 | Token in an `authorization` header; link at `{json:files[0].url}` | None |
| Chibisafe | 6.7.0 | 18362 | API key in an `x-api-key` header; link at `{json:url}` | None |
| XBackBone | 3.8.2 | 18363 | Token as a form field; the same token ends its deletion URL | Yes, behind a confirmation page |

Images are pinned by digest in `receivers.py`. Nothing else in the lab uses these
ports. The receivers hold synthetic uploads only.

## Start, check, stop

Run on the builder that hosts the receivers, from a checkout:

```sh
python3 tools/lab/http_receivers/receivers.py up --bind 192.168.68.71
python3 tools/lab/http_receivers/receivers.py status --bind 192.168.68.71
python3 tools/lab/http_receivers/receivers.py sxcu --bind 192.168.68.71     # fetch the files again
python3 tools/lab/http_receivers/receivers.py down [--purge]
```

- `--bind` is the one address the receivers listen on: the builder's lab-network
  address, or `127.0.0.1`. An unspecified address such as `0.0.0.0` and any public
  address are refused, because each receiver has an admin page. Receivers that already
  run on another address are refused too: run `down` first.
- `up` creates one pod per receiver (`putgd-rx-zipline`, `putgd-rx-chibisafe`,
  `putgd-rx-xbackbone`), waits until each answers, then runs a Playwright container
  that signs in to each admin page and downloads that service's own ShareX
  configuration. It ends with one line per receiver naming the fields of its file, with
  every value masked.
- State is in `~/putgd-lab/http-receivers` (mode 0700): generated admin passwords,
  container env files, and `sxcu/zipline.sxcu`, `sxcu/chibisafe.sxcu`,
  `sxcu/xbackbone.sxcu` (mode 0600). Secrets are in env files, never on a command line.
- `down` removes the pods and keeps the volumes and files; `down --purge` also removes
  the volumes, the passwords and the `.sxcu` files, each by name.

The `.sxcu` files hold upload tokens. Copy them to the desktop under test with `scp`,
into a folder of mode 0700, and do not print them. `d69` runs the owner's own versitygw
and SMB demo: put the receivers on another builder, or leave that demo untouched.

## Drive a resident against them

`check_uploads.py` works a running resident through `putgd-cli`, for each `.sxcu` file
in a folder:

```sh
python3 tools/lab/http_receivers/check_uploads.py --cli /usr/bin/putgd-cli \
    --sxcu-dir ~/putgd-lab/sxcu --allow-insecure-http [--socket PATH] \
    [--credential-mode session|keychain] [--keep]
```

It imports the file with `settings.importUploader`, reads which values are stored, runs
Test connection, uploads a synthetic PNG and fetches its link, deletes it from History
when the receiver gave a deletion URL, and finally scans the resident's configuration,
data and state folders and a diagnostics package for every value of the files that
put.gd stores as a secret. The report has names, states and links, never a token. It
exits 0 when every receiver passed and nothing held a token.

- The receivers speak plain HTTP on the lab network, so `--allow-insecure-http` is
  needed; without it the files import and their uploads are refused.
- Session mode (the default) stores nothing in the desktop keyring. Keychain mode leaves
  one keyring entry per destination, `putgd/http/rx-<name>`, as put.gd itself does when
  a destination is removed; the report lists them.
- Destinations the run imported are removed again unless `--keep` is given, also when
  the run stops early. A destination named `rx-<name>` that already exists is reported
  and left alone.
- Every upload link goes to the clipboard, as every put.gd upload does. On a physical
  desktop, hold the desktop lock.

Expected results: all three pass. Zipline and Chibisafe report `deletion.offered:
false` with the reason, and History offers no deletion for them. XBackBone's upload is
deleted and its link answers 404 afterwards.

## Traps

- **XBackBone's deletion URL shows a confirmation page.** A plain GET deletes nothing
  and answers 200. put.gd submits the page's own form on the same host and then checks
  that the link stopped serving the file.
- **XBackBone's file names `{input}` beside the file.** For an image the field is sent
  empty, as ShareX sends it.
- **Chibisafe's web container listens on the pod's host name,** not on loopback: the
  Caddy file proxies to `putgd-rx-chibisafe:8001`. Its `.sxcu` needs an API key, which
  the recipe requests, and uploads are served by Caddy from the uploads volume.
- **A receiver that is still starting answers 5xx** through its proxy; `up` waits until
  it answers for itself before it opens the admin pages.
- **A refresh that fails leaves no file.** `sxcu` removes a receiver's old file before
  it downloads the new one, so a stale token cannot pass for a current one.
