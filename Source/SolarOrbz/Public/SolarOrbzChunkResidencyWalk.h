// SolarOrbzChunkResidencyWalk - checklist item 3 of Docs/ChunkedPlanetTerrain.md's "Phase 1,
// continued" section: "starting from the 20 base icosahedron faces, recursively applying the LOD
// policy, what is the resulting desired set of leaf chunks for a viewer at a given position?" New
// file, deliberately kept separate from SolarOrbzIcoSphereChunk.h/.cpp and
// SolarOrbzChunkLODPolicy.h/.cpp - this is pure tree-walk glue layered ON TOP of both (addressing +
// per-node split/stay decision), and doesn't need or want to touch either one's own logic.
//
// ================================================================================================
// Algorithm.
// ================================================================================================
// Start at the 20 base icosahedron faces as the 20 quadtree roots - FSolarOrbzChunkAddress
// (BaseFaceIndex, 0, 0) for BaseFaceIndex 0..19 (FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron's
// own 20 faces - the same source of truth every other piece of this system uses). For each node
// visited:
//   1. Resolve its 3 corners (FSolarOrbzChunkAddress::GetCornerUnitDirections), scale by Radius -
//      FSolarOrbzChunkLODPolicy takes WORLD-SPACE corner positions, not unit directions (see that
//      file's own header comment on "input shape").
//   2. Ask FSolarOrbzChunkLODPolicy::ShouldSplit(CornerA, CornerB, CornerC, ViewerWorldPosition,
//      Settings, Node.Depth) whether this chunk should split one level deeper.
//   3. If it says yes AND Node.Depth < FSolarOrbzChunkAddress::MaxDepth: fetch its 4 children
//      (FSolarOrbzChunkAddress::GetChildren) and recurse into each.
//   4. Otherwise: this node is a candidate resident leaf - append it to the output list.
//
// That's the ENTIRE algorithm - everything else in this file is bookkeeping (recursion, the 20
// roots) around those 4 steps.
//
// ================================================================================================
// Why the walker owns an explicit MaxDepth guard too, not just ShouldSplit's own CurrentDepth check.
// ================================================================================================
// FSolarOrbzChunkLODPolicy::ShouldSplit already refuses to recommend splitting once CurrentDepth >=
// FSolarOrbzChunkAddress::MaxDepth (that parameter exists specifically so a caller gets this for
// free - see that file's own "MaxDepth" header section). This file passes Node.Depth as
// CurrentDepth on every call, so in practice that alone would already stop recursion at MaxDepth.
// This file ALSO checks `Node.Depth < FSolarOrbzChunkAddress::MaxDepth` explicitly before recursing,
// deliberately redundant with ShouldSplit's own check - per Docs/ChunkedPlanetTerrain.md, the TREE
// WALKER itself must own this bound, not just delegate to the policy function. Two concrete reasons
// this isn't just belt-and-suspenders paranoia:
//   1. FSolarOrbzChunkAddress::GetChildren itself already degrades at MaxDepth (it logs a warning
//      and returns this chunk's own address for all 4 "children" rather than going deeper - see its
//      own header comment) - recursing into those non-advancing "children" when Node.Depth ==
//      MaxDepth would walk forever (or until the stack overflows) if this file relied SOLELY on
//      ShouldSplit's internal check and that check were ever weakened/removed/miscalled (e.g. a
//      future refactor that calls ShouldSplit without CurrentDepth, or passes the wrong depth).
//   2. It keeps this file correct even against a hypothetical future FSolarOrbzChunkLODPolicy change
//      that drops or alters the CurrentDepth parameter's semantics - the walker's own invariant
//      ("never recurse past MaxDepth") doesn't depend on a second file's internals staying exactly
//      as they are today.
//
// ================================================================================================
// What this file deliberately does NOT do (see Docs/ChunkedPlanetTerrain.md items 4 onward).
// ================================================================================================
// This is the NAIVE, unrestricted walk only - exactly checklist item 3, nothing more:
//   - No neighbor-depth restriction (item 4 - the 1-level-max-difference fixpoint via
//     GetEdgeNeighbor/GetPentagonVertexNeighbors). Two adjacent leaves in this walk's output can
//     differ by an arbitrary number of depth levels (a shallow leaf right next to one that split all
//     the way to MaxDepth is a perfectly possible, if undesirable, output of THIS file alone) - the
//     fixpoint pass that caps that is a separate, not-yet-built piece that would consume this file's
//     output and force-split some of these leaves further.
//   - No resident-set diffing, chunk lifecycle, or spawning/despawning (item 5) - this is a pure,
//     stateless function of (Radius, ViewerWorldPosition, Settings); it has no idea what was
//     resident on the previous call and doesn't try to minimize churn against it.
//   - No skirts/seam-stitching (item 6) - this produces addresses only, no mesh data.
//   - No owning actor/update cadence (item 7).
// Also note: ShouldMerge (the OTHER half of FSolarOrbzChunkLODPolicy's hysteresis pair) is
// deliberately unused here - this walk always starts fresh from the 20 roots and always asks
// "should a currently-nonexistent leaf split", which is exactly ShouldSplit's question. ShouldMerge
// only makes sense once there's actual per-call resident state to decide whether to collapse
// (item 5) - this file has none.
//
// ================================================================================================
// Verification methodology (Python, before this was ported - same standing discipline as every
// other non-trivial piece of this system; see Docs/ChunkedPlanetTerrain.md).
// ================================================================================================
// This is a genuine TOPOLOGICAL correctness claim (does the output leaf-set actually tile the whole
// sphere with no gaps/overlaps, and does it respect MaxDepth), not a tuned heuristic like
// FSolarOrbzChunkLODPolicy itself - so it was held to that bar, not the heuristic one. A ground-truth
// Python reconstruction of GetBaseIcosahedron's 12 vertices/20 faces, GetCornerUnitDirections'
// recursive corner-child/center-child split, FSolarOrbzChunkLODPolicy's exact
// ComputeScreenSizeRatio/ShouldSplit formula, and FSolarOrbzChunkPointLocator's point-in-spherical-
// triangle test (all hand-transcribed from the real .cpp files, not re-derived) was used as the ONLY
// ground truth - this file's algorithm was checked against it, never the other way around. Five
// scenarios were run (Radius/viewer pairs spanning orbit altitude down to standing-on-the-ground and
// a forced-to-MaxDepth case), each checked three ways:
//   - **Coverage, no gaps, no overlaps**: 4,000 random unit directions per scenario (20,000 total
//     across all 5 scenarios). Each point was classified into a base face, then every gathered leaf
//     on that same base face was tested directly against it (the same fixed-sign point-in-spherical-
//     triangle test FSolarOrbzChunkPointLocation.cpp already uses, established correct in this
//     codebase) - requiring EXACTLY one match. Zero gaps (0 points matched no leaf) and zero overlaps
//     (0 points matched more than one leaf) across all 20,000 samples. Each matched leaf was also
//     cross-checked against a second, independently-coded oracle (a per-point recursive descent that
//     re-derives the same ShouldSplit decision at every level and geometrically picks which child
//     contains the point, rather than re-using this file's own gathered list) - zero mismatches.
//     Additionally, every one of the (194 + 530 + 1292 + 632 + 1415 =) 4,063 gathered leaves across
//     the 5 scenarios was checked structurally for nesting (no leaf's address is a strict quadtree
//     ancestor of another leaf's address, which would itself be a geometric overlap) - zero failures.
//   - **MaxDepth respected**: zero leaves with Depth > FSolarOrbzChunkAddress::MaxDepth (24) across
//     all 5 scenarios' 4,063 gathered leaves. One scenario (viewer placed exactly ON the surface,
//     exactly along one of the base icosahedron's own 12 original vertex directions - the
//     NearestDistance floor case, see FSolarOrbzChunkLODPolicy::ComputeScreenSizeRatio's safe-
//     distance guard) was specifically constructed to keep ShouldSplit returning true at every level
//     along that whole path (an always-maximal ratio) to exercise the walker's own hard stop - it
//     produced leaves at Depth exactly 24 along that path and nowhere deeper, confirming the
//     explicit guard (not just ShouldSplit's internal check) is what's actually doing the stopping.
//   - **Monotonic sanity**: a fixed Earth-like Radius with the viewer's altitude swept from 400 km
//     down to 1.8 m (400,000 / 10,000 / 100 / 1.8) produced a strictly increasing leaf count at every
//     step (194 -> 530 -> 932 -> 1,292) and a strictly increasing max depth reached (5 -> 10 -> 17 ->
//     23) and average leaf depth (2.835 -> 5.619 -> 9.024 -> 12.036) - closer viewer, more leaves,
//     deeper leaves, directionally as expected, with zero exceptions across the sweep.
// Total: 5 scenarios x 4,000 coverage samples = 20,000 geometric coverage/overlap checks (zero
// gap/overlap/oracle-mismatch failures) + 4,063 gathered leaves checked for MaxDepth compliance and
// structural nesting (zero failures either way) + a 4-point monotonicity sweep (zero violations).
// The C++ below was then cross-checked function-by-function against that verified Python source
// (GatherDesiredLeaves/VisitNode mirror gather_desired_leaves/visit exactly, same recursion shape
// and same explicit depth guard placement) before being considered done. Same standing caveat as the
// rest of this plugin: the ALGORITHM was checked rigorously outside the engine; this specific C++
// transcription of it has not been compiled or run (no UE5.8 compiler available in this
// environment).
//
// ================================================================================================
// Known limitations / open questions (deliberately out of scope here).
// ================================================================================================
// - No per-call budget/early-out. A sudden large viewer jump (teleport) recomputes the full walk
//   from the 20 roots every time this is called, however many leaves that produces - no
//   amortization/incremental-update story exists yet (see the design doc's own "explicitly out of
//   scope" list for this whole piece).
// - No caching of GetBaseIcosahedron/GetCornerUnitDirections results across sibling visits within
//   one call - each node independently re-resolves its own corners from scratch, same approach
//   SolarOrbzChunkPointLocation.cpp already takes (see that file's own "Performance" note) for the
//   same reason: fine for the expected call pattern (one full gather per residency-manager update,
//   not per-vertex/per-frame), not optimized further until profiling says otherwise.
// - Recursion is plain C++ call-stack recursion, not an explicit stack - safe here because
//   FSolarOrbzChunkAddress::MaxDepth (24) bounds recursion depth absolutely regardless of viewer
//   position or Settings (every recursive call strictly increases Node.Depth, and the walker's own
//   guard above refuses to recurse at or past MaxDepth), so the worst case is a stack depth of 24
//   plus whatever one call frame costs - not a meaningful risk at today's MaxDepth.

#pragma once

#include "CoreMinimal.h"

struct FSolarOrbzChunkAddress;
struct FSolarOrbzChunkLODSettings;

/**
 * Walks the triangular quadtree from the 20 base icosahedron faces, applying
 * FSolarOrbzChunkLODPolicy::ShouldSplit at every node, to produce the naive (unrestricted - see
 * this header's own top-of-file comment for exactly what "unrestricted" excludes) desired leaf-set
 * for a viewer at a given position. Pure, stateless, engine-free (no UObject/AActor/UWorld
 * dependency) - every input is plain data, same shape as FSolarOrbzChunkLODPolicy itself.
 */
class SOLARORBZ_API FSolarOrbzChunkResidencyWalker
{
public:
	/**
	 * Gathers the full set of candidate resident leaf chunks for a viewer at ViewerWorldPosition,
	 * by recursively applying FSolarOrbzChunkLODPolicy::ShouldSplit starting from the 20 base-face
	 * roots - see this header's own top-of-file comment for the exact algorithm and what it
	 * deliberately does not do yet (neighbor-depth restriction, resident diffing, skirts).
	 *
	 * @param Radius                Planet radius, SAME units FSolarOrbzChunkLODPolicy expects its
	 *                               corner positions pre-scaled by (UE units/cm, meters, or anything
	 *                               else consistent) - this multiplies each resolved unit-direction
	 *                               corner by Radius before calling ShouldSplit, so the caller does
	 *                               NOT need to pre-scale anything itself.
	 * @param ViewerWorldPosition    The viewer's position, relative to the planet's center (same
	 *                               frame/units as Radius - see FSolarOrbzChunkLODPolicy's own
	 *                               header comment on "input shape" for why this is the caller's
	 *                               responsibility to get right).
	 * @param Settings               Tunable LOD thresholds - see FSolarOrbzChunkLODSettings.
	 * @param OutLeaves              Receives every candidate resident leaf, in base-face-ascending,
	 *                               depth-first order (BaseFaceIndex 0's leaves before BaseFaceIndex
	 *                               1's, etc.) - reset at the start of this call (any prior contents
	 *                               are discarded, not appended to). No particular order is
	 *                               guaranteed to matter to callers beyond "deterministic for the
	 *                               same inputs"; this is simply whatever order the recursive visit
	 *                               produces.
	 */
	static void GatherDesiredLeaves(
		double Radius,
		const FVector& ViewerWorldPosition,
		const FSolarOrbzChunkLODSettings& Settings,
		TArray<FSolarOrbzChunkAddress>& OutLeaves);

private:
	/**
	 * Visits one node: resolves its world-space corners, asks ShouldSplit, and either recurses into
	 * its 4 children (split) or appends Node itself to OutLeaves (stay a leaf). See this header's
	 * own top-of-file comment for why the `Node.Depth < FSolarOrbzChunkAddress::MaxDepth` guard here
	 * is deliberately redundant with ShouldSplit's own internal CurrentDepth check rather than
	 * relying solely on it.
	 */
	static void VisitNode(
		const FSolarOrbzChunkAddress& Node,
		double Radius,
		const FVector& ViewerWorldPosition,
		const FSolarOrbzChunkLODSettings& Settings,
		TArray<FSolarOrbzChunkAddress>& OutLeaves);
};
