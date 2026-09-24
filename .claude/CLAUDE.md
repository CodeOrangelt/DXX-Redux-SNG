# CLAUDE.md — DXX-Redux-SNG

Private working notes (gitignored). Descent 1 & 2 source port, forked from
[DXX-Retro](https://github.com/CDarrow/DXX-Retro) (itself a DXX-Rebirth descendant).
"SNG" is this fork's branding. ~773 commits.

## Layout

`d1/` and `d2/` are **independent CMake projects** with near-duplicate source trees
(`main/`, `arch/`, `2d/`, `3d/`, `misc/`, `texmap/`, `iff/`, `mem/`, `maths/`).
There is no shared core — each has its own `CMakeLists.txt` and `vcpkg.json`.

**Critical consequence:** a fix in `d1/main/foo.c` almost certainly needs porting to
`d2/main/foo.c` and vice versa. Always `diff -u d1/main/foo.c d2/main/foo.c` after
touching either. This has bitten twice on `dxma.c` (see Known Traps).

Other top-level: `contrib/packaging/{linux,macos,windows}/build_package.sh`,
`.github/workflows/package-*.yml`, `cmake/`, `docs/` (empty),
`dxma_missions_complete_with_direct_links.csv` (1805 missions, compiled into the binary).

### Lineage
SNG began as [`CodeOrangelt/D1X-SNG`](https://github.com/CodeOrangelt/D1X-SNG)
(D1-only, last release `D1X-SNG-1.7` "New Beginnings", 2023-03-14) before migrating
onto the Redux base. Everything from that repo carried over — it is the origin of
the CTF/KOTH/LMS modes, the HUD work, and the netgame toggles below. Treat 1.7's
release notes as the changelog for everything predating this repo.

### SNG-specific features (vs upstream)
- `survival.c/h` — Survival mode (**both** d1 and d2)
- `race.c/h`, `racebot.c/h`, `race_energy_icon*.c/h` — Race mode (**d2 only**)
- `dxma.c/h` — DXMA mission browser: fetch/refresh/filter missions from sectorgame.com
- `gns_bridge.cpp/h` — GameNetworkingSockets ICE P2P NAT traversal
- Multi-tracker support (dxxtracker.com), UPnP port-forwarding, relay server
- D1 Arcade mode, static powerups, pitched sample playback in SDL_mixer backend
- HUD/UI: 3-color ping, yellow DEMO/FPS hugging the HUD, total-time counter,
  multi-color weapon cycle, improved bomb/missile/shield gauges, "Proxy" not "P"

### Game modes — two separate mechanisms
`GMNames[]` in `main/multi.c` is the authoritative mode list, and **d1 and d2 differ**:

- **d1** (`MULTI_GAME_TYPE_COUNT` 12): Anarchy, Team Anarchy, Robo Anarchy, Cooperative,
  *Unknown ×3*, Bounty, Capture Flag, Turkey Shoot, Arcade, Survival
- **d2** (9): Anarchy, Team Anarchy, Robo Anarchy, Cooperative, Capture the Flag,
  Hoard, Team Hoard, Bounty, Race

The three `"Unknown"` slots at d1 indices 4–6 are placeholders reserving d2's
CTF/Hoard/Team Hoard positions — keep them aligned, don't reuse the indices.

**King of the Hill and Last Man Standing are not in `GMNames`** — they are netgame
*toggles*. When adding a mode, decide deliberately which mechanism it belongs to.

### Netgame toggle menu — `d1/main/net_udp.c` ~5130-5215
One contiguous block, split by comment banners into `SNG GAME MODES`,
`SNG TOGGLES & FEATURES`, and `REDUX ADVANCED OPTIONS`. Each entry is an
`opt_*` index plus an `NM_TYPE_CHECK`/`SLIDER` row backed by a `Netgame.*` field.
Adding one means touching the menu block, the `Netgame` struct, and the protocol
version. Current SNG entries:

| Menu label | `Netgame` field | Notes |
|---|---|---|
| King of the Hill | `PointCapture` | capture logic in `fuelcen.c:665`; `ScoreGoal` slider is KOTH-only and greys out otherwise |
| Last Man Standing | `Deathmatch` | |
| No Fusion Flash | `PurpleFlash` | 1.7's "Fusion Flash Removal" |
| No Fusion Shake | `FusionShake` | 1.7's "Fusion Bump Removal" |
| Fast Doors | `FastDoor` | |
| Dark Smart Blobs | `DarkSmartBlobs` | |
| Quiet Fan | `QuietFan` | |
| No Weapon Stun | `WeaponStun` | |
| Vulcan Overheat | `VulcanShake` | |
| Select Static Weapons… / Start Mission With… | submenus | `NM_TYPE_MENU` |

Redux-side (not SNG): `HomingUpdateRate`, `RemoteHitSpark`, `AllowCustomModelsTextures`,
`ReducedFlash`, `DisableFOVChange`, `BrightPlayers`, `ShowEnemyNames`, `AllowPreferredColors`.

### HUD / UI feature map
- **Tri-color ping** — `gauges.c:2530-2555`. Thresholds >100 / >70 / >1 call
  `fontcolor_bad()` / `fontcolor_ehh()` / `fontcolor_good()`, defined at
  `gauges.c:2312-2322`. Packet-loss display sits directly below and is
  `Netgame.RetroProtocol`-gated. Ported to d2 in `8f857d8`.
- **FPS counter** — `gamerend.c:129-164` (`show_framerate()`); call site
  `gamerend.c:497`, gated on `GameCfg.FPSIndicator`.
- **DEMO indicator** — `gamerend.c:458-492`; `PlayerCfg.DemoRecordingIndicator`
  picks text vs. a red disk, and `PlayerCfg.AutoDemoHideUi` suppresses it.
- **Total-time counter** — `gamerend.c:264`; also `gamecntl.c:303`, `scores.c:183`.
- **Shield indicator** — `gauges.c:2755` and `4197` (pulse below 30).
- Arcade-mode HUD banners/labels — `gauges.c:4472` and `4537` (`// SNG:` comments).

> **Note:** 1.7's notes describe DEMO and FPS as yellow. In the current code FPS is
> green (`BM_XRGB(0,31,0)`) and DEMO is red (`BM_XRGB(27,0,0)`). The 1.7 notes are a
> reliable index of *which* features exist, but not of their present styling —
> verify against source before quoting them.

## Build

CMake options (identical in d1 and d2): `OPENGL`, `SDLMIXER`, `ASM`, `EDITOR`, `IPV6`,
`UDP`, `TRACKER`, `GNS`, `UPNP`, `PNG`, `OPENGLMERGE`. All default ON except `ASM`/`EDITOR`/`IPV6`.

### Linux (VS Code Run & Debug)
Uses **CMake presets**. `CMakePresets.json` is tracked; `CMakeUserPresets.json` is
gitignored/personal and defines `linux-debug-local` / `linux-release-local`
(these add `-std=gnu17 -Wno-implicit-function-declaration -Wno-implicit-int`).

```bash
cmake --preset linux-debug-local -S d1
cd d1 && cmake --build --preset linux-debug-local -j$(nproc)
```
Output: `d1/out/build/linux-debug-local/main/d1x-redux-sng` — exactly where
`.vscode/launch.json` expects it. **Deleting `d1/out`/`d2/out` breaks Run & Debug**
(they look like stale IDE dirs but are live build targets).

Deps (Debian/Ubuntu): `build-essential git cmake libphysfs-dev libsdl1.2-dev
libsdl-mixer1.2-dev libpng-dev libglew-dev`

### Windows cross-build from Linux
See `~/.claude/projects/-home-malachi-DXX-Redux-SNG/memory/windows-cross-build-recipe.md`
for the full recipe and its traps. Summary: mingw toolchain at
`~/mingw-build-src/toolchain.cmake`, sysroot `~/mingw64-sysroot`. Requires
`-DOPENSSL_USE_STATIC_LIBS=ON` and `-DCMAKE_CXX_FLAGS="-DCMSG_LEN=WSA_CMSG_LEN"`.

The sysroot now has static SDL_mixer + ogg/vorbis/FLAC/mikmod/mad, so `-DSDLMIXER=ON`
works and produces a **fully static** exe (imports only Windows system DLLs).
Result is a legitimate stopgap, but **CI remains the reference for releases**.

## Releases & CI

Repo is `CodeOrangelt/DXX-Redux-SNG`. **There is also an `upstream` remote
(`dxx-redux/dxx-redux`) and no gh default is set — bare `gh` commands silently answer
about upstream.** Always pass `--repo CodeOrangelt/DXX-Redux-SNG`. A read that merely
looks empty or stale is the dangerous case.

Released by pushing a tag; `package-all.yml` fans out to the per-platform workflows.
Builds fine: Linux (ubuntu-22.04), Windows 64 (MSYS2 MINGW64, windows-2022),
macOS Intel + arm64. **Dead:** windows32 (MSYS2 dropped all `mingw-w64-i686-*`),
MSVC (vcpkg/CMake pinning, broken since 2026-08). Neither blocks a release.

Packaging scripts derive version from `git tag --points-at HEAD`; the Windows script
names zips by commit hash (`-win-<hash>.zip`) and must be renamed for release assets.
macOS builds cannot be produced locally — CI only.

Replace a release asset with `gh release upload <tag> <file> --repo ... --clobber`.

## Known traps

- **Duplicate d1/d2 source** — see Layout. `dxma.c` diverged twice this way.
- **`dxma_mission` arrays are ~3.8MB** (`MAX_DXMA_MISSIONS` 3000 × 1264B). Must be
  `d_malloc`/`d_free`, never stack locals — MinGW's 1MB thread stack overflows
  instantly while Linux's 8MB absorbs it. Windows-only crash, invisible on Linux.
- **`d2` CMake reconfigure corrupts `buildwin-d2`** — any edit to a `CMakeLists.txt`
  under `d2/` can flip Protobuf from Module- to Config-mode resolution, producing
  `gmake: *** target pattern contains no '%'`. Verify with
  `grep 'certs.pb.h:' buildwin-d2/_deps/.../GameNetworkingSockets_s.dir/build.make`
  (want `/usr/bin/protoc`, not `protobuf::protoc`); fix by `rm -rf buildwin-d2` + full reconfigure.
- **`d2/main/inferno.c` SEH guard** — `__try`/`__except` must be guarded so plain MinGW
  GCC is excluded; d1's guard is the correct one to copy.
- **`git fetch --tags` breaks tag-triggered runs** — use `--force`.
- **Retired runner labels queue forever** rather than failing. If a job hasn't *started*
  after a few minutes, check `runs-on`.

## Debugging Windows-only crashes without Windows

Reproduce under `wine <exe>` with real game data (`DESCENT.HOG`/`.PIG`, etc.) in the
same dir. `WINEDLLOVERRIDES="winedbg.exe=d"` disables Wine's crash dialog so the
process just runs or dies cleanly. A clean A/B — old exe vs new exe in one directory —
is the most reliable verification. Full technique in memory:
`windows-crash-debugging-technique.md`.

Note the game buffers `gamelog.txt` and only flushes on clean exit, so a killed
process leaves it empty.

## Environment

- Game data / user dirs: `~/.d1x-redux/`, `~/.d2x-redux/` (data in `data/`, `dosbox/`)
- Build artifacts are all gitignored and regeneratable; `~/windows-builds/` holds
  hand-built exes. `TimGM6mb.sf2` (soundfont) and `SNG-Media/` (art sources) are
  gitignored but **used by the Linux packaging script** — don't delete.
- Persistent memory lives in `~/.claude/projects/-home-malachi-DXX-Redux-SNG/memory/`.

## Code style

The language is C. Write it the way the kernel does: small functions, flat control
flow, explicit ownership, no cleverness. Prefer deleting code to adding it.

### Comments
**Default to none.** Code that needs a comment to be understood should be rewritten
until it doesn't. When a comment is genuinely required, it must earn its line:

- A few words, not a sentence. `// heap: 3.8MB blows MinGW's 1MB stack` — good.
- Explain **why**, never **what**. `i++; // increment i` is noise.
- Non-obvious constraints, protocol quirks, and hard-won bug context are the
  legitimate cases. Everything else is a naming problem in disguise.
- Never leave commented-out code. Delete it; git remembers.
- No changelog comments, no attribution, no `// TODO` without an owner.

Existing `// SNG:` markers denote fork-specific divergence from upstream — keep that
prefix when adding to that category, since it makes fork deltas greppable.

### Humanized code
Code is read far more than written. Optimize for the reader:

- Names carry the meaning. `incoming_count`, not `n2`. A good name removes a comment.
- One function, one job. If you need a comment to separate "phases" inside a
  function, those phases are separate functions.
- Flatten. Guard-clause early returns beat nested `if` pyramids. The happy path
  should run down the left margin.
- Keep a function short enough to hold in your head at once. If it scrolls, split it.
- Consistency beats personal preference: match the surrounding file's idiom even
  where you'd write it differently.
- No magic numbers. Name the constant.

### Memory discipline
Use the project allocator from `include/u_mem.h` — never raw `malloc`/`free`:

| API | Notes |
|---|---|
| `d_malloc(size)` / `d_calloc(n,size)` | `d_calloc` zeroes |
| `d_realloc(ptr,size)` | |
| `d_free(ptr)` | **sets `ptr = NULL`** — do not NULL it yourself afterward |
| `MALLOC(var, type, count)` | typed helper; assigns and casts |

In debug builds (`!NDEBUG`) these route through `mem_malloc`/`mem_free` with
`__FILE__`/`__LINE__` tracking, plus `mem_validate_heap()` and `mem_display_blocks()`
for overwrite detection. In release they collapse to plain libc. Use the debug build
when chasing corruption.

Rules:

- **Check every allocation.** `if (!p) return 0;` immediately, before any other work.
- **One owner per allocation**, and free on *every* exit path. If a function grows a
  second early return, audit the frees before committing.
- **Big buffers go on the heap, always.** MinGW's default thread stack is 1MB against
  Linux's 8MB, so a large stack local is a Windows-only crash that never shows up in
  local testing. Anything near or above a few hundred KB is heap. See Known Traps.
- **Size arithmetic uses `sizeof` on the variable or type**, never a hardcoded width:
  `d_malloc(sizeof(dxma_mission) * MAX_DXMA_MISSIONS)`.
- **Bound every loop and copy** by the destination capacity, not the source length.
  Prefer `snprintf` over `sprintf`, and pass `sizeof(dest)`.
- Free in reverse order of acquisition; keep the innermost scope's cleanup innermost.
- Don't reuse a buffer for a second purpose. Allocate a second one; clarity is cheaper
  than the bug.

### Formatting
Tabs for indent. Braces on their own line for functions and control blocks, matching
the surrounding file. Don't reformat untouched lines — it destroys `git blame` and
buries the real change in noise.

## Conventions

- Commit subjects are prefixed by scope: `D1:`, `D2:`, `D1/D2:`, `CI:`, `README:`,
  `D2 Race Mode:`, etc.
- Network protocol changes require bumping the protocol version.
- Any change to shared d1/d2 code must be mirrored and diffed — see Layout.
