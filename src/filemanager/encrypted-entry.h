// "Upload encrypted with put.gd" hides where encrypting can't work (#2136): the
// resident keeps this marker under the user's data folder while the destination Upload with put.gd goes to can't serve an
// encrypted page, or there is none. Its presence hides the entry; no marker shows
// it. The plugins check it with one access() per menu, the Nemo and FileManager-Actions
// entries with the shell test in their packaged files. Plain C, no includes: the
// resident, the GTK plugins and the Qt plugins all use it. The data folder is
// $XDG_DATA_HOME only when it is absolute; unset, empty or relative means
// $HOME/.local/share, as the XDG spec says, for the writer and every reader.
#pragma once
#define PUTGD_FM_ENCRYPTED_HIDDEN "putgd/file-manager/encrypted-entry-hidden"
