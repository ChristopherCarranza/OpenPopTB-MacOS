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
| One-finger drag | selection box (left button held from where the finger landed), as with a mouse; only the finger that started it counts (a resting thumb is ignored) |
| Two-finger drag | grab and move the world: the camera follows the fingers 1:1 (3D build, direct camera write; other builds use scroll-key pulses) |
| Two-finger twist | turn the world like a dial, 1:1 with the fingers (3D build writes the heading directly) |
| Pinch / spread | zoom out / in with the fingers: the picture scales with the finger spread (3D build, see Direct zoom; other builds step = / - once per 15% of spread) |
| Left column, top (in levels only) | MENU (Esc), PAUSE (P), SKIP flyby (Space), WORLD (Return) |
| RIGHT CLICK, bottom left (left thumb) | tap: right click where the cursor is (deselects the selection; over a unit, panel icon or building it does what a right click there does: query, zoom to the shaman...) |
| RIGHT CLICK held + one-finger drag | grab and move the world 1:1, like a two-finger drag (no selection box) |
| RIGHT CLICK held + tap on the game | right click at that spot (query, or deselect on empty ground) |
| Text fields (save game names) | the iOS keyboard comes up while the game has a text field open (D3DPopTB.exe keeps it at `0xa6905c`) and goes away when it closes; the view slides up so the row being edited stays above the keyboard; Return saves, as in the game |
| Apple Pencil | works exactly like a finger (tap, drag, RIGHT CLICK held + drag/tap); while it touches the game, other fingers on the game are ignored (palm) but the buttons still work; hovering moves the cursor where the hardware senses it |

Fingers on buttons don't count towards gestures, so RIGHT CLICK can be held with the left thumb while
the right hand drags or taps. A pan keeps going if RIGHT CLICK is let go before the finger lifts. The
SHIFT / CTRL / ALT buttons from earlier versions are still in the code but off (`THUMB_MODS`); SHAMAN,
SAVE and the group buttons were dropped (keyboard: . F9 1-6; the game's own panel has the shaman too).

On iOS the game picture is placed by the app (not SDL's letterbox): inside the horizontal safe area
(clear of camera cutouts), and in a level, in touch mode, shrunk to leave a 100-point strip on the
left for the button column (`LEFT_COLUMN`). Menus, cutscenes and mouse/keyboard play use the full size.

Hardware mouse and keyboard (iPad, or any device with a mouse connected): the pointer is captured
and the game gets raw mouse movement, buttons, wheel and keys exactly like the Mac app. The on-screen
buttons hide while the mouse or keyboard is in use and come back on the next touch.

Edge scrolling: the game scrolls while its cursor sits at the screen edge. After a finger lifts,
a cursor within 4 game pixels of the edge (debug `edge N`) is moved inward so the map stops.
While a finger drags a selection box the cursor stays those 4 pixels inside the edge, so the box can't
set off the game's own edge scrolling (which raced the map away with a finger at the bezel). Instead, in the
3D build, a box dragged into the outer 30 game pixels scrolls the camera slowly towards that edge, faster
the closer the finger is to the bezel (`DRAG_EDGE_BAND`, `DRAG_EDGE_SPEED`: 100 game pixels of view per
second at most, about a third of the game's own edge scroll).

An iPad trackpad or mouse moves the cursor directly.

The game reads only relative mouse motion and keeps its own cursor per screen, each clamped to a
rect (whole screen in menus, 64,48-576,432 in a software-rendered level), read from the mouse thread
object (`popTB.exe` 0x973e94, `D3DPopTB.exe` 0xd0595c: cursor at +0x20, rect at +0x38). When the
game's cursor matches ours the move is sent directly; otherwise ("homing") a huge up-left motion pins
every cursor to the rect's corner, then the distance from that corner to the finger is sent. Only outside a
level and with no button held: in a level the corner sweep makes the game edge-scroll the map for a few
frames (seen on every Apple Pencil tap, whose hover leaves the game a move behind at touch-down), and with a
button held it presses on the minimap. There the move carries on from the game's own cursor instead. Frames
drawn while homing are not shown, so the cursor never visibly jumps to the corner.

The buttons show only while a level is loaded: the game's mode word (`popTB.exe` 0x599ab4,
`D3DPopTB.exe` 0x5d45a8) is 1 in menus, world select and cutscenes, 3 in a level.

## Direct camera (D3DPopTB.exe 1.01)
The local player's view block: `0x89d1ec` = camera centre (u16 X, u16 Z, wraps at 65536),
`0x89d1f8` high 16 bits = heading (2048 per turn); scrolling forward moves along
(sin t, -cos t) with t = (1023 - heading)·2π/2048. Writing the centre moves the view on the next frame.
Two-finger panning converts finger travel (game pixels) to world units with CAM_KX / CAM_KY
(measured at the default zoom). Note: `0x5fe308`/`0x5fe30c` change while scrolling but are not the
camera (writing them does nothing).

## Direct zoom (D3DPopTB.exe 1.01)
The zoom keys step through four camera presets (94-byte records at `0x88f0c0`, group `[0x89c6ee]` of five,
current one `[0x89c6ec]`): from close to far 3, 0, 2, 4. Each step changes distance and tilt (0 to 2 is
mostly tilt, to a top-down view). The game's scripted camera blends presets with `f_41c700(z, 0x4000)`
(preset 0 of the group towards preset 2 for z > 0, 3 for z < 0) and rebuilds the view. Pinch zoom calls it
through `guest_call` from the DirectDraw present paths (`plat_frame_hook`, on the game thread between
frames), with the two neighbouring presets copied into slots 0 and 2 for the call and put back right after,
so any point between any two stops can be shown. The pinch maps finger spread to a log scale along the
stops (`zweight`, measured from on-screen sizes), so doubling the spread roughly doubles the picture. It
is skipped while the scripted camera runs (`0x969bd7` bit 0: flybys, tutorial). After each change
`[0x89c6ec]` is set to the nearest stop so = / - carry on from there. Debug: `zoomu U` (U = 0..3).

## Power use
The game paces its frames by polling `GetTickCount` until a deadline (D3D: logic at 60 fps, drawing at
`DrawFrameRateLimit`). These measures keep the phone cool:
- **Frame waits sleep.** `poll_time` in `runtime/k32.c` sleeps to the next millisecond, with the GIL released,
  when the same call site polls again within a millisecond. This replaced a core spinning at 100%; the game
  speed is unchanged.
- **Guest waits sleep.** Waits (events, threads, semaphores, critical sections) sleep on a signal counter
  (`wait_seq`) instead of polling every 2 ms. The waveOut notify thread waits on the mixer mutex instead of
  polling every 5 ms.
- **Fewer clock reads.** `gil_yield` reads the clock on every 16th call only.
- **Indirect calls use a cache.** A direct-mapped cache sits in front of the function-table lookup in
  `dispatch` (`runtime/cpu.c`).
- **Lower drawing load on iOS:**
  - The 3D scene renders at 2× (1280×960) instead of up to 4× (`pick_scale` in `runtime/gpu.c`);
    `POPTB_D3D_SCALE` overrides this.
  - Drawing runs at 30 fps instead of the game's default 40, through a `DrawFrameRateLimit` registry value in
    `runtime/fileio.c`. Game logic stays at 60.
- **A cheaper overlay pass.** The 2D overlay over the 3D scene is built in one pass: compare with the 3D frame,
  then convert (`plat_present_hybrid`). Only the rectangle that changed since the last frame is uploaded.
- **The game pauses when the app isn't in front** (Control Center, a call, the app switcher, Home).
  - An SDL event watch (`app_watch`) sees the lifecycle events. The main thread then sleeps in the run loop
    (`app_pause`) and audio is paused.
  - The time away is left out of the game's clock (`clock_add_pause`), so the game doesn't fast-forward.
  - In the simulator, CPU while paused is about 1.5%.
- **The audio watchdog checks less often** while the device is healthy (every 125 ms).

On an iPhone Duo (Time Profiler, intro/menu), app CPU dropped from about 100% of a core to about 24%.
Not done yet:
- each changed texture still gets its own GPU texture and command buffer (`gpu.c`);
- each 3D frame is still read back to the CPU for compositing.

## Audio stalls
Videos (and the intro) pace themselves on DirectSound play positions, which only move when the audio
device asks for sound. If the device stops (phone call, Bluetooth/route change, audio server
restart), `runtime/dsound.c` advances the mixer from a watchdog thread in real time after 250 ms
of silence from the device, so nothing waits forever; the device takes over again when it returns.

## Debugging
`POPTB_DEBUG_CMDS=1` makes the app read commands from `Documents/poptb_cmd.txt` twice a second and
append results to `Documents/poptb_dbg.txt`: `find X Y T`, `filt X Y T`, `peek ADDR N`,
`poke ADDR V`, `quad A B C D`, `rect`, `key NAME`, `mouse down|up|move X Y` (raw left button, game pixels), `edge N`, `finger down|up|move ID X Y [pen]` (inject touches, window points; `pen` = as the Apple Pencil), `layout`, `stat` (cursor moves, homings, zoom steps, edge moves, held modifiers), `zoomu U` (pinch-zoom position 0..3), `touchlog` (the last 160 touches and what they turned into: clicks, homings, gestures, camera), `dump NAME` (0x400000-0x1000000 to
`Documents/dump_NAME.bin`, for diffing game state between screens), `stall 1|0` (ignore the audio
device, to test the stall watchdog). The input script (`--script`) also takes
`ttap:x,y` (touch tap in game coordinates).
