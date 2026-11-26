#pragma once
// putgd-cli --encrypt-job (#2079): the resident's encrypting helper, so the resident itself
// never maps libcrypto. One JSON request on standard input:
//  {input, bytes, name, type, form ("inline" or "split"), page, data (split only),
//   bin (split only: the data file's name at the destination), passphrase (optional)}
// `page` and `data` are new files it creates (O_EXCL, mode 0600) in private folders.
// One JSON reply on standard output: {ok, key (the link fragment, "v1.…"),
//  page: {bytes, sha256}, data: {bytes, sha256}} or {ok: false, error}.
// The key and the passphrase travel only over the two pipes.
namespace putgd::encrypt {
bool isEncryptJob(int argc,char** argv);
int runEncryptJob();
}
