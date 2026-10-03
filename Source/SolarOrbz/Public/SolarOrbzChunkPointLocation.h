// SolarOrbzChunkPointLocation - "which FSolarOrbzChunkAddress (at a chosen depth) contains this
// unit-sphere direction?" See Docs/ChunkedPlanetTerrain.md's "Phase 1, continued" section, item 1 -
// the streaming/residency manager can't decide anything (which chunks should be resident, what the
// viewer is even standing on) without this. New file, deliberately kept separate from
// SolarOrbzIcoSphereChunk.h/.cpp - this is pure geometric query logic layered ON TOP of
// FSolarOrbzChunkAddress's existing GetChildren()/GetCornerUnitDirections(), and doesn't need or
// want to touch that struct's own addressing logic.
//
// ================================================================================================
// Algorithm: point-in-spherical-triangle test + recursive descent.
// ================================================================================================
// 1. Classify UnitDirection into one of the 20 base icosahedron faces (Depth 0) by testing it
//    against each face's spherical triangle in turn.
// 2. Walk down to TargetDepth: at each level, fetch the current chunk's 4 children
//    (FSolarOrbzChunkAddress::GetChildren), resolve each child's 3 corners
//    (GetCornerUnitDirections), and run the same point-in-spherical-triangle test against each of
//    the 4 - they exactly partition the parent (same guarantee BuildBaseIcosahedron/SubdivideOnce
//    already rely on for the whole-sphere mesh), so exactly one should contain the point. Descend
//    into it and repeat.
//
// ================================================================================================
// The point-in-spherical-triangle test itself, and why no "compare against the opposite corner" is
// needed.
// ================================================================================================
// For a spherical triangle with unit-vector corners A, B, C, the standard same-side test for a
// point P is: for edge AB, check that Dot(Cross(A, B), P) has the same sign as Dot(Cross(A, B), C)
// (i.e. P is on the same side of the AB great-circle as the triangle's own third corner), and
// likewise for BC (against A) and CA (against B).
//
// This project's base icosahedron faces (FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron) are
// "wound CCW viewed from outside" by that function's own comment, and GetCornerUnitDirections
// preserves that same winding at every recursive split (its own header comment: "winding stays
// consistent with the rest of the plugin at every depth"). That means the "same sign as the
// opposite corner" reduces to one FIXED sign for every single triangle this project ever produces
// at any depth: Dot(Cross(A, B), C) is always >= 0 for a CCW-wound, less-than-a-hemisphere
// spherical triangle with outward-pointing winding. Verified empirically (not just assumed) in the
// Python ground-truth script before this was ported - see "Verification" below - by checking every
// one of the 20 base faces' own centroid scores >=0 on all 3 of its own edges using the FIXED sign
// (no opposite-corner comparison at all), with zero exceptions. So PointInSphericalTriangle below
// just checks all three Dot(Cross(...), P) values are >= 0 directly, rather than re-deriving the
// comparison sign per call - simpler, and exactly as correct given this project's one winding
// convention never varies.
//
// ================================================================================================
// Floating-point edge cases at shared boundaries (tie-breaking rule).
// ================================================================================================
// Two adjacent triangles sharing an edge are wound in OPPOSITE vertex order along that edge (true
// for base-face edges - verified directly by SolarOrbzIcoSphereChunk.cpp's own
// GetFaceEdgeCrossingTable checkf - and equally true for a parent's internal corner-child/
// center-child split, by the same construction). A point sitting mathematically exactly on such an
// edge therefore scores exactly 0 against the matching test on BOTH adjacent triangles in EXACT
// arithmetic - a genuine tie, since this test is inclusive (>= 0, not > 0). In floating point,
// rounding in the Cross/Dot chain usually (not always) nudges the computed value to one side or the
// other before it ever reaches exact zero, so most boundary points still resolve unambiguously
// without needing the tie-break rule at all - confirmed directly in the Python verification (see
// below): of 30 real shared base-face edge midpoints tested, only 14 were genuine float-level ties
// (both adjacent faces scoring >= 0); the other 16 happened to resolve to exactly one side purely
// from rounding.
//
// For the genuine-tie case, the rule used throughout (both here and in the base-face classification
// pass) is: **test candidates in a fixed, ascending index order and return the first match** - base
// faces 0..19 in GetBaseIcosahedron's own order, children 0..3 in GetChildren's own corner-child/
// corner-child/corner-child/center-child order. This is an arbitrary choice (nothing about the
// geometry prefers lower indices), but it is deterministic and cheap, which is all a boundary tie
// needs to be - a caller streaming chunks around a point sitting exactly on a shared edge just needs
// a STABLE answer (the same chunk every time it asks), not a "more correct" one; both adjacent
// chunks are equally valid owners of a zero-width boundary. Verified directly: every one of the 30
// shared base-face edges' midpoints, and 500 random internal corner/center-child shared-edge
// midpoints across a range of faces/depths, classify into exactly one face/child (no gap, no
// unresolved double-match reaching the caller), and every genuine float-level tie among them
// resolved to the lower-indexed candidate exactly as this rule predicts.
//
// A direction of exactly FVector::ZeroVector is a degenerate special case worth naming explicitly:
// every Cross(...)/Dot(..., P) term is then identically 0 regardless of which triangle is tested,
// so EVERY candidate "ties" under the inclusive >= 0 test, and the first-match-wins rule
// deterministically resolves it to base face 0 (and from there, child 0 at every subsequent level) -
// harmless and stable, but not a meaningful "location" (a zero vector isn't a direction). Callers
// should treat FindChunkContainingDirection as (by construction) only answering for a genuine
// direction (FVector::GetSafeNormal() with a valid non-zero input) - see
// FindChunkContainingDirection's own comment for the explicit normalize-and-warn handling of a
// degenerate input.
//
// ================================================================================================
// Verification methodology (Python, before any of this was ported - see the task's standing
// discipline, same bar as GetEdgeNeighbor/GetPentagonVertexNeighbors in SolarOrbzIcoSphereChunk.*).
// ================================================================================================
// A ground-truth Python reconstruction of GetBaseIcosahedron's literal 12 vertices/20 faces and
// GetCornerUnitDirections' exact recursive corner-split (hand-transcribed from the real .cpp, not
// re-derived) was used as the ONLY position authority - this file's algorithm was checked against
// it, never the other way around. Checks performed, all zero failures:
//   - Winding-sign check: all 20 base faces' own centroids score >= 0 on all 3 edges with the fixed
//     (no-opposite-corner-comparison) sign - establishes the sign convention empirically rather than
//     assuming it.
//   - Base-face classification alone: all 20 face centroids classify into their own face and no
//     other; 60 points nudged slightly inside each face's 3 corners classify correctly; 200,000
//     random unit directions each classify into exactly one of the 20 faces (0 gaps, 0 double-
//     matches) - confirms the 20 faces are a full, non-overlapping partition of the sphere.
//   - Recursive descent, exhaustive: every (base face, depth, path) for depths 0-5 across all 20
//     base faces (27,300 cases) - a chunk's own renormalized corner centroid (strictly interior,
//     nowhere near its own edges, so this tests the ALGORITHM, not the boundary tie-break) always
//     round-trips back through FindChunkContainingDirection to the exact same chunk. Zero failures.
//   - Recursive descent, random sampling: depths 6-15, 3,000 random (face, path) combinations per
//     depth (30,000 cases) - zero failures. Extended with a smaller spot-check at depths 16-20 (300
//     combos/depth, 1,500 cases) - zero failures.
//   - Boundary tie-break, explicit: all 30 real shared base-face edge midpoints, and 500 random
//     internal corner/center-child shared-edge midpoints (a range of faces/depths 0-8) - confirmed
//     exactly one face/child always wins (no gap, no unresolved ambiguity), and every genuine
//     float-level tie resolves to the documented lower-index rule.
// Total: 20 + 60 + 200,000 + 20 (depth-0 sanity) + 27,300 + 30,000 + 1,500 + 30 + 500 = well over
// 259,000 cases, zero failures. The C++ below was then cross-checked function-by-function against
// that verified Python source (same structure: ClassifyBaseFace mirrors classify_base_face,
// PointInSphericalTriangle mirrors point_in_spherical_triangle, the descent loop in
// FindChunkContainingDirection mirrors find_chunk_containing_direction) before being considered
// done. Same standing caveat as the rest of this plugin: the ALGORITHM was checked rigorously
// outside the engine; this specific C++ transcription of it has not been compiled or run (no UE5.8
// compiler available in this environment).
//
// ================================================================================================
// Known limitations / open questions (deliberately out of scope here, same as the design doc's
// "known out of scope" lists elsewhere).
// ================================================================================================
// - No antimeridian/UV concern here at all - this answers a pure 3D geometric question (which
//   spherical triangle contains this direction), nothing about equirectangular UV, so the whole-
//   sphere mesh's/chunk generator's antimeridian caveat doesn't apply to this piece.
// - No LOD-aware early-stop. This always walks to EXACTLY TargetDepth. The design doc's own
//   description of this item mentions "or until a caller-supplied 'stop here' predicate (the LOD
//   policy below) says this node is fine as a leaf" - that's the NEXT checklist item (LOD policy)
//   and deliberately not built here; a caller that wants LOD-aware descent can trivially wrap this
//   same per-level loop with its own stop predicate once that policy exists, or this function can
//   grow an optional predicate parameter later without changing its existing behavior when the
//   predicate is omitted. Not done now to keep this piece's surface area matched to exactly what
//   the checklist item asks for.
// - Performance: every call to GetCornerUnitDirections re-walks PathBits from the base face down
//   (no caching across levels), and ClassifyBaseFace/FindChunkContainingDirection both re-fetch
//   GetBaseIcosahedron's table on every call. Fine for the expected call pattern (locating a single
//   viewer position per streaming-manager update tick, not per-vertex), not optimized further -
//   revisit only if profiling says this is ever hot.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzIcoSphereChunk.h"

/**
 * Finds which FSolarOrbzChunkAddress (at a chosen depth) contains an arbitrary unit-sphere
 * direction - see this header's own top-of-file comment for the full algorithm, sign-convention
 * derivation, boundary tie-break rule, and verification methodology.
 */
class SOLARORBZ_API FSolarOrbzChunkPointLocator
{
public:
	/**
	 * Finds the FSolarOrbzChunkAddress at Depth == TargetDepth whose spherical triangle contains
	 * UnitDirection.
	 *
	 * @param UnitDirection  The direction to locate. Does not need to already be exactly unit
	 *                       length - this normalizes defensively (FVector::Normalize) the same way
	 *                       other direction-taking functions in this plugin do. A zero-length (or
	 *                       otherwise degenerate) input logs a warning and falls back to
	 *                       FVector::ForwardVector rather than propagating a NaN/zero result - see
	 *                       this header's own comment on the FVector::ZeroVector special case for
	 *                       why that fallback is itself well-defined (deterministically base face 0)
	 *                       even without the substitution, but substituting a real direction avoids
	 *                       returning a technically-correct-but-meaningless answer for a caller that
	 *                       passed bad input by accident.
	 * @param TargetDepth    Desired quadtree depth. Clamped to [0, FSolarOrbzChunkAddress::MaxDepth].
	 * @return The chunk address at TargetDepth containing UnitDirection. Always succeeds (the 20
	 *         base faces, and every subsequent 4-way split, are a full partition of the sphere/
	 *         parent triangle respectively) - no bool return. In the practically-unreachable case
	 *         that base-face classification or a descent step still somehow fails to find a match
	 *         (see this header's verification notes - never observed in 259,000+ test cases), this
	 *         logs an error and returns the shallowest address it had successfully resolved so far
	 *         rather than an uninitialized/undefined result.
	 */
	static FSolarOrbzChunkAddress FindChunkContainingDirection(const FVector& UnitDirection, int32 TargetDepth);
};
