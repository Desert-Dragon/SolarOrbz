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
