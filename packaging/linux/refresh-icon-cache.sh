#!/bin/sh
set -e
# Notify running Gtk/St icon themes after all icon files are in place. A cache
# copied from a prior installation must not keep an old or fallback menu icon.
if [ -d /usr/share/icons/hicolor ]; then
    touch /usr/share/icons/hicolor
    gtk-update-icon-cache -f -t /usr/share/icons/hicolor
fi
