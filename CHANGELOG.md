# Changelog

## 1.2.6
- Removed the Better Straight pipe build mode and its setting. It has moved to the Follow Terrain (Terrain Belts) mod.

## 1.2.5
- Fixed the startup crash on Satisfactory 1.2.6 (CL520140) by resolving conveyor attachment hologram overrides through runtime reflection.
- Resolve lift passthroughs and conveyor connections through runtime reflection too, since their native offsets also changed.
- Disable the padding-based multiplayer flow-direction fix when the runtime attachment layout differs from the SDK. Single-player fixes remain enabled; multiplayer flow-direction correction needs revalidation against updated headers.

## 1.2.4
- New: Better Straight pipe build mode. Level and vertical runs with clean 90 degree bends, and scroll to choose which way the end faces.
- Better Straight replaces vanilla Straight by default. Turn this off under Mods > Vertical Fixes.

## 1.2.3
- Fixed lifts built out of a vertical attachment landing half a metre off grid, without the invisible top segment from 1.2.1.

## 1.2.2
- Renamed to Vertical Fixes.
- Fixed a crash caused by the rename.
- Removed the lift top fix, which left the top half metre of some lifts invisible.

## 1.2.1
- Ported to Satisfactory 1.2.
- Removed the half-metre fix for attachments on lifts, since placement is freeform in 1.2.
