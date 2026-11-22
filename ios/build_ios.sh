#!/bin/sh
# Builds Populous TB for iOS/iPadOS from YOUR copy of the game.
# usage: [POPTB_VARIANT=d3d] ios/build_ios.sh sim|device|xcode [game folder] [signing identity SHA1] [provisioning profile]
#   POPTB_VARIANT: tb (software renderer, default) or d3d (Direct3D build: 3D at the screen's resolution)
#   sim:    builds for the iOS Simulator (build-ios-sim/PopulousTB.app)
#   device: builds for iPhone/iPad and signs with the given identity + profile (build-ios-dev/PopulousTB.app)
#   xcode:  generates an Xcode project (build-xcode/PopulousTB.xcodeproj) to build, sign and run from Xcode;
#           POPTB_TEAM=<team id> preselects the signing team (automatic signing)
# Needs: Xcode, SDL3 source in ../ios-deps/SDL, a venv with capstone pefile cmake ninja (.venv).
set -e
P="$(cd "$(dirname "$0")/.." && pwd)"
KIND="${1:-sim}"
GAME="${2:-$HOME/Library/Application Support/OpenPopTB/Game}"
VARIANT="${POPTB_VARIANT:-tb}"
if [ "$VARIANT" = d3d ]; then ID="${POPTB_BUNDLE_ID:-com.unionworx.openpoptb.d3d}"; SUF=-d3d; else ID="${POPTB_BUNDLE_ID:-com.unionworx.openpoptb}"; SUF=; fi
export PATH="$P/.venv/bin:$PATH"
if [ "$VARIANT" = d3d ] && [ ! -f "$P/gen_d3d/table.c" ]; then
  echo "== recompiling D3DPopTB.exe"
  "$P/.venv/bin/python" -I "$P/recomp/gen.py" "$P/gen_d3d" "$GAME/D3DPopTB.exe:$P/re/export_d3d/functions.tsv" "$GAME/QMixer.dll" "$GAME/WEANETR.dll"
elif [ "$VARIANT" != d3d ] && [ ! -f "$P/gen/table.c" ]; then
  echo "== recompiling popTB.exe"
  "$P/.venv/bin/python" -I "$P/recomp/gen.py" "$P/gen" "$GAME/popTB.exe:$P/re/export/functions.tsv" "$GAME/QMixer.dll" "$GAME/WEANETR.dll"
fi
# simulator only: SDL_GPU refuses the simulator's GPU (no Apple3 family reported); the Direct3D build needs it
SDL_SRC="$P/../ios-deps/SDL"
if [ "$KIND" = sim ] && ! grep -q "OpenPopTB local patch" "$SDL_SRC/src/gpu/metal/SDL_gpu_metal.m"; then
  (cd "$SDL_SRC" && git apply "$P/ios/sdl-simulator-gpu.patch")
fi
case "$KIND" in
  sim) B="$P/build-ios-sim$SUF"; SYSROOT=iphonesimulator ;;
  device) B="$P/build-ios-dev$SUF"; SYSROOT=iphoneos ;;
  xcode)
    B="$P/build-xcode$SUF"
    TEAMARG=""; [ -n "$POPTB_TEAM" ] && TEAMARG="-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=$POPTB_TEAM"
    cmake -S "$P/ios" -B "$B" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
      -DCMAKE_XCODE_GENERATE_SCHEME=ON -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic $TEAMARG \
      -DPOPTB_BUNDLE_ID="$ID" -DPOPTB_VARIANT="$VARIANT" >/dev/null
    echo "generated $B/openpoptb_ios.xcodeproj (scheme PopulousTB)"
    exit 0 ;;
  *) echo "usage: $0 sim|device|xcode" >&2; exit 1 ;;
esac
cmake -S "$P/ios" -B "$B" -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SYSROOT \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
  -DPOPTB_BUNDLE_ID="$ID" -DPOPTB_VARIANT="$VARIANT" >/dev/null
cmake --build "$B"
APP="$B/PopulousTB.app"
if [ "$KIND" = device ]; then
  SIGN="$3"; PROFILE="$4"
  [ -n "$SIGN" ] && [ -f "$PROFILE" ] || { echo "device build needs: <identity SHA1> <profile.mobileprovision>" >&2; exit 1; }
  cp "$PROFILE" "$APP/embedded.mobileprovision"
  TEAM=$(security cms -D -i "$PROFILE" | plutil -extract TeamIdentifier.0 raw -)
  cat > "$B/ent.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>application-identifier</key><string>$TEAM.$ID</string>
<key>com.apple.developer.team-identifier</key><string>$TEAM</string>
<key>get-task-allow</key><true/>
</dict></plist>
PL
  codesign -f -s "$SIGN" --entitlements "$B/ent.plist" "$APP"
fi
echo "built $APP"
