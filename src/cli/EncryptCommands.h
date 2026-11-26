#pragma once
// Encrypted links (#2079) in putgd-cli:
//   putgd-cli upload [--encrypt] [--passphrase-file F] [--profile ID] [--socket P] PATH...
//       upload.files for these files; --encrypt encrypts them (with the passphrase in F, one
//       line, when given) whatever the destination's Encrypted links setting says.
//   putgd-cli open URL|- [-o FILE] [--passphrase-file F]   (- reads the link from standard input)
//       downloads an encrypted link and decrypts it here. It first checks the page (parsePage):
//       the bytes before and after the one data element must equal put.gd's pinned viewer
//       around its marker (the two-file form's CSP has connect-src 'self' in place of 'none'),
//       and the element must be the inline form's base64 or the two-file form's bin name. The
//       key is the link's fragment, which is never sent.
//   putgd-cli --encrypt-probe URL|-
//       the resident's Test check: fetches URL (or one line of standard input) as a visitor
//       and reports how it was served.
// Each prints one JSON line.
namespace putgd::cli {
bool isEncryptCommand(int argc,char** argv);
int runEncryptCommand(int argc,char** argv);
}
