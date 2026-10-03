// SolarOrbzChunkRestrictedQuadtree - checklist item 4 of Docs/ChunkedPlanetTerrain.md's "Phase 1,
// continued" section: "cap how much a resident leaf's depth can differ from its same-depth edge-
// neighbors' depth to at most 1 level." Takes FSolarOrbzChunkResidencyWalker::GatherDesiredLeaves'
// naive output (item 3 - a leaf-set with NO constraint on neighboring depths) and force-splits
// whichever leaves are too shallow next to a deep neighbor until the whole set satisfies that
// constraint - the standard "restricted quadtree" invariant universal to real-time terrain LOD
// (CDLOD, geometry clipmaps, etc.), and the reason `FSolarOrbzChunkAddress::GetEdgeNeighbor` was
// built before any of this streaming-manager work started (see that file's own header comment).
// New file, deliberately kept separate from SolarOrbzChunkResidencyWalk.h/.cpp - this is a pass
// layered ON TOP of the naive walk's output, consuming only its already-public leaf-list shape
// (TArray<FSolarOrbzChunkAddress>), not touching that walk's own recursion/LOD-policy logic at all.
//
// ================================================================================================
// The algorithm (the standard restricted-quadtree fixpoint - not redesigned here, just applied).
// ================================================================================================
// 1. Copy the input leaves into a TSet<FSolarOrbzChunkAddress> ("the leaf set") - fast membership
//    testing is the whole reason this needs to be a set rather than staying a flat array; this is
//    the thing the rest of the algorithm mutates in place.
// 2. FindCoveringLeafDepth(Address, LeafSet, OutDepth): walk upward from Address through
//    GetParent() (bounded by MaxDepth+1 steps, same bound GetEdgeNeighbor's own ascend loop would
//    need), checking at each level (Address itself first, i.e. step 0) whether that ancestor is IN
//    the leaf set. Returns true and that ancestor's depth on the first hit. If the walk reaches
//    Depth 0 and still finds no match, returns false - meaning the region that actually covers
//    Address is a DESCENDANT of Address (finer/deeper than Address's own depth), which needs no
//    fix from this pass (a neighbor that's already deeper than the chunk asking about it is never
//    the "too shallow" case this pass exists to correct).
// 3. Seed a worklist with every leaf in the initial set. While it isn't empty: pop one leaf L, and
//    for each of its 3 edges (ESolarOrbzChunkEdge::AB/BC/CA - deliberately NOT the pentagon-vertex
//    corner case, see the dedicated section below for why): compute L.GetEdgeNeighbor(Edge) (same
//    depth as L, by construction - see that function's own guarantee) and call
//    FindCoveringLeafDepth on it against the CURRENT (possibly already-mutated-this-pass) leaf set.
//      - Not found (false): the neighbor region is already finer than L - nothing to do for this
//        edge; L's own depth can never be "too deep" relative to a neighbor, only "too shallow".
//      - Found, OutDepth >= L.Depth - 1: within the 1-level restriction already - nothing to do.
//      - Found, OutDepth < L.Depth - 1: the covering ancestor A (at OutDepth) is too shallow
//        relative to L. Remove A from the leaf set, add its 4 children (now at OutDepth+1) to the
//        leaf set, and push those 4 children AND L itself back onto the worklist. Re-queuing L
//        matters for two reasons: one split might not be enough (if OutDepth+1 is still
//        < L.Depth - 1, A's new children need splitting again themselves, which the worklist
//        entries for them will discover on their own next pop) and because splitting A may have
//        changed what L's OTHER edges see too (checked when L comes back off the worklist).
// 4. Repeat until the worklist drains.
//
// ================================================================================================
// Why this terminates.
// ================================================================================================
// Every split strictly increases the depth of the region being split (A -> A's children, one level
// deeper) - it can never decrease a depth or re-split an already-fine region into something coarser.
// FSolarOrbzChunkAddress::MaxDepth (24) bounds how deep ANY address can ever go, so no single
// region can be split more than (MaxDepth - its own starting depth) times, and the total amount of
// "depth debt" the naive input can ever contain is itself bounded by MaxDepth (the worst possible
// per-edge mismatch is MaxDepth itself, e.g. one leaf at Depth 0 beside one at Depth 24). Each pass
// over a leaf either finds nothing to fix (and is simply dropped from the worklist) or performs a
// split that permanently reduces that bounded total debt by at least one level somewhere in the
// tree - so the worklist cannot refill forever. This is the standard, well-known termination
// argument for this exact algorithm (used identically by CDLOD-style restricted quadtrees/octrees
// elsewhere) - see the "Verification" section below for the concrete split-count/pop-count numbers
// observed on this project's own adversarial test inputs, confirming the bound holds in practice
// here too, not just in the abstract argument.
//
// ================================================================================================
// The pentagon-vertex (5-valent) question - a decision made and documented, not silently skipped.
// ================================================================================================
// FSolarOrbzChunkAddress::GetEdgeNeighbor only resolves the 3 ordinary EDGE-adjacent neighbors. A
// chunk anchored at one of the 12 original icosahedron vertices (IsAnchoredAtOriginalVertex) has a
// 4th kind of adjacency at that one corner point: GetPentagonVertexNeighbors resolves the 4 OTHER
// same-depth chunks (from the 4 other base faces touching that vertex) that meet it there, not
// across a shared edge but at a single shared point where 5 wedges converge instead of the usual 6
// (or 4-at-a-corner-plus-center, for an internal quadtree corner). The design doc (Docs/
// ChunkedPlanetTerrain.md, "Phase 1, continued" item 4) explicitly leaves whether to enforce this
// as an open decision for implementation time. Decision made here: ENFORCE EDGE-NEIGHBOR
// RESTRICTION ONLY (ESolarOrbzChunkEdge::AB/BC/CA via GetEdgeNeighbor); do NOT separately walk
// GetPentagonVertexNeighbors or force-split based on depth mismatches found only there. Reasoning:
//   1. A shared EDGE at mismatched depth is what actually produces a T-junction crack - a whole
//      line of unmatched vertices along the boundary, the dominant and most visually obvious seam
//      artifact in any LOD terrain system, and exactly the case this restriction (and the skirts
//      item 6 is designed around) targets. A single shared POINT at mismatched depth is a strictly
//      smaller, more localized artifact - one vertex position disagreeing among up to 5 wedges,
//      not a whole crack line - and skirts (which extend every chunk's own boundary vertices
//      inward/downward, independent of what a neighbor's actual density is) hide a point
//      discontinuity at least as well as they hide an edge one; there's no reason to expect the
//      pentagon-point case to defeat skirts when the edge case (already explicitly a harder,
//      longer-crack problem) does not.
//   2. Enforcing edge-neighbor restriction ALONE already substantially narrows the pentagon-vertex
//      case too, transitively: each of the (up to 5) wedge chunks meeting at a pentagon vertex is
//      edge-adjacent to its two immediate neighbors in the fan, so the same 1-level restriction
//      chains around the point, just without a single pass that looks at all 5 simultaneously and
//      without a provable tight bound AT the point itself (5 wedges, each independently within 1
//      level of its 2 edge-neighbors in the fan, can in principle still differ by more than 1
//      level from a wedge on the "far side" of the fan - the chain isn't transitively 1-level
//      tight the way a direct check would be). Measured directly (see "Verification" below) on
//      every adversarial test case this task constructed, including a deliberately extreme 8-level
//      mismatch imposed right up to a pentagon vertex: the worst same-point depth spread observed
//      after edge-only restriction was consistently just 2 levels, never more, across every
//      pentagon-anchored leaf examined - i.e. edge restriction alone already keeps the pentagon
//      case to "off by one extra level beyond the edge guarantee," not "unbounded," even without a
//      dedicated pass for it.
//   3. Implementation cost/complexity: a correct corner-fan restriction pass would need to collect
//      all (up to) 5 wedges around a point simultaneously (GetPentagonVertexNeighbors only returns
//      the OTHER 4 for a chunk that IS anchored there - finding "the leaf that currently covers
//      that same point on each of those 4 other faces" is an extra FindCoveringLeafDepth-style
//      walk per neighbor, per pentagon-anchored leaf, every worklist pass) for comparatively little
//      additional crack-hiding benefit over what skirts already need to handle regardless (per
//      point 1) - not worth adding to Phase 1 given point 2's measured result.
// This is a documented engineering call, not an oversight - revisit it if a later pass (real
// vertex-stitching instead of skirts, item 6's own explicitly-deferred "more correct" alternative)
// ever needs a tighter guarantee at these 12 points specifically.
//
// ================================================================================================
// Verification methodology (Python, before this was ported - same standing discipline as every
// other non-trivial piece of this system; see Docs/ChunkedPlanetTerrain.md).
// ================================================================================================
// This is a topological/combinatorial correctness claim (does the output satisfy the 1-level
// neighbor-depth invariant everywhere, and does it still tile the sphere with no gaps/overlaps
// afterward), held to the STRICT bar (not FSolarOrbzChunkLODPolicy's heuristic bar). A Python
// ground-truth reconstruction of GetBaseIcosahedron, GetCornerUnitDirections' recursive split,
// GetChildren, GetParent, and GetEdgeNeighbor (all hand-transcribed from the real .cpp files, NOT
// re-derived) was built first, then spot-checked against GetCornerUnitDirections itself (the
// established ground truth every other piece of this system verifies against) before being relied
// on: 20,460 exhaustive (face, depth 0-4, path, edge) cases plus 4,200 random-depth-0-20 spot
// checks, all confirming the transcribed GetEdgeNeighbor's result shares exactly 2 of 3 corners
// with the query chunk - zero failures (this re-uses GetEdgeNeighbor's OWN already-established
// correctness, per that function's 327,660+ test-case history in SolarOrbzIcoSphereChunk.cpp; this
// script only needed to confirm its OWN transcription of that already-verified algorithm, not
// re-derive the algorithm's correctness from nothing).
//
// Four adversarial/realistic naive leaf sets were constructed and run through the fixpoint:
//   - **max_mismatch_one_edge**: one base face split uniformly to Depth 6, all 19 other base faces
//     left at Depth 0 - the maximum possible depth mismatch across every one of that face's 3 real
//     shared edges at once. 4,115 naive leaves -> 150 splits -> 4,565 fixed-up leaves.
//   - **checkerboard_irregular**: 5 adjacent base faces (the fan around one original vertex) given
//     a deliberately non-monotonic depth pattern (1, 5, 2, 6, 1), the remaining 15 faces at a 6th,
//     intermediate depth (3) - multiple simultaneous mismatches in different directions at once,
//     not just one clean boundary. 6,104 naive leaves -> 154 splits -> 6,566 fixed-up leaves.
//   - **realistic_viewer**: an actual Python port of GatherDesiredLeaves/ShouldSplit (same formula
//     as FSolarOrbzChunkLODPolicy/FSolarOrbzChunkResidencyWalker, re-derived here for this check)
//     run for a viewer at a real, close-to-the-ground altitude (100 m) directly above one of the
//     12 original icosahedron vertices, Earth-like radius - a genuine, not hand-constructed, input.
//     950 naive leaves (Depth 0-17) -> 5 splits -> 965 fixed-up leaves.
//   - **extreme_mismatch_depth8**: pushed further than the first case - one base face split
//     uniformly to Depth 8 (65,536 leaves on its own), all others at Depth 0 - an 8-level mismatch.
//     65,555 naive leaves -> 588 splits -> 67,319 fixed-up leaves, confirming the split count stays
//     small and bounded (not exploding) even as the input mismatch grows well past the already-
//     extreme 6-level case.
//
// Each of the 4 cases' FIXED-UP output was then checked:
//   1. **Neighbor-depth invariant, exhaustively**: for every leaf in the output, for each of its 3
//      edges, the same-depth edge-neighbor's covering-leaf depth (found by the same ancestor-walk
//      FindCoveringLeafDepth uses) is >= (this leaf's own depth - 1). 238,245 total (leaf, edge)
//      pairs checked across all 4 cases (13,695 + 19,698 + 2,895 + 201,957) - ZERO failures.
//   2. **Tiling still holds (no gaps, no overlaps)**: re-ran the same random-direction point-in-
//      spherical-triangle coverage check the residency-walk work already established, against the
//      FIXED-UP leaf set (not the naive one) - 3,000 samples for the first 3 cases, 300 for the
//      large 67k-leaf case (per-sample cost scales with leaf count on the matched face), 9,300
//      total - ZERO gaps, ZERO overlaps. A structural nesting check (no leaf is a strict quadtree
//      ancestor of another output leaf - itself a geometric overlap) was also run on the 3 smaller
//      cases (16,822,062 + 17,954,616 + 173,970 = 34,950,648 pairs checked) - ZERO violations
//      (skipped on the 67k-leaf case purely for this scratch script's own O(n^2) runtime cost at
//      that size, not because it's expected to differ - the point-sampling check above already
//      independently corroborates that case).
//   3. **Termination/MaxDepth sanity**: every case's fixpoint completed in well under a second
//      (worklist pop counts: 4,865 / 6,874 / 975 / 68,495 - all comfortably finite, no runaway
//      growth even on the most extreme input) and zero output leaves exceeded MaxDepth (24) in any
//      case (max depth seen per case: 6 / 6 / 17 / 8).
//   4. **Pentagon-vertex spread diagnostic** (supporting the design decision above, not a pass/fail
//      gate): for every pentagon-anchored output leaf in all 4 cases, the worst same-point depth
//      spread among the (up to 5) wedges meeting there was measured directly (walking
//      GetPentagonVertexNeighbors' 4 other same-depth addresses, then finding each one's actual
//      covering leaf in the fixed-up set) - worst spread observed: 2, consistently, across every
//      case including the deliberately extreme 8-level-mismatch one.
//
// The C++ below was then cross-checked function-by-function against this verified Python source
// (ApplyNeighborDepthRestriction/FindCoveringLeafDepth mirror apply_neighbor_depth_restriction/
// find_covering_leaf_depth exactly, same worklist shape, same no-dedup FIFO behavior) before being
// considered done. Same standing caveat as the rest of this plugin: the ALGORITHM was checked
// rigorously outside the engine; this specific C++ transcription of it has not been compiled or
// run (no UE5.8 compiler available in this environment).
//
// ================================================================================================
// What this file deliberately does NOT do.
// ================================================================================================
// - No pentagon-vertex/corner-fan restriction - see the dedicated section above for why, and the
//   measured bound (worst spread 2) that result rests on.
// - No resident-set diffing/chunk lifecycle (item 5) - this is a pure function from one
//   TArray<FSolarOrbzChunkAddress> to another (or in-place mutation - see the API comment below),
//   with no idea what was previously resident.
// - No skirts/seam-stitching (item 6) - this produces addresses only, same as the naive walk.
// - This does not re-run FSolarOrbzChunkLODPolicy at all - a force-split leaf's children are added
//   purely to satisfy the depth-restriction invariant, regardless of whether the LOD policy would
//   have wanted that region split on its own merits. This is standard and expected for a restricted
//   quadtree (the restriction can request MORE detail than the raw LOD heuristic alone would have,
//   never less) - see the design doc's own framing of this item for why that trade is accepted.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzIcoSphereChunk.h"

/**
 * Takes a naive, unrestricted leaf-set (FSolarOrbzChunkResidencyWalker::GatherDesiredLeaves' own
 * output - no constraint on how much neighboring leaves' depths can differ) and force-splits
 * whichever leaves are too shallow next to a deep same-depth edge-neighbor until every leaf's 3
 * edge-neighbors are within 1 depth level of its own - see this header's own top-of-file comment
 * for the full algorithm, termination argument, pentagon-vertex decision, and verification.
 */
class SOLARORBZ_API FSolarOrbzChunkRestrictedQuadtree
{
public:
	/**
	 * Mutates InOutLeaves in place: replaces its contents with the depth-restricted fixed-up leaf
	 * set (every leaf's 3 GetEdgeNeighbor results now resolve, via FindCoveringLeafDepth, to a
	 * depth within 1 level of that leaf's own - see this header's algorithm comment). Resulting
	 * order is NOT guaranteed to match the input order or any particular deterministic order beyond
	 * "whatever TSet iteration produces" - callers that need a stable order (e.g. for a diff against
	 * a previously-resident set, item 5) should sort/re-key this output themselves; this function's
	 * own job ends at "produces the correct SET of leaves," same as GatherDesiredLeaves' own output
	 * contract ("no particular order is guaranteed to matter to callers beyond deterministic for the
	 * same inputs").
	 *
	 * In-place mutation (rather than returning a new array) was chosen to mirror the natural "take
	 * the naive walk's output, fix it up" framing from the design doc and this file's own call
	 * site shape (GatherDesiredLeaves already fills a TArray& out-parameter the same way) - a caller
	 * that wants to keep the naive list around separately should copy it first.
	 *
	 * @param InOutLeaves  On entry: the naive leaf-set to restrict (typically straight from
	 *                     FSolarOrbzChunkResidencyWalker::GatherDesiredLeaves). On return: the same
	 *                     set, force-split wherever the 1-level neighbor-depth restriction required
	 *                     it. A set that already satisfies the restriction is returned unchanged
	 *                     (aside from possible reordering - see above).
	 */
	static void ApplyNeighborDepthRestriction(TArray<FSolarOrbzChunkAddress>& InOutLeaves);

	/**
	 * Walks upward from Address through GetParent() (Address itself counts as step 0), checking at
	 * each level whether that ancestor is present in LeafSet, and returns the first (shallowest-
	 * ascent, i.e. nearest to Address) match's own Depth. Returns false if the walk reaches Depth 0
	 * without ever finding a match - which means the region that actually covers Address in LeafSet
	 * is a DESCENDANT of Address (finer/deeper than Address's own Depth), not an ancestor or Address
	 * itself; callers should treat "not found" as "already fine from this function's point of view,"
	 * per this header's own algorithm comment (a neighbor covered by something DEEPER than the
	 * neighbor's own nominal depth is never the "too shallow" case this pass corrects).
	 *
	 * Exposed as a public static (not kept file-private) so a future caller - e.g. a debug overlay,
	 * or item 5's resident-set diffing logic wanting to answer "what currently covers this address"
	 * - can reuse it without reimplementing the same ancestor walk; ApplyNeighborDepthRestriction
	 * itself is simply the first, and so far only, consumer.
	 *
	 * @param Address   The address to find the covering leaf of (typically a same-depth edge-
	 *                  neighbor address from GetEdgeNeighbor, which does not itself have to be
	 *                  present in LeafSet - only one of ITS ancestors, or itself, needs to be).
	 * @param LeafSet   The current working leaf set to search (membership-tested via TSet::Contains,
	 *                  i.e. FSolarOrbzChunkAddress's own operator==/GetTypeHash).
	 * @param OutDepth  On true return, the found covering leaf's own Depth (always <= Address.Depth,
	 *                  since every step of the walk is Address itself or a strict ancestor of it).
	 * @return          True if a covering leaf was found (at Address.Depth or shallower); false if
	 *                  the walk reached Depth 0 without a match (covering leaf is strictly deeper
	 *                  than Address - see above).
	 */
	static bool FindCoveringLeafDepth(
		const FSolarOrbzChunkAddress& Address,
		const TSet<FSolarOrbzChunkAddress>& LeafSet,
		int32& OutDepth);
};
