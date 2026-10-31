#!/bin/sh
# Notify running Gtk/St icon themes after all icon files are in place. A cache
# copied from a prior installation must not keep an old or fallback menu icon.
# Best effort: a cache refresh failure must never fail the package transaction.
if [ -d /usr/share/icons/hicolor ] &&
   command -v gtk-update-icon-cache >/dev/null 2>&1; then
    touch /usr/share/icons/hicolor || :
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor >/dev/null 2>&1 || :
fi
exit 0
