// SolarOrbzChunkLODPolicy - checklist item 2 of Docs/ChunkedPlanetTerrain.md's "Phase 1, continued"
// section: "what depth should a given chunk be, for a viewer at this position?" A pure numeric
// heuristic, no engine/actor dependencies (no AActor, no camera manager, no UWorld) - just plain
// FVector math, so it can be unit-tested (or, here, Python-sanity-checked - see the verification
// note below) without a compiler. Everything below is first-pass tuning, not a finished answer - see
// each section's own "known limitation" callouts.
//
// What this file deliberately does NOT do (see Docs/ChunkedPlanetTerrain.md item 3 onward):
//   - It does not walk the quadtree. It answers "should THIS ONE chunk split?" given its own 3
//     corners - the residency walker (item 3) is the thing that starts at the 20 base faces and
//     recursively calls this per node, stopping when it says "false" or MaxDepth is hit.
//   - It does not enforce the 1-level neighbor-depth restriction (item 4) - that's a constraint
//     across MULTIPLE chunks' decisions, which this single-chunk function has no way to see.
//   - It does not know about resident state, chunk lifecycle, or skirts (items 5-6).
//
// --- Input shape: pre-scaled world-space positions, not unit directions + radius ---
// FSolarOrbzChunkAddress::GetCornerUnitDirections() returns UNIT-SPHERE directions (radius 1) - the
// chunk system's addressing is deliberately radius-agnostic. This policy instead takes already-
// world-scaled corner positions (i.e. CALLER does `Direction * Radius` first) and a viewer position
// in that same space, both relative to the planet's center. Two reasons for pushing the scaling step
// onto the caller instead of taking (UnitDirection, Radius) here directly:
//   1. The viewer position is already naturally in world space (wherever the camera/pawn actually
//      is) - taking unit directions here would force THIS function to re-derive the planet's actor
//      transform/origin just to make the units match, which is exactly the kind of engine-shaped
//      dependency this is supposed to avoid.
//   2. A future terrain-displaced chunk (once real height data affects corner altitude, not just
//      direction) still fits this same signature unchanged - "3 world-space points + a viewer
//      world-space point" doesn't care whether those points sit exactly on the ideal sphere or not.
//      Today's caller (the undisplaced-sphere chunk generator) happens to pass
//      `UnitDirection * Radius`, but that's the caller's choice, not something baked in here.
// Both CornerA/B/C and ViewerWorldPosition must be in the SAME linear unit (UE's cm "world units" if
// that's what the caller's Radius was already in, or meters, or anything else) and in the SAME
// reference frame (relative to the planet's center - e.g. `WorldPawnLocation - PlanetActorLocation`,
// not raw UWorld-space, since this function has no idea where the planet actor itself sits). Getting
// either of those wrong silently produces nonsense ratios, not a compile or runtime error - there's
// no way for a pure FVector function to validate either on its own.
//
// --- Core metric: (chunk's longest edge) / (distance from viewer to the chunk) ---
// This is the standard "projected size over distance" proxy for angular/screen size used all over
// real-time terrain LOD (CDLOD, geometry clipmaps, etc.) when no camera FOV/resolution is available
// to compute an exact screen-space error in pixels: for a chord of length E viewed from distance D
// (D >> E), the subtended angle is ~ E/D radians (exact for a true angular measure would be
// `2*asin((E/2)/D)`, but asin(x) ~ x for the small angles this is used at, so the division alone
// captures the same direction/shape without a trig call every chunk, every update). Bigger edge or
// smaller distance => bigger ratio => wants to split further; the opposite shrinks it. This doesn't
// know the actual camera FOV or screen resolution (neither is available to a pure, engine-free
// function) - the tunable threshold below is standing in for "how big is too big," and MUST be
// re-tuned once an actual camera is looking at actual chunks on screen; see "Known limitations."
//
// Why the chunk's LONGEST edge (not shortest, not an average): the longest edge is the chunk's worst-
// case screen footprint - a long thin sliver chunk (which these spherical-triangle chunks can be,
// especially near a pentagon vertex where a corner angle is small) still needs splitting if ONE of
// its edges is huge on screen, even if its other two edges or its area are comparatively modest. A
// shortest-edge or area-based metric would under-split exactly that shape.
//
// Why "distance to the chunk" = min(distance to each of the 3 corners, distance to the chunk's own
// centroid re-projected onto the sphere): the design doc's own wording for this item offers a choice
// between "nearest corner" and "centroid projected back onto the sphere" - this uses BOTH, as a min,
// rather than picking just one, for a concrete reason: nearest-corner alone has a real blind spot a
// single corner-only metric would miss. A viewer standing almost directly beneath a LARGE chunk's
// own interior (not near any of its 3 corners - e.g. still-shallow, still-huge Depth 2-4 chunks, well
// before the quadtree has had a chance to localize down near the viewer) is very close to the
// chunk's true surface, but could be FAR from all 3 corners, which sit out at the chunk's edges. A
// corners-only metric would under-estimate how close the chunk actually is and under-split right
// when it matters most. The re-projected centroid adds exactly the "point near the middle" sample
// that fixes that case, at the cost of one extra FVector and one extra Dist() call - cheap. This is
// still only an approximate closest-point-on-a-spherical-triangle (the TRUE closest point can sit on
// an edge's interior, matching neither a corner nor the centroid), so it can still mildly over-
// estimate distance - generally the safer direction to be wrong in (see ComputeNearestDistance's own
// comment for the precise guarantee this does and doesn't make: the 3 corners are exact points on the
// chunk's surface, so distance-to-nearest-corner ALONE is a true upper bound on the real closest-
// surface distance, no caveats; the re-projected centroid is only an approximate "near the middle"
// point, not guaranteed to sit exactly on the surface, so it does not carry that same ironclad
// guarantee - in a sufficiently distorted/oversized triangle it could in principle sit slightly closer
// to the viewer than the true surface does, undercutting the corner-only bound rather than only ever
// tightening it. Not a practical concern at the triangle sizes/depths this will actually run at
// (Depth 2+, where triangles are no longer a meaningful fraction of the whole sphere - see the
// concrete depth-by-depth edge lengths below), but called out here rather than silently overclaimed).
//
// --- Threshold default: SplitScreenSizeRatio = 1.0 ---
// Picked, then checked, by hand-computing (see this task's sanity-check numbers, reproduced in the
// accompanying report/commit message - the Python script itself lives outside the repo per this
// task's scope, not checked in here) the ratio this metric actually produces at depths 0-24 for an
// Earth-like Radius = 6,371,000 m, descending a chunk chain that keeps one corner pinned to the
// viewer's own position (so "distance to nearest corner" is exactly the viewer's altitude above the
// ground, the worst case for how close a chunk can get):
//   - Viewer standing at a plausible eye height (altitude 1.8 m): ratio crosses below 1.0 between
//     Depth 22 (ratio ~1.10) and Depth 23 (ratio ~0.55) - i.e. this policy settles at Depth 23, one
//     short of FSolarOrbzChunkAddress::MaxDepth (24). Sub-2-meter chunk edges at ground level, which
//     is exactly the kind of resolution the whole design doc exists to reach (see its "Why this
//     exists" section's point that a single whole-planet mesh can't get anywhere near this).
//     Crucially, this is NOT "wants MaxDepth with no margin" (margin to spare is a good sign the cap
//     isn't the thing actually constraining the decision) and NOT "stays shallow" (a 1.8 m-tall
//     viewer getting a multi-kilometer chunk under their feet would be an obviously-wrong result in
//     the other direction).
//   - Viewer in a 400 km (ISS-like) orbit: ratio crosses below 1.0 between Depth 4 (ratio ~1.29) and
//     Depth 5 (ratio ~0.65) - a far, slow-moving view only wants shallow depth, also as expected.
// A ratio-of-1 threshold reads naturally too: "stop splitting once this chunk's longest edge is no
// longer bigger than its own distance to the viewer" - i.e. once the chunk is no longer close enough
// to subtend a very large angle. This is a first-pass constant, explicitly expected to be re-tuned
// once real rendering is on screen to look at (same framing the design doc itself uses for this
// whole item - "tuning the exact threshold is a later pass once something is actually on screen").
//
// --- Stability / hysteresis ---
// A single threshold compared every update WILL flicker for a chunk sitting (or a viewer moving)
// right at the boundary - one update's floating-point rounding/viewer-jitter nudges the ratio from
// 0.999 to 1.001 and the decision flips, over and over, for a chunk that hasn't meaningfully changed
// size or distance. This is addressed here with the standard two-threshold ("split-high, merge-low")
// dead zone, as two SEPARATE functions rather than one function with a mode flag:
//   - ShouldSplit(...) uses Settings.SplitScreenSizeRatio - call this for a chunk that is CURRENTLY a
//     leaf, deciding whether to split it one level deeper.
//   - ShouldMerge(...) uses Settings.MergeScreenSizeRatio (meaningfully LOWER than the split
//     threshold - see FSolarOrbzChunkLODSettings' own comment) - call this for a chunk whose children
//     are CURRENTLY resident, deciding whether to collapse them back and make this chunk the leaf
//     again instead.
// As long as MergeScreenSizeRatio < SplitScreenSizeRatio, a chunk whose ratio sits anywhere in
// between the two thresholds stays exactly as it already is - already-split stays split, already-leaf
// stays leaf - regardless of which function gets called on it, and that gap is what actually stops
// the flicker (neither function alone is "the fix"; the GAP between the two thresholds is). This is a
// real, load-bearing piece of the hysteresis story, not just a note-for-later - but one real gap
// remains, honestly flagged rather than silently assumed solved: hysteresis on its own only stops a
// decision from flipping back and forth at a FIXED viewer position/chunk size. It does not stop a
// steadily-moving viewer from eventually crossing even the wider gap and triggering a split/merge
// right as they pass through it - expected and fine (the chunk legitimately needs to change
// eventually), but a residency manager that recomputes very frequently while a viewer sits and
// oscillates around exactly the boundary (e.g. a ship circling at constant altitude right across a
// split/merge line) could still thrash between calling ShouldSplit and ShouldMerge on consecutive
// updates. The usual real fix for that (hold a decision for a minimum number of updates/seconds once
// made, independent of the metric) needs actual per-chunk resident STATE and an update cadence to
// attach to - neither of which a pure, stateless function can own. Left as a known, explicitly-
// flagged follow-up for the residency manager (item 5 in the design doc's checklist), not solved here.
//
// --- MaxDepth ---
// This function does not own enforcing FSolarOrbzChunkAddress::MaxDepth as a hard ceiling - the tree
// walker (item 3) does, by simply not calling GetChildren/recursing past it regardless of what this
// says. But taking an optional CurrentDepth lets any caller get that enforcement for free rather than
// having to duplicate an "if (Depth >= MaxDepth) don't split" check at every call site - pass
// INDEX_NONE (the default) to skip this and let ShouldSplit answer purely from geometry, or pass the
// chunk's actual Depth to have ShouldSplit itself refuse to recommend splitting past MaxDepth.
//
// --- Verification note ---
// Per this task's own framing, this is "a pure numeric heuristic, not an exact topological
// algorithm" - the exhaustive brute-force verification this plugin otherwise holds itself to for
// GetEdgeNeighbor/GetPentagonVertexNeighbors (see Docs/ChunkedPlanetTerrain.md) isn't the right bar
// here, since there is no single ground-truth "correct" split/stay answer to check against - only a
// directional one ("closer/bigger should want more depth than farther/smaller", which is monotonic in
// the ratio by construction, not something that needs a search to confirm). What WAS checked by hand,
// independent of this C++ (same depth-by-depth numbers quoted above): the actual ratio values this
// produces across the realistic range it will run at (Depth 0-24, Earth-like Radius, viewer altitudes
// from orbit down to standing-height), confirming the chosen threshold lands in a sane middle ground
// rather than either extreme this task specifically called out as a failure mode ("requesting depth
// 24 for someone in orbit, or depth 0 for someone standing on the ground").

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzChunkLODPolicy.generated.h"

/**
 * Tunable thresholds for FSolarOrbzChunkLODPolicy. USTRUCT/BlueprintType (matching this plugin's
 * existing convention for small config structs, e.g. FSolarOrbzAtmosphereGas in SolarOrbzProfiles.h)
 * so a future AASolarOrbzChunkedPlanetActor (Docs/ChunkedPlanetTerrain.md item 7, not built yet) can
 * expose this directly as a UPROPERTY and let a planet designer tune it per-body without a recompile.
 */
USTRUCT(BlueprintType)
struct SOLARORBZ_API FSolarOrbzChunkLODSettings
{
	GENERATED_BODY()

	/**
	 * Split a leaf chunk further once (its longest edge / its distance to the viewer) exceeds this.
	 * Smaller = splits sooner/deeper (more detail, more chunks resident, more generation cost);
	 * larger = splits later/shallower (coarser, cheaper). See this file's own top-of-header comment
	 * for how the default of 1.0 was picked and checked against concrete depth/distance numbers -
	 * this is a first-pass constant, expected to be re-tuned once real rendering is on screen.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|LOD", meta = (ClampMin = "0.01"))
	double SplitScreenSizeRatio = 1.0;

	/**
	 * The companion "merge back to a leaf" threshold ShouldMerge compares against, for a chunk whose
	 * children are CURRENTLY resident - deliberately LOWER than SplitScreenSizeRatio so the two
	 * thresholds don't sit on top of each other. The gap between them is a dead zone that stops a
	 * chunk sitting right at the boundary (or a viewer jittering across it) from splitting and
	 * merging every update - see this file's "Stability / hysteresis" comment for the full reasoning
	 * and its one known remaining gap. Not hard-clamped to stay below SplitScreenSizeRatio (this is a
	 * plain data struct, no custom PostEditChangeProperty to enforce it) - a designer who sets this
	 * >= SplitScreenSizeRatio just gets the dead zone back down to zero (no hysteresis, not a crash),
	 * so the invariant is documented here rather than enforced. Default of 0.75 (75% of the default
	 * split threshold) is an arbitrary but ordinary dead-zone width for this kind of two-threshold
	 * scheme - not independently derived from the same depth-by-depth numbers the split default was.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|LOD", meta = (ClampMin = "0.01"))
	double MergeScreenSizeRatio = 0.75;

	/**
	 * The actual ground resolution this planet aims for at the closest LOD, in meters per quad edge -
	 * e.g. the default 2.0 means a 2x2 meter (4 square meter) quad, the "4 meter scale" ground
	 * resolution target. Set to 1.0 for a 1x1 meter quad instead. This exists because the screen-size-
	 * ratio metric above (SplitScreenSizeRatio) has NO awareness of a chunk's internal triangle density
	 * (`ChunkResolution` on the owning actor) - left alone, it keeps splitting purely by on-screen
	 * angular size, which for a standing-height viewer on an Earth-like planet settles around Depth 23
	 * (see FSolarOrbzChunkLODPolicy's own header, "Threshold default" section) - already SUB-METER per
	 * CHUNK before `ChunkResolution` even subdivides it further, so a default ChunkResolution of 16
	 * would produce ~5 cm quads: roughly 80x finer than this field's own 2 m default, for no visual
	 * benefit and a lot of wasted chunk/triangle count. FSolarOrbzChunkLODPolicy::ComputeMaxUsefulDepth
	 * turns this field (plus Radius and ChunkResolution, both known to the resident-set manager) into a
	 * depth CEILING - "don't split further than this, further splitting only produces finer-than-target
	 * quads" - enforced as an extra cap alongside (not instead of) the screen-ratio metric: whichever of
	 * the two says stop first, stops the recursion first; this field can only make the result SHALLOWER
	 * than the screen-ratio metric would alone, never deeper. A plain planet-radius example: Earth-like
	 * Radius (6,371,000 m) with the default ChunkResolution (16) and this field at 2.0 computes an
	 * effective ceiling of Depth 18 (not 23) - at 1.0 (the "chance to go down to 1 meter" case), Depth
	 * 19 - one level deeper for 2x the density, not an explosion in chunk count.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|LOD", meta = (ClampMin = "0.01", Units = "m"))
	double TargetQuadEdgeMeters = 2.0;
};

/**
 * Pure LOD decision function(s) for one chunk - see this file's own top-of-header comment for the
 * full rationale (metric choice, threshold derivation, hysteresis, units). No UObject/AActor/UWorld
 * dependency anywhere in this class - every input is plain FVector/double data, so this can be
 * called (and was checked) from a plain C++ or even a Python-equivalent re-implementation without
 * touching the engine at all.
 */
class SOLARORBZ_API FSolarOrbzChunkLODPolicy
{
public:
	/**
	 * True if this chunk (currently a leaf) should split one level deeper, for a viewer at
	 * ViewerWorldPosition. False means it's fine to stay as-is.
	 *
	 * @param CornerA, CornerB, CornerC  The chunk's 3 corners, WORLD-SPACE (already scaled by the
	 *                                   planet's radius and offset into the same frame as
	 *                                   ViewerWorldPosition - see this file's header comment on why
	 *                                   this takes positions, not unit directions + a separate
	 *                                   radius). Order doesn't matter to this function (unlike most of
	 *                                   this plugin's geometry code, nothing here is winding-sensitive
	 *                                   - every quantity used is a distance).
	 * @param ViewerWorldPosition        The viewer's position, in the SAME space as the corners
	 *                                   (relative to the planet's center, not raw UWorld-space).
	 * @param Settings                   Tunable thresholds - see FSolarOrbzChunkLODSettings.
	 * @param CurrentDepth               Optional. Pass this chunk's actual Depth to have this function
	 *                                   refuse to recommend splitting once CurrentDepth already equals
	 *                                   or exceeds FSolarOrbzChunkAddress::MaxDepth, regardless of the
	 *                                   computed ratio - lets a tree-walker get that bound enforced
	 *                                   here for free instead of duplicating the check at every call
	 *                                   site, without this function otherwise needing to know anything
	 *                                   about chunk addressing. Leave as the default (INDEX_NONE) to
	 *                                   skip this check entirely and answer purely from geometry -
	 *                                   enforcing MaxDepth is ultimately the tree-walker's own
	 *                                   responsibility either way (see this file's header comment).
	 * @param EffectiveMaxDepth           Optional. A second, lower depth ceiling on top of
	 *                                   FSolarOrbzChunkAddress::MaxDepth - see ComputeMaxUsefulDepth's
	 *                                   own comment for how a caller derives this from
	 *                                   Settings.TargetQuadEdgeMeters, Radius, and ChunkResolution. The
	 *                                   actual ceiling applied is min(EffectiveMaxDepth,
	 *                                   FSolarOrbzChunkAddress::MaxDepth) - this parameter can only make
	 *                                   the result SHALLOWER than MaxDepth alone would, never deeper.
	 *                                   Defaults to MAX_int32 (no additional ceiling, i.e. only the
	 *                                   hard MaxDepth applies) so existing callers that don't yet pass
	 *                                   this keep their current behavior unchanged. Deliberately typed
	 *                                   as a plain int32 rather than FSolarOrbzChunkAddress::MaxDepth's
	 *                                   own type, to avoid pulling that struct's full definition into
	 *                                   this otherwise engine/address-agnostic header.
	 */
	static bool ShouldSplit(
		const FVector& CornerA,
		const FVector& CornerB,
		const FVector& CornerC,
		const FVector& ViewerWorldPosition,
		const FSolarOrbzChunkLODSettings& Settings,
		int32 CurrentDepth = INDEX_NONE,
		int32 EffectiveMaxDepth = MAX_int32);

	/**
	 * True if a chunk whose 4 children are CURRENTLY resident should have them collapsed back,
	 * making this chunk the resident leaf again instead. Uses Settings.MergeScreenSizeRatio (the
	 * lower of the two thresholds) rather than SplitScreenSizeRatio - see this file's "Stability /
	 * hysteresis" comment for why these need to be two different functions/thresholds, not one
	 * function compared against the same number both ways. Parameters otherwise identical to
	 * ShouldSplit - pass THIS chunk's (the parent's, not a child's) own 3 corners. No CurrentDepth
	 * parameter: merging back towards the root is never bounded by MaxDepth the way splitting deeper
	 * is, so there is nothing for it to enforce here.
	 */
	static bool ShouldMerge(
		const FVector& CornerA,
		const FVector& CornerB,
		const FVector& CornerC,
		const FVector& ViewerWorldPosition,
		const FSolarOrbzChunkLODSettings& Settings);

	/**
	 * The raw (longest edge / distance-to-chunk) ratio ShouldSplit/ShouldMerge both compare against
	 * their respective thresholds - exposed directly (rather than kept private) so a future debug
	 * overlay/HUD can show the actual number driving a visible LOD decision instead of just the
	 * boolean, and so this can be sanity-checked/logged independent of whichever threshold is
	 * currently configured.
	 */
	static double ComputeScreenSizeRatio(
		const FVector& CornerA,
		const FVector& CornerB,
		const FVector& CornerC,
		const FVector& ViewerWorldPosition);

	/**
	 * Converts a ground-resolution TARGET (FSolarOrbzChunkLODSettings::TargetQuadEdgeMeters, already
	 * x100'd by the caller into the same centimeter convention as Radius - see
	 * AASolarOrbzChunkedPlanetActor::ConstructResidentSetManager's own RadiusCm conversion) into a
	 * quadtree depth CEILING: the depth beyond which a chunk's internal ChunkResolution subdivisions
	 * would already be finer than TargetQuadEdgeMetersCm, making further splitting purely wasted
	 * chunk/triangle count for no additional visible detail. Derivation: the base icosahedron's own
	 * edge length at Radius is `Radius * BaseEdgeAngleRadians` (BaseEdgeAngleRadians = the angle
	 * between two adjacent base vertices, computed from FSolarOrbzIcoSphereGenerator::
	 * GetBaseIcosahedron()'s own vertex data rather than a hand-typed trig constant - this plugin's
	 * established convention). Each depth level halves a chunk's edge length (4-way subdivision), so a
	 * chunk at Depth D has edge length `Radius * BaseEdgeAngleRadians / 2^D`, and its own internal quad
	 * edge (after ChunkResolution subdivisions) is that divided by ChunkResolution again. Solving
	 * `Radius * BaseEdgeAngleRadians / (2^D * ChunkResolution) <= TargetQuadEdgeMetersCm` for the
	 * smallest such D gives `D >= log2(Radius * BaseEdgeAngleRadians / (ChunkResolution *
	 * TargetQuadEdgeMetersCm))`, rounded up and clamped to [0, FSolarOrbzChunkAddress::MaxDepth].
	 *
	 * @param Radius                  Planet radius, in the SAME unit as the caller's other chunk-system
	 *                                 radii (this codebase's convention is centimeters - see
	 *                                 AASolarOrbzChunkedPlanetActor::ConstructResidentSetManager).
	 * @param ChunkResolution          Subdivisions per chunk edge (AASolarOrbzChunkedPlanetActor::
	 *                                 ChunkResolution) - floored at 1 for safety.
	 * @param TargetQuadEdgeMetersCm   FSolarOrbzChunkLODSettings::TargetQuadEdgeMeters, already
	 *                                 converted to the same unit as Radius by the caller (x100 for the
	 *                                 meters-to-centimeters convention this chunk system uses) - floored
	 *                                 at KINDA_SMALL_NUMBER for safety.
	 */
	static int32 ComputeMaxUsefulDepth(double Radius, int32 ChunkResolution, double TargetQuadEdgeMetersCm);

private:
	/** The chunk's longest edge - see this file's header comment for why longest rather than shortest/average. */
	static double ComputeLongestEdgeLength(const FVector& CornerA, const FVector& CornerB, const FVector& CornerC);

	/**
	 * min(distance to CornerA, to CornerB, to CornerC, to the centroid re-projected onto the sphere
	 * implied by the 3 corners' own average distance from the origin) - see this file's header
	 * comment for why all 4 candidate points are used rather than just one.
	 *
	 * The guarantee this does and doesn't make: CornerA/B/C are exact points on the chunk's actual
	 * surface, so distance-to-nearest-corner ALONE is already a true upper bound on the real closest-
	 * surface distance (the true closest point can only be that close or closer, never farther) - a
	 * clean, unconditional bound with no caveats. The centroid sample does NOT carry that same
	 * guarantee: it's only a cheap "near the middle" approximation (a flat average of the 3 corners,
	 * re-projected onto the sphere), not a point proven to sit exactly on the chunk's surface, so in a
	 * sufficiently distorted/oversized triangle it could in principle land slightly closer to the
	 * viewer than the true surface does - undercutting, rather than only ever tightening, the corner-
	 * only bound. Not a practical concern at the triangle sizes this actually runs at in the quadtree
	 * (Depth 2+, once a chunk is no longer a meaningful fraction of the whole sphere - see the header
	 * comment's concrete depth-by-depth edge lengths), but called out here rather than overclaimed.
	 *
	 * Net effect either way: an over-estimated distance makes the ratio smaller than reality, which
	 * only ever makes this function LESS eager to split, never more - missing a split that was
	 * technically warranted is a small, bounded popping/under-detail artifact, the safer direction for
	 * a first-pass heuristic to be wrong in versus the reverse (splitting far more than needed because
	 * distance was under-estimated, wasting generation budget on chunks that didn't need it).
	 */
	static double ComputeNearestDistance(const FVector& CornerA, const FVector& CornerB, const FVector& CornerC, const FVector& ViewerWorldPosition);
};
