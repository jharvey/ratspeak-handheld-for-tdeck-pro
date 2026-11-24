#!/bin/sh
# Build RatSpeak-ATAK-bridge.apk without Gradle: aapt2 (manifest), javac, d8, zipalign, apksigner.
# Needs JDK 17 and the Android SDK (platforms;android-34, build-tools;34.0.0) under $ANDROID_HOME
# (default ~/android-sdk). On VM100: tools/android/build_apk.py runs this there and copies the APK
# back to firmware/RatSpeak-ATAK-bridge.apk.
# Signing key: ~/.android-keys/ratspeak-bridge.jks (made on the first build, password in
# ~/.android-keys/pass). Keep it: an update must be signed with the same key to install over.
set -e
SDK=${ANDROID_HOME:-$HOME/android-sdk}
BT=$SDK/build-tools/34.0.0
JAR=$SDK/platforms/android-34/android.jar
D=$(cd "$(dirname "$0")" && pwd)
O=$D/build
VC=${VC:-1}
VN=${VN:-0.1}
rm -rf "$O"
mkdir -p "$O/classes"

"$BT/aapt2" link -I "$JAR" --manifest "$D/AndroidManifest.xml" --min-sdk-version 26 --target-sdk-version 34 \
    --version-code "$VC" --version-name "$VN" -o "$O/base.apk"
javac -nowarn -Xlint:-options -source 8 -target 8 -bootclasspath "$JAR:$BT/core-lambda-stubs.jar" -classpath "$JAR" -d "$O/classes" \
    $(find "$D/src" -name '*.java')
"$BT/d8" --lib "$JAR" --min-api 26 --release --output "$O" $(find "$O/classes" -name '*.class')
cp "$O/base.apk" "$O/unsigned.apk"
(cd "$O" && zip -q -j unsigned.apk classes.dex)
"$BT/zipalign" -f -p 4 "$O/unsigned.apk" "$O/aligned.apk"

K=$HOME/.android-keys
if [ ! -f "$K/ratspeak-bridge.jks" ]; then
    mkdir -p "$K" && chmod 700 "$K"
    head -c 24 /dev/urandom | base64 | tr -d '/+=' > "$K/pass"
    chmod 600 "$K/pass"
    keytool -genkeypair -keystore "$K/ratspeak-bridge.jks" -storepass:file "$K/pass" -keypass:file "$K/pass" \
        -alias bridge -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=RatSpeak ATAK bridge" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$K/ratspeak-bridge.jks" --ks-pass "file:$K/pass" \
    --out "$O/RatSpeak-ATAK-bridge.apk" "$O/aligned.apk"
"$BT/apksigner" verify "$O/RatSpeak-ATAK-bridge.apk"
ls -l "$O/RatSpeak-ATAK-bridge.apk"
