#pragma once
// Encrypted links (#2079) in putgd-cli:
//   putgd-cli upload [--encrypt] [--passphrase-file F] [--profile ID] [--socket P] PATH...
//       upload.files for these files; --encrypt encrypts them (with the passphrase in F, one
//       line, when given) whatever the destination's Encrypted links setting says.
//   putgd-cli open URL|- [-o FILE] [--passphrase-file F]   (- reads the link from standard input)
//       downloads an encrypted link, checks the page is byte for byte put.gd's pinned viewer,
//       and decrypts it here. The key is the link's fragment, which is never sent.
//   putgd-cli --encrypt-probe URL|-
//       the resident's Test check: fetches URL (or one line of standard input) as a visitor
//       and reports how it was served.
// Each prints one JSON line.
namespace putgd::cli {
bool isEncryptCommand(int argc,char** argv);
int runEncryptCommand(int argc,char** argv);
}
