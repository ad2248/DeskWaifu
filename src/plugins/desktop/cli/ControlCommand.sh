#!/bin/sh
# Preserve the historical one-shot command behavior without Qt or configuration.
for argument do
    case "$argument" in
        toggle|--toggle) killall -s SIGUSR1 deskwaifu; exit 0 ;;
        focus|--focus) killall -s SIGUSR2 deskwaifu; exit 0 ;;
    esac
done
exit 2
