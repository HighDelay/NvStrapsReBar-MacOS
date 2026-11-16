#!/bin/sh
set -eu
app_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$app_directory"
printf '%s\n' 'NvStrapsReBar needs your macOS password to save firmware settings.'
exec /usr/bin/sudo "$app_directory/NvStrapsReBar" --interactive
