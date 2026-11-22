# Satisfactory 1.2.6 startup crash

The installed game is CL520140. The local SDK and upstream SML `dev` headers
checked on 2026-11-18 are still CL517554 (`a7e85503`).

The crash dump points to `FinishSetup` iterating `mHologramOverrides`.
The installed mod's instruction at RVA `0x14699` reads the array pointer at
`attachment + 0x658`, then reads its count at `+0x660`. The pointer is null at
the faulting instruction, RVA `0x146b7`. This is a stale native offset, not a
null buildable or a null element in a valid array.

The game's generated reflection records provide the actual member offsets:

| Member | CL517554 SDK binary | CL520140 game binary |
| --- | --- | --- |
| Conveyor attachment `mBufferInventory` | `0x618` | `0x620` |
| Conveyor attachment `mSavedDirections` | `0x620` | `0x628` |
| Conveyor attachment `mHologramOverrides` | `0x658` | `0x660` |
| Conveyor base `mConnection0` | `0x6a0` | `0x6a8` |
| Conveyor base `mConnection1` | `0x6a8` | `0x6b0` |
| Conveyor lift `mSnappedPassthroughs` | `0x868` | `0x878` |

`VLQoLRuntimeProperties.h` resolves the affected properties from their owning
runtime classes. Object arrays are checked for the expected element class and
storage size. No game SDK headers are patched and no guessed offset delta is
applied. The lift connection accessors in the old SDK are inline, so calls to
those accessors also had to be replaced.

The synthetic replicated `mIsUpwardsFlow` property uses unreflected padding.
It is now installed only if the runtime class size and neighboring reflected
offsets match the compiled layout. The original static assertion remains.
On CL520140 this fix is skipped with a warning; single-player features remain
enabled. Do not claim multiplayer flow-direction correction on this build.
Revalidate the native padding against updated headers before enabling it again.

## Build validation

`FactoryGameSteam Win64 Shipping -Module=VerticalLogisticsQoL -MaxParallelActions=4`
compiled and linked successfully on 2026-11-18. The Steam test ZIP at
`Saved/CrashFix-CL520140/VerticalFixes-Windows.zip` reuses the existing cooked
assets and Alpakit manifest, replacing only the Steam DLL and its matching PDB.
ZIP integrity and both replacement hashes were verified. Version metadata is
still 1.2.4; this is an unreleased test package, not a published version.

Installation is staged in `C:\SF\pending-installs\VerticalFixes`. Close the game
and run `C:\SF\scripts\install-pending.bat -Mod VerticalFixes` outside the
restricted session. The protected Steam install was not modified.

## Runtime validation still needed

- Launch through the main menu and confirm the log lists hologram overrides.
- Place regular and vertical mergers/splitters, including fixed up/down modes.
- Merge/split lifts through floor holes and inside a blueprint designer.
- Check lift alignment, preview arrows, and the Better Straight pipe mode.
- Dedicated servers and multiplayer are not validated by a Windows Steam build.

The repository already contained uncommitted Better Straight bend-radius work
when this crash fix was made. It was preserved.
