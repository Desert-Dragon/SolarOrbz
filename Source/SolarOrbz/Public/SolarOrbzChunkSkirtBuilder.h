// SolarOrbzChunkSkirtBuilder - checklist item 6 of Docs/ChunkedPlanetTerrain.md's "Phase 1,
// continued" section: "extend each chunk's boundary vertices inward by a fixed amount so any gap at
// a differing-LOD edge is hidden by a near-vertical wall instead of showing through." With the
// neighbor-depth restriction (item 4) capping how much adjacent chunks' resolutions can differ, the
// remaining crack at a differing-LOD boundary is small and bounded - skirts are the standard cheap
// fix used broadly in terrain engines for exactly this case. Real vertex-welding (matching densities
// across the edge) is more correct and explicitly deferred - see the design doc.
//
// New file, deliberately kept separate from SolarOrbzIcoSphereChunk.h/.cpp - this is a pure,
// ADDITIVE post-process on an already-generated chunk's FSolarOrbzIcoSphereMeshData
// (FSolarOrbzIcoSphereChunkGenerator::GenerateChunk's own output), not a change to GenerateChunk
// itself. It needs no chunk address, no corners, no Radius - only the already-built mesh data plus
// the Resolution it was generated at (to re-derive which vertices sit on the boundary - see below).
//
// ================================================================================================
// Why this needs Resolution, and the exact indexing it re-derives from GenerateChunk.
// ================================================================================================
// GenerateChunk (SolarOrbzIcoSphereChunk.cpp) lays out a triangular grid of
// (Resolution+1)(Resolution+2)/2 vertices, indexed PointIndex(I, J) = I*(I+1)/2 + J for I in
// 0..Resolution, J in 0..I, with barycentric weights WeightA=(Resolution-I)/Resolution,
// WeightB=(I-J)/Resolution, WeightC=J/Resolution. That means:
//   - WeightC == 0 (J == 0) is the AB edge: PointIndex(I, 0) for I = 0..Resolution, A (I=0) -> B (I=Resolution).
//   - WeightA == 0 (I == Resolution) is the BC edge: PointIndex(Resolution, J) for J = 0..Resolution, B (J=0) -> C (J=Resolution).
//   - WeightB == 0 (J == I) is the CA edge: PointIndex(I, I) for I = Resolution..0 (descending), C (I=Resolution) -> A (I=0).
// This is re-derived here (not re-exported from GenerateChunk) specifically so this file stays a
// pure post-process that doesn't need GenerateChunk to expose anything new - Resolution is the only
// extra input required, and the formula itself is a straightforward transcription, independently
// re-verified (see below) before being trusted.
//
// There is no way to recover Resolution purely from InOutMeshData.Vertices.Num() without inverting
// the (R+1)(R+2)/2 formula anyway, so this function just takes Resolution directly from the caller
// (who already has it - it's the same value passed to GenerateChunk) rather than re-deriving it.
//
// ================================================================================================
// The algorithm.
// ================================================================================================
// 1. Build the 3 ordered boundary vertex lists above.
// 2. The 3 corners (A, B, C) each appear in TWO of the three lists (A ends CA and starts AB; B ends
//    AB and starts BC; C ends BC and starts CA) - each ORIGINAL vertex index gets exactly ONE new
//    skirt counterpart, found-or-created via a TMap<int32,int32> cache keyed by original index, so a
//    shared corner reuses the same skirt vertex across both edges that touch it rather than creating
//    two separate ones (which would leave a gap/seam exactly at the corner - the opposite of what
//    this feature exists to prevent).
// 3. For each unique boundary vertex, append one new "skirt vertex": position =
//    OriginalVertex.Position - OriginalVertex.Normal * SkirtDepth. Pulled inward along the vertex's
//    own FINAL normal (GenerateChunk recomputes real area-weighted smooth normals as its last step
//    before returning - see that function's own comment - so by the time this runs, Normals already
//    holds the right per-vertex reference direction), not the radial/from-planet-center direction -
//    the normal is the better local "down" once terrain displacement means a vertex isn't exactly on
//    the ideal sphere any more, and it degrades to the radial direction exactly on an undisplaced
//    patch where they coincide anyway. UV and Tangent are copied unchanged from the original vertex
//    onto its skirt counterpart - texturing continuity along a thin, meant-to-be-hidden skirt wall
//    isn't a real concern, but there's no reason to leave them zero/uninitialized either.
// 4. For each of the 3 ordered boundary lists, for every consecutive PAIR of original boundary
//    vertices (Resolution such pairs per edge, 3*Resolution total), append a quad (2 triangles)
//    connecting (OriginalA, OriginalB, SkirtB, SkirtA) - a thin strip wall between each original
//    boundary segment and its skirt counterpart directly "below" it.
// 5. Everything is purely ADDITIVE - every original Vertices/Normals/Tangents/UVs/Triangles entry is
//    left byte-for-byte unchanged; only new entries are appended after them.
//
// Winding of the new skirt triangles: picked for consistency with itself, but - exactly like
// GenerateChunk's own triangles (see that function's own comment: "Winding UNVERIFIED without a
// renderer") - NOT independently confirmed correct without an actual renderer. This file claims
// exactly the same level of confidence GenerateChunk already claims for its own triangles, no more.
//
// ================================================================================================
// Verification methodology - mostly the strict bar, since this is really an indexing/array-
// manipulation algorithm wearing mesh-generation clothing, not a rendering question.
// ================================================================================================
// A standalone Python reimplementation (this file's exact algorithm: PointIndex formula, the 3
// ordered boundary lists, the lazy-create-or-reuse skirt-vertex cache, the per-segment quad
// triangulation) was checked across Resolution = 1, 2, 4, 8, 16, 32 and multiple SkirtDepth values
// (including 0.0, the degenerate "skirt vertex coincides exactly with the original" case), against 6
// properties, ALL passing with zero failures at every Resolution/depth combination tried:
//   1. Additivity - original Vertices/Normals/Tangents/UVs/Triangles arrays unchanged in their
//      original index range after the call.
//   2. Boundary-vertex count - exactly 3*Resolution unique boundary vertices (3 edges of
//      Resolution+1 points each, minus the 3 corners double-counted once each).
//   3. Triangle count - exactly 6*Resolution new triangles (2 per segment, Resolution segments per
//      edge, 3 edges), confirming no double-counting at the shared corners.
//   4. Skirt depth correctness - every new skirt vertex sits exactly SkirtDepth away from its
//      original counterpart along that vertex's own normal, for every SkirtDepth tried.
//   5. No duplicate/orphan skirt vertices - the set of original indices that got a skirt counterpart
//      exactly matches an INDEPENDENTLY re-derived boundary set (built by directly scanning every
//      (I,J) pair for J==0 or I==Resolution or J==I, not reusing the ordered-list-building code being
//      tested) - confirms the dedup cache neither missed a boundary vertex nor created a duplicate.
//   6. Triangle index validity - every new triangle references only valid (in-range) vertex indices
//      and no triangle has a repeated index (degenerate/zero-area).
// The C++ below is a direct, faithful transcription of that verified Python. Same standing caveat as
// the rest of this plugin: the ALGORITHM (indexing, counts, dedup, additivity) was checked
// rigorously outside the engine; this specific C++ transcription has not been compiled or run (no
// UE5.8 compiler available in this environment). Unlike GenerateChunk's own triangle winding (and
// this file's own new skirt-triangle winding), the properties checked above do NOT depend on
// rendering to confirm - they are exact, checkable array/indexing invariants, so this gets the same
// strict bar as the rest of this subsystem's pure pieces, with the one explicit exception of winding
// correctness noted above.
//
// ================================================================================================
// Known limitations / out of scope.
// ================================================================================================
// - No real vertex-stitching - this hides a seam, it does not close it. See the design doc.
// - No antimeridian handling - inherited from GenerateChunk itself, not worsened here (this file
//   never touches UV values beyond copying them unchanged onto new skirt vertices).
// - SkirtDepth is a flat, caller-chosen constant - no distance/depth-mismatch-aware sizing (e.g.
//   sizing the skirt to the specific depth difference at a given edge) - Phase 1 keeps this simple;
//   see the design doc's own framing of this item.

#pragma once

#include "CoreMinimal.h"

struct FSolarOrbzIcoSphereMeshData;

/**
 * Appends skirt geometry (a thin inward-facing wall along each of a chunk's 3 boundary edges) to an
 * already-generated chunk's mesh data - see this header's own top-of-file comment for the full
 * algorithm, the exact vertex-indexing scheme it re-derives from GenerateChunk, and verification.
 */
class SOLARORBZ_API FSolarOrbzChunkSkirtBuilder
{
public:
	/**
	 * @param Resolution      The SAME Resolution value FSolarOrbzIcoSphereChunkGenerator::GenerateChunk
	 *                        was called with to produce InOutMeshData - needed to re-derive which
	 *                        vertices sit on the chunk's 3 boundary edges. Getting this wrong (a value
	 *                        that doesn't match what InOutMeshData was actually generated at) silently
	 *                        produces nonsense indices/skirts, not a detectable error - there is no way
	 *                        for this function to validate it against the mesh data alone.
	 * @param SkirtDepth      How far inward (along each boundary vertex's own normal) the new skirt
	 *                        vertices are pulled. 0.0 is a well-defined degenerate case (skirt vertices
	 *                        coincide exactly with their originals) rather than a special case to avoid.
	 * @param InOutMeshData   On entry: an already-complete chunk mesh (typically straight from
	 *                        GenerateChunk - this expects Normals to already hold the FINAL,
	 *                        recomputed smooth normals GenerateChunk produces as its own last step).
	 *                        On return: unchanged in its original index range, with new skirt vertices
	 *                        and triangles appended after the originals - see this header's own
	 *                        algorithm comment.
	 */
	static void AppendSkirts(int32 Resolution, double SkirtDepth, FSolarOrbzIcoSphereMeshData& InOutMeshData);
};
