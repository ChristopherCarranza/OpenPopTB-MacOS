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

## Touch input (ios/touch_ios.inc)
| Gesture | Does |
|---|---|
| Tap | left click (on release, so a drag never clicks) |
| Long press (0.5 s) | right click (query) |
| Two-finger tap | right click |
| One-finger drag | scroll the map, joystick style (arrows / Ctrl+arrows held while dragged) |
| Two-finger sideways drag | rotate the camera |
| Pinch / spread | zoom out / in, one step per 15% change in finger spread (stops when the fingers stop) |
| Buttons, top right (in levels only) | MENU (Esc), PAUSE (P), SKIP flyby (Space), SHAMAN (.), SAVE (F9), WORLD (Return); command groups 1-6 below |
| Thumb buttons, bottom right | SHIFT / CTRL / ALT (in the black margin beside the game on wide screens) |

SHIFT / CTRL / ALT: hold one with your right thumb while tapping the game with the other hand, or
tap it to apply to the next tap, long press or button (lit yellow, then off); tap twice quickly to
lock it on (orange), tap again to unlock. Fingers on buttons don't count towards gestures. With
groups: SHIFT+n assigns the selected followers, n selects, CTRL+n adds to the selection, ALT+n zooms.

Hardware mouse and keyboard (iPad, or any device with a mouse connected): the pointer is captured
and the game gets raw mouse movement, buttons, wheel and keys exactly like the Mac app. The on-screen
buttons hide while the mouse or keyboard is in use and come back on the next touch.

Edge scrolling: the game scrolls while its cursor sits at the screen edge. After a finger lifts,
a cursor within 4 game pixels of the edge (debug `edge N`) is moved inward so the map stops.

An iPad trackpad or mouse moves the cursor directly.

The game reads only relative mouse motion and keeps its own cursor per screen, each clamped to a
rect (whole screen in menus, 64,48-576,432 in a software-rendered level), read from the mouse thread
object (`popTB.exe` 0x973e94, `D3DPopTB.exe` 0xd0595c: cursor at +0x20, rect at +0x38). When the
game's cursor matches ours the move is sent directly; otherwise ("homing") a huge up-left motion pins
every cursor to the rect's corner, then the distance from that corner to the finger is sent. Frames
drawn while homing are not shown, so the cursor never visibly jumps to the corner.

The buttons show only while a level is loaded: the game's mode word (`popTB.exe` 0x599ab4,
`D3DPopTB.exe` 0x5d45a8) is 1 in menus, world select and cutscenes, 3 in a level.

## Audio stalls
Videos (and the intro) pace themselves on DirectSound play positions, which only move when the audio
device asks for sound. If the device stops (phone call, Bluetooth/route change, audio server
restart), `runtime/dsound.c` advances the mixer from a watchdog thread in real time after 250 ms
of silence from the device, so nothing waits forever; the device takes over again when it returns.

## Debugging
`POPTB_DEBUG_CMDS=1` makes the app read commands from `Documents/poptb_cmd.txt` twice a second and
append results to `Documents/poptb_dbg.txt`: `find X Y T`, `filt X Y T`, `peek ADDR N`,
`poke ADDR V`, `quad A B C D`, `rect`, `key NAME`, `edge N`, `finger down|up|move ID X Y` (inject touches, window points), `layout`, `stat` (cursor moves, homings, zoom steps, edge moves, held modifiers), `dump NAME` (0x400000-0x1000000 to
`Documents/dump_NAME.bin`, for diffing game state between screens), `stall 1|0` (ignore the audio
device, to test the stall watchdog). The input script (`--script`) also takes
`ttap:x,y` (touch tap in game coordinates).
