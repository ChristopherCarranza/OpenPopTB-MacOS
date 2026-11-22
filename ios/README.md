# iOS / iPadOS build (experimental)

Runs the recompiled game on iPhone and iPad. Unlike the Mac app, nothing is compiled on the device
(iOS forbids it): the game code is generated on the Mac from your own copy and linked into the app.

## Variants
- `tb` (default): software renderer, `popTB.exe` -> app "Populous TB" (`com.unionworx.openpoptb`).
- `d3d`: Direct3D build, `D3DPopTB.exe`, 3D scene at the screen's resolution -> "Populous 3D"
  (`com.unionworx.openpoptb.d3d`). Needs SDL_GPU (Apple GPU family 3+). In the simulator
  `build_ios.sh sim` applies `sdl-simulator-gpu.patch` to the local SDL copy, because the
  simulator's GPU reports no Apple family.

## Build
Needs Xcode, SDL3 source in `../ios-deps/SDL` (tag `release-3.4.20`), and `.venv` with
`capstone pefile cmake ninja`.
```sh
ios/build_ios.sh sim                                   # build-ios-sim/PopulousTB.app
POPTB_VARIANT=d3d ios/build_ios.sh sim                 # build-ios-sim-d3d/
ios/build_ios.sh device "" <identity SHA1> <profile>   # signed, build-ios-dev/
```
### From Xcode
```sh
ios/build_ios.sh xcode                    # build-xcode/openpoptb_ios.xcodeproj
POPTB_VARIANT=d3d ios/build_ios.sh xcode  # build-xcode-d3d/openpoptb_ios.xcodeproj
```
Open the project, pick the `PopulousTB` scheme and your iPhone/iPad as destination, then Run.
Signing is automatic with the Union Worx team (9A3R3VAZ8B) by default; `POPTB_TEAM=<id>` picks another,
`POPTB_TEAM=` leaves it to choose under Signing & Capabilities. A free
Personal Team works (add your Apple ID in Xcode > Settings > Accounts): the app needs no special
entitlement, but the install expires after 7 days (Run again to renew) and a free team can have at
most 3 of its apps on a device. Generating the
project takes a few minutes (thousands of generated functions); re-run the command after pulling
changes, the folder is disposable.

`POPTB_BUNDLE_ID` overrides the bundle id. The game folder defaults to
`~/Library/Application Support/OpenPopTB/Game` (where the Mac app unpacks the GOG installer).

## Game data
The app reads the game from its `Documents/Game` folder (visible in the Files app and Finder, so
saves land there too). Copy the game folder in once, e.g.:
```sh
xcrun devicectl device copy to --device <udid> --domain-type appDataContainer \
  --domain-identifier com.unionworx.openpoptb --source <game folder> --destination Documents/Game
```

## Touch input
The game only reads relative mouse motion and keeps its own cursor per screen, each clamped to a
rect (whole screen in menus, the 512x384 view in a level). A touch first sends a huge up-left
motion ("homing") so every cursor pins to its rect's corner, then the distance from that corner
(read from the mouse thread object: `popTB.exe` 0x973e94, `D3DPopTB.exe` 0xd0595c, rect at +0x38)
to the finger; the press follows 40 ms later. Two-finger tap = right click.

## Debugging
`POPTB_DEBUG_CMDS=1` makes the app read commands from `Documents/poptb_cmd.txt` twice a second and
append results to `Documents/poptb_dbg.txt`: `find X Y T`, `filt X Y T`, `peek ADDR N`,
`poke ADDR V`, `quad A B C D`, `rect`, `key NAME`. The input script (`--script`) also takes
`ttap:x,y` (touch tap in game coordinates).
