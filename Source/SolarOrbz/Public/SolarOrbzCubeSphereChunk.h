// SolarOrbzCubeSphereChunk - the chunked/streaming planet terrain system's foundational piece: a
// cube-sphere chunk addressing scheme and a per-chunk mesh generator. See
// Docs/ChunkedPlanetTerrain.md for the full design and what's deliberately NOT built yet (neighbor/
// LOD-seam stitching, distortion correction, streaming, baking, ASN_MK1 integration). This is a
// second, parallel mesh generator alongside FSolarOrbzIcoSphereGenerator (SolarOrbzIcoSphere.h) - that
// one is unchanged and keeps serving ASolarOrbzIcoSphereActor's bounded preview/bake use case.
//
// A cube-sphere (not a quadtree over the icosphere's own triangles) specifically because it gives
// uniform quad chunks with exactly 4 same-depth neighbors, vs. the icosphere's 12 permanently 5-valent
// vertices needing special-cased neighbor logic everywhere - see the design doc's "Topology" section.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzIcoSphere.h"

class USolarOrbzTerrainLayerStack;
struct FSolarOrbzClimateGrid;

/** Which of the cube's 6 faces a chunk belongs to, before projection onto the sphere. */
UENUM(BlueprintType)
enum class ESolarOrbzCubeFace : uint8
{
	PlusX,
	MinusX,
	PlusY,
	MinusY,
	PlusZ,
	MinusZ,
};

/**
 * A quadtree node's address on one cube face: Depth 0 is the whole face as a single chunk; each
 * +1 Depth quarters it, so a face at Depth D has (2^D)^2 chunks, (X, Y) each in [0, 2^D). Pure
 * addressing only - no residency/streaming state lives here, see Docs/ChunkedPlanetTerrain.md for
 * where that's planned to go.
 */
struct SOLARORBZ_API FSolarOrbzChunkAddress
{
	ESolarOrbzCubeFace Face = ESolarOrbzCubeFace::PlusZ;
	int32 Depth = 0;
	int32 X = 0;
	int32 Y = 0;

	FSolarOrbzChunkAddress() = default;
	FSolarOrbzChunkAddress(ESolarOrbzCubeFace InFace, int32 InDepth, int32 InX, int32 InY)
		: Face(InFace), Depth(InDepth), X(InX), Y(InY)
	{
	}

	bool operator==(const FSolarOrbzChunkAddress& Other) const
	{
		return Face == Other.Face && Depth == Other.Depth && X == Other.X && Y == Other.Y;
	}

	friend uint32 GetTypeHash(const FSolarOrbzChunkAddress& Address)
	{
		return HashCombine(HashCombine(HashCombine(
			GetTypeHash((uint8)Address.Face), GetTypeHash(Address.Depth)),
			GetTypeHash(Address.X)), GetTypeHash(Address.Y));
	}

	/** This chunk's footprint in face-local (S, T) space, each axis in [-1, 1] - the range FSolarOrbzCubeSphereChunkGenerator::GenerateChunk covers. */
	void GetFaceLocalBounds(double& OutMinS, double& OutMaxS, double& OutMinT, double& OutMaxT) const;

	/** Depth 0's own parent is itself (nothing above the whole-face root) - check Depth before walking further if that matters to the caller. */
	FSolarOrbzChunkAddress GetParent() const;

	/** Writes this chunk's 4 children (one deeper) into OutChildren[0..3], in (X,Y) = (0,0),(1,0),(0,1),(1,1) order relative to this chunk's own footprint. */
	void GetChildren(FSolarOrbzChunkAddress OutChildren[4]) const;
};

/**
 * Generates one cube-sphere chunk's mesh data. Mirrors FSolarOrbzIcoSphereGenerator's shape
 * (unit-direction math, radius applied at finalize time, reuses the same FSolarOrbzIcoSphereMeshData
 * output struct - a grid of displaced vertices is a grid of displaced vertices regardless of which
 * generator produced it) so the two can eventually share bake/collision code.
 */
class SOLARORBZ_API FSolarOrbzCubeSphereChunkGenerator
{
public:
	/**
	 * Maps a face-local (S, T), each in [-1, 1], to a unit-sphere direction. Plain normalize of the
	 * cube point - NOT area-preserving (see Docs/ChunkedPlanetTerrain.md's "known out of scope" list
	 * on the COBE quad-sphere warp this could use instead later). UNVERIFIED without a compiler -
	 * GetFaceBasis's per-face vectors need checking against a real rendered chunk before trusting
	 * seams line up between faces.
	 */
	static FVector FaceLocalToUnitSphereDirection(ESolarOrbzCubeFace Face, double S, double T);

	/**
	 * Generates Address's chunk as a (Resolution+1) x (Resolution+1) vertex grid, projected onto the
	 * sphere and scaled by Radius (UE units/cm, matching FSolarOrbzIcoSphereGenerator's convention),
	 * displaced by TerrainStack if non-null (nullptr = plain undisplaced sphere patch - useful for
	 * testing chunk topology/seams before wiring in real terrain). ClimateGridForMasking is forwarded
	 * to TerrainStack->EvaluateHeight exactly as RunTerrainPassA does in SolarOrbzIcoSphere.cpp - null
	 * is correct unless a climate-gated layer mask is in play.
	 *
	 * No neighbor/LOD-seam stitching, no UV-seam handling for a chunk that straddles the
	 * equirectangular antimeridian - see the design doc's "known out of scope" list.
	 *
	 * @param Resolution  Quads per chunk edge - vertex grid is (Resolution+1)^2. Keep modest (e.g.
	 *                    16-64) for now; this has no subdivision-level safety clamp the way
	 *                    FSolarOrbzIcoSphereGenerator does yet.
	 */
	static void GenerateChunk(
		const FSolarOrbzChunkAddress& Address,
		int32 Resolution,
		double Radius,
		const USolarOrbzTerrainLayerStack* TerrainStack,
		const FSolarOrbzClimateGrid* ClimateGridForMasking,
		FSolarOrbzIcoSphereMeshData& OutMeshData);

private:
	/**
	 * Right/Up/Forward basis for a face: a face-local point (S, T) maps to the cube point
	 * (Right * S + Up * T + Forward), i.e. Forward is the face's own outward normal at its center.
	 * UNVERIFIED (see FaceLocalToUnitSphereDirection's own comment) - the six triples must be chosen
	 * so adjacent faces agree on shared-edge direction, or chunks will show seams/gaps at cube edges.
	 */
	static void GetFaceBasis(ESolarOrbzCubeFace Face, FVector& OutRight, FVector& OutUp, FVector& OutForward);
};
