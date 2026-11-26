#pragma once
// Sealed links (#2079) in putgd-cli:
//   putgd-cli upload [--seal] [--passphrase-file F] [--profile ID] [--socket P] PATH...
//       upload.files for these files; --seal seals them (with the passphrase in F, one
//       line, when given) whatever the destination's Seal links setting says.
//   putgd-cli open URL|- [-o FILE] [--passphrase-file F]   (- reads the link from standard input)
//       downloads a sealed link, checks the page is byte for byte put.gd's pinned viewer,
//       and decrypts it here. The key is the link's fragment, which is never sent.
//   putgd-cli --seal-probe URL|-
//       the resident's Test check: fetches URL (or one line of standard input) as a visitor
//       and reports how it was served.
// Each prints one JSON line.
namespace putgd::cli {
bool isSealCommand(int argc,char** argv);
int runSealCommand(int argc,char** argv);
}
