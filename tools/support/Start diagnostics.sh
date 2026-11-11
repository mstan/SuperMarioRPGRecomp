#!/bin/sh
set -u
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$HERE" || exit 1
mkdir -p logs || exit 1
REPORT="logs/startup-$(date +%Y%m%d-%H%M%S)-$$.log"
export SMRPG_REPLAY_RECORD=1
export SMRPG_REPLAY_DIR="$HERE/logs"
{
  printf 'Super Mario RPG diagnostic launch\n'
  date -u
  uname -a
  wine --version
  wine "$HERE/SuperMarioRPGSNESRecomp.exe" "$@"
  RESULT=$?
  printf 'Exit code: %s\n' "$RESULT"
  exit "$RESULT"
} > "$REPORT" 2>&1
RESULT=$?
printf 'Diagnostic output: %s\nAfter a failure, close the game and run: sh "Collect diagnostics.sh"\n' "$REPORT"
exit "$RESULT"
