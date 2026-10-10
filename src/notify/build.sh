#!/bin/bash
# Rebuild the embedded notification dex (src/src/notify_dex.h) from the Java
# source. Run this after editing src/notify/java/com/rain/mod/RainNotify.java,
# then rebuild librain.so.
#
# Requires: a JDK, and Android SDK build-tools (d8).
set -e

JAVA_HOME="${JAVA_HOME:-C:/Program Files/Eclipse Adoptium/jdk-25.0.3.9-hotspot}"
SDK="${ANDROID_SDK:-$HOME/AppData/Local/Android/Sdk}"
ANDROID_JAR="$SDK/platforms/android-35/android.jar"
D8="$SDK/build-tools/35.0.0/d8.bat"

HERE="$(cd "$(dirname "$0")" && pwd)"

rm -rf "$HERE/build"
mkdir -p "$HERE/build/dex"

"$JAVA_HOME/bin/javac" -source 8 -target 8 \
    -bootclasspath "$ANDROID_JAR" \
    -d "$HERE/build" \
    "$HERE/java/com/rain/mod/RainNotify.java"

"$D8" --min-api 26 --output "$HERE/build/dex" \
    "$HERE/build/com/rain/mod/RainNotify.class"

python3 - "$HERE/build/dex/classes.dex" "$HERE/../src/notify_dex.h" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
out = [
    '#pragma once',
    '// Generated from RainNotify.java -> d8 dex. Do not edit by hand.',
    '// Rebuild: src/notify/build.sh',
    '',
    'static const unsigned char kRainNotifyDex[] = {',
]
for i in range(0, len(data), 12):
    out.append('    ' + ', '.join(f'0x{b:02x}' for b in data[i:i+12]) + ',')
out += ['};', f'static const unsigned int kRainNotifyDexLen = {len(data)};', '']
open(dst, 'w').write('\n'.join(out))
print(f'wrote {dst} ({len(data)} bytes)')
PY

echo "done - now rebuild librain.so"
