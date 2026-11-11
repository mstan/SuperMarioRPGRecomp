#!/bin/sh
set -eu
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$HERE" || exit 1
command -v zip >/dev/null 2>&1 || { printf 'Install the zip utility, then retry.\n' >&2; exit 1; }
set --
for path in logs saves config.ini keybinds.ini last_run_report.json mods/preloaded/state.toml crash_report_*.json crash_minidump_*.dmp; do
  [ ! -e "$path" ] || set -- "$@" "$path"
done
[ "$#" -gt 0 ] || { printf 'No files found; run Start diagnostics first.\n' >&2; exit 1; }
OUTPUT="smrpg-diagnostics-$(date +%Y%m%d-%H%M%S)-$$.zip"
zip -qr "$OUTPUT" "$@"
printf 'Created %s\nContains game memory, settings and saves. Nothing was uploaded. Keep your originals.\n' "$OUTPUT"
