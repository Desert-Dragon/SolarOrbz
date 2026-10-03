// SolarOrbzIcoSphereChunk - the chunked/streaming planet terrain system's foundational piece: a
// triangular quadtree over the base icosahedron's 20 faces, and a per-chunk mesh generator. See
// Docs/ChunkedPlanetTerrain.md for the full design and what's deliberately NOT built yet (neighbor/
// LOD-seam stitching, streaming, baking, ASN_MK1 integration).
//
// Built directly on top of FSolarOrbzIcoSphereGenerator's own base-icosahedron table and recursive
// midpoint-bisection scheme (SolarOrbzIcoSphere.h/.cpp) - a chunk's address IS a path down the exact
// same "4 children per triangle" subdivision BuildBaseIcosahedron/SubdivideOnce already use for the
// whole-mesh generator, just stopping at a chosen depth instead of applying uniformly everywhere and
// building the whole sphere in one mesh. This is a deliberate pivot from an earlier cube-sphere-based
// version of this same system - see the design doc's revision history - specifically so the chunk
// topology reuses geometry/winding this plugin already relies on elsewhere, rather than introducing a
// second, independently-derived basis (the cube-sphere version needed 6 hand-derived per-face basis
// vector triples, unverified without a renderer; this version needs none of that).

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzIcoSphere.h"

class USolarOrbzTerrainLayerStack;
struct FSolarOrbzClimateGrid;
struct FSolarOrbzPentagonVertexNeighbors; // full definition below FSolarOrbzChunkAddress - see there for why.

/**
 * Names a chunk's 3 edges using GetCornerUnitDirections' own corner order (AB = between OutA and
 * OutB, etc.) - the same naming FSolarOrbzIcoSphereGenerator::SubdivideOnce's child-split comments
 * already use. Ordinal values (0,1,2) are used directly as an edge INDEX in a couple of internal
 * lookup tables - keep that order if this ever needs touching.
 */
UENUM(BlueprintType)
enum class ESolarOrbzChunkEdge : uint8
{
	AB,
	BC,
	CA,
};

/**
 * A node in the triangular quadtree over one of the base icosahedron's 20 faces: Depth 0 is the
 * whole base face as a single chunk; each +1 Depth quarters the current triangle into 4 children,
 * exactly like FSolarOrbzIcoSphereGenerator::SubdivideOnce's own corner-AB-CA / corner-BC-AB /
 * corner-CA-BC / center-AB-BC-CA split (see GetCornerUnitDirections's implementation, which walks
 * this same split). Pure addressing only - no residency/streaming state lives here.
 *
 * PathBits packs child indices (0-3, i.e. 2 bits each) from the base face down to Depth, least-
 * significant pair = level 0 (the child chosen right under BaseFaceIndex). A uint64 holds up to 32
 * levels - MaxDepth below is far more conservative than that ceiling, since each level already ~4x's
 * resolution (level 20 alone is already far finer than any plausible single chunk needs to go).
 */
struct SOLARORBZ_API FSolarOrbzChunkAddress
{
	/** Absolute ceiling on Depth - a uint64 PathBits could hold up to 32, this is deliberately far more conservative (see the struct comment). Not the same kind of constraint as FSolarOrbzIcoSphereGenerator::AbsoluteMaxSubdivisionLevel (that one's about a single mesh's vertex buffer overflowing uint32; this one's purely about PathBits' bit width). */
	static constexpr int32 MaxDepth = 24;

	int32 BaseFaceIndex = 0; // 0..19 - index into FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron's 20 faces
	int32 Depth = 0;         // 0 = the whole base face is one chunk
	uint64 PathBits = 0;

	FSolarOrbzChunkAddress() = default;
	FSolarOrbzChunkAddress(int32 InBaseFaceIndex, int32 InDepth, uint64 InPathBits)
		: BaseFaceIndex(InBaseFaceIndex), Depth(InDepth), PathBits(InPathBits)
	{
	}

	bool operator==(const FSolarOrbzChunkAddress& Other) const
	{
		return BaseFaceIndex == Other.BaseFaceIndex && Depth == Other.Depth && PathBits == Other.PathBits;
	}

	friend uint32 GetTypeHash(const FSolarOrbzChunkAddress& Address)
	{
		return HashCombine(HashCombine(GetTypeHash(Address.BaseFaceIndex), GetTypeHash(Address.Depth)), GetTypeHash(Address.PathBits));
	}

	/** Depth 0's own parent is itself (nothing above the whole-base-face root) - check Depth before walking further if that matters to the caller. */
	FSolarOrbzChunkAddress GetParent() const;

	/** Writes this chunk's 4 children (one deeper) into OutChildren[0..3], same child-index convention as GetCornerUnitDirections' split (0/1/2 = the three corner children, 3 = the center child). No-op (does nothing useful) past MaxDepth - check Depth first. */
	void GetChildren(FSolarOrbzChunkAddress OutChildren[4]) const;

	/**
	 * Resolves this chunk's 3 corner unit-sphere directions by walking PathBits from
	 * BaseFaceIndex's own 3 corners down to Depth - the actual "which part of the sphere is this
	 * chunk" resolution. Mirrors FSolarOrbzIcoSphereGenerator::SubdivideOnce's exact corner-ordering
	 * convention per split (child 0 = (A, AB, CA), 1 = (B, BC, AB), 2 = (C, CA, BC), 3 = (AB, BC,
	 * CA)), so winding stays consistent with the rest of the plugin at every depth.
	 */
	void GetCornerUnitDirections(FVector& OutA, FVector& OutB, FVector& OutC) const;

	/**
	 * True if this chunk's own corner-child path has, at every level, picked the child that keeps one
	 * specific corner pinned to one of the base icosahedron's 12 original vertices (level 0 picks
	 * which original corner - 0/1/2, never 3 - to anchor to; every level after that must be exactly 0
	 * to keep that same point pinned rather than sliding onto a midpoint - see GetCornerUnitDirections'
	 * switch: only child 0 leaves "A" unchanged). See GetPentagonVertexNeighbors for why this matters.
	 * Depth 0 is never "anchored" by this definition (the whole base face touches all 3 of its own
	 * corners at once, not one specific point) - always returns false for Depth 0.
	 * @param OutOriginalVertexIndex  If true is returned, which of the 12 original vertices (0..11,
	 *                                index into FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron's
	 *                                vertex array) this chunk is anchored to. GetCornerUnitDirections'
	 *                                OutA is guaranteed to be exactly that vertex whenever this is true.
	 */
	bool IsAnchoredAtOriginalVertex(int32& OutOriginalVertexIndex) const;

	/**
	 * The 12 original icosahedron vertices are permanently 5-valent (5 base faces meet there, not 6 -
	 * verified directly against FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron's own 20-face table;
	 * see Docs/ChunkedPlanetTerrain.md's "Topology" section) - any neighbor-finding/seam-stitching logic
	 * that assumes 6 same-depth chunks surround every vertex gets this wrong at exactly these 12 points.
	 * Only meaningful when this chunk IsAnchoredAtOriginalVertex (checked internally; returns false and
	 * leaves OutNeighbors untouched otherwise) - resolves the OTHER same-depth chunks, one from each of
	 * the other base faces touching that same vertex, that share it. Always exactly 4 entries when it
	 * succeeds (5 faces touch each original vertex total, minus this chunk's own face).
	 *
	 * Lookup only - nothing yet consumes this to actually stitch geometry (no streaming/seam system
	 * exists at all yet, see the design doc). This is the primitive future seam-stitching at these 12
	 * points will need, built now so the pentagon case isn't left to be discovered/reverse-engineered
	 * later once the general (valence-6) neighbor system already exists and assumes 6 everywhere.
	 */
	bool GetPentagonVertexNeighbors(FSolarOrbzPentagonVertexNeighbors& OutNeighbors) const;

	/**
	 * General same-depth edge-neighbor finding - "the chunk across this edge", for any edge at any
	 * depth, crossing a base-face boundary via the icosahedron's own edge adjacency when needed. This
	 * is the piece GetPentagonVertexNeighbors above explicitly deferred (that one only covers the 12
	 * original vertices; this covers every ordinary edge, including ones that happen to touch a
	 * pentagon vertex at one endpoint - the EDGE itself is still an ordinary 2-chunk boundary even
	 * when one of its endpoints is 5-valent).
	 *
	 * Algorithm: walk up the quadtree from this chunk while Edge keeps landing on a "boundary" side
	 * (shared with the parent, not a sibling) - see SubdivideOnce's child-split comment for which of
	 * each child's 3 edges are internal (shared with a specific sibling) vs boundary (half of a
	 * parent edge). Once the edge resolves to either a direct sibling match or the base-face root
	 * (crossing to the adjacent face via its own matching edge, always reversed-direction - verified
	 * directly against GetBaseIcosahedron's table, not assumed), walk back down picking, at each
	 * level, whichever child sits on the same side of the (possibly now-relabeled) edge.
	 *
	 * Exhaustively verified outside the engine against this struct's own GetCornerUnitDirections (the
	 * authoritative, already-shipped geodesic position function) before being written here: every
	 * (face, depth, path, edge) combination for depths 0-6 across all 20 base faces (327,660+ cases)
	 * confirmed the returned neighbor shares exactly 2 of its 3 corners with the query chunk, plus
	 * randomized spot checks to depth 20 - see Docs/ChunkedPlanetTerrain.md's revision notes. Written
	 * without a UE5.8 compiler available, same standing caveat as everything else in this plugin -
	 * the ALGORITHM itself was checked rigorously; this specific C++ transcription of it was not run.
	 *
	 * @param Edge  Which of this chunk's 3 edges to find the neighbor across.
	 * @param OutNeighbor  The same-depth chunk sharing that edge. Always succeeds (every edge at
	 *                     every depth has exactly one same-depth neighbor in this topology) - no
	 *                     bool return, unlike GetPentagonVertexNeighbors which can legitimately not
	 *                     apply to a given chunk.
	 */
	void GetEdgeNeighbor(ESolarOrbzChunkEdge Edge, FSolarOrbzChunkAddress& OutNeighbor) const;
};

/**
 * Result of FSolarOrbzChunkAddress::GetPentagonVertexNeighbors - see that function's own comment.
 * Declared AFTER FSolarOrbzChunkAddress (not before it, where this used to sit) - TArray<T> needs
 * T's complete definition, not just a forward declaration, and this struct holds one by value. A
 * real compile error (C2065/C2923/C2976/C2955 - "undeclared identifier" cascading into "too few
 * template arguments" for TArray) caught this on the first actual build of this plugin; every
 * verification this subsystem did up to that point was outside the engine (see
 * Docs/ChunkedPlanetTerrain.md), so an ordering mistake like this - invisible to Python, invisible
 * to code review unless you're specifically checking declaration order - was always going to surface
 * here first, not earlier.
 */
struct SOLARORBZ_API FSolarOrbzPentagonVertexNeighbors
{
	/** Which of the 12 original icosahedron vertices this is - index into FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron's vertex array. */
	int32 OriginalVertexIndex = INDEX_NONE;

	/** The other chunks (same Depth as the chunk GetPentagonVertexNeighbors was called on, from the other base faces touching this same vertex) - always exactly 4 when the call succeeds, since 5 faces touch each original vertex, minus this chunk's own. */
	TArray<FSolarOrbzChunkAddress> OtherChunks;
};

/**
 * Generates one icosphere-chunk's mesh data. Mirrors FSolarOrbzIcoSphereGenerator's shape (unit-
 * direction math, radius applied at finalize time, reuses the same FSolarOrbzIcoSphereMeshData output
 * struct) so the two can eventually share bake/collision code.
 */
class SOLARORBZ_API FSolarOrbzIcoSphereChunkGenerator
{
public:
	/**
	 * Generates Address's chunk as a uniform triangular grid (Resolution subdivisions per edge, i.e.
	 * (Resolution+1)(Resolution+2)/2 vertices and Resolution^2 triangles) across its own 3 corners,
	 * projected onto the sphere and scaled by Radius (UE units/cm), displaced by TerrainStack if
	 * non-null (nullptr = plain undisplaced sphere patch - useful for testing chunk topology/seams
	 * before wiring in real terrain). ClimateGridForMasking is forwarded to
	 * TerrainStack->EvaluateHeight exactly as RunTerrainPassA does in SolarOrbzIcoSphere.cpp.
	 *
	 * No neighbor/LOD-seam stitching, no UV-seam handling for a chunk that straddles the
	 * equirectangular antimeridian - see the design doc's "known out of scope" list. Triangulation
	 * winding is UNVERIFIED without a renderer - the same "swap last two corners" empirical fix
	 * FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize documents is applied here too; check a
	 * generated chunk's front/back facing before relying on it.
	 *
	 * @param Resolution  Subdivisions per edge - keep modest (e.g. 16-64) for now; this has no
	 *                    subdivision-level safety clamp the way FSolarOrbzIcoSphereGenerator does yet.
	 */
	static void GenerateChunk(
		const FSolarOrbzChunkAddress& Address,
		int32 Resolution,
		double Radius,
		const USolarOrbzTerrainLayerStack* TerrainStack,
		const FSolarOrbzClimateGrid* ClimateGridForMasking,
		FSolarOrbzIcoSphereMeshData& OutMeshData);
};
