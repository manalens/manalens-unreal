#!/usr/bin/env bash
# Builds the plugin with RunUAT BuildPlugin, copies it into HostProject/Plugins,
# and runs the Manalens automation tests headless.
#
#   UE_ROOT="/Users/Shared/Epic Games/UE_5.8" sdks/unreal/Tests/run-tests.sh
#
# PACKAGE_DIR=<dir> reuses a plugin that BuildPlugin already built there.
# UPDATE_FIXTURES=1 writes the request bodies to Tests/fixtures.
set -euo pipefail

UE_ROOT="${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}"
SDK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HOST="$SDK_DIR/HostProject"

case "$(uname -s)" in
  Darwin) PLATFORM=Mac; EDITOR="$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" ;;
  Linux) PLATFORM=Linux; EDITOR="$UE_ROOT/Engine/Binaries/Linux/UnrealEditor-Cmd" ;;
  *) echo "Use Mac or Linux, or run the same steps by hand on Windows" >&2; exit 2 ;;
esac

if [ -z "${PACKAGE_DIR:-}" ]; then
  # The physical path (macOS /var is a link to /private/var), or clang cannot find the precompiled headers.
  PACKAGE_DIR="$(cd "$(mktemp -d)" && pwd -P)/Manalens"
  "$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildPlugin \
    -Plugin="$SDK_DIR/Manalens/Manalens.uplugin" \
    -Package="$PACKAGE_DIR" \
    -TargetPlatforms="$PLATFORM"
fi

rm -rf "$HOST/Plugins/Manalens"
mkdir -p "$HOST/Plugins"
cp -R "$PACKAGE_DIR" "$HOST/Plugins/Manalens"

export MANALENS_UNITY_FIXTURES="$SDK_DIR/../unity/Tests~/fixtures"
if [ "${UPDATE_FIXTURES:-}" = "1" ]; then
  export MANALENS_WRITE_FIXTURES="$SDK_DIR/Tests/fixtures"
fi

LOG="$HOST/Saved/Logs/ManalensTests.log"
mkdir -p "$(dirname "$LOG")"
set +e
"$EDITOR" "$HOST/HostProject.uproject" \
  -ExecCmds="Automation RunTests Manalens; Quit" \
  -unattended -nullrhi -nopause -nosplash -nosound -log -abslog="$LOG"
STATUS=$?
set -e

grep -E "Test Completed\. Result=|TEST COMPLETE" "$LOG" || true
PASSED=$(grep -c "Test Completed. Result={Success}" "$LOG" || true)
FAILED=$(grep -c "Test Completed. Result={Fail" "$LOG" || true)
echo "Manalens automation tests: $PASSED passed, $FAILED failed (editor exit $STATUS)"
[ "$FAILED" = "0" ] && [ "$PASSED" != "0" ]
