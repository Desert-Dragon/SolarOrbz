// SolarOrbz - Parametric IcoSphere Generator
// Produces a geodesic sphere by subdividing a regular icosahedron, with
// seam-corrected UVs so it's ready for texturing and, later, terrain
// displacement without visible cracks or pinched poles.

#pragma once

#include "CoreMinimal.h"

/**
 * Raw mesh data for a generated icosphere. Feed this into a
 * UProceduralMeshComponent, a MeshDescription-based UStaticMesh build,
 * or any other mesh consumer.
 *
 * NOTE: Because of UV-seam and pole splitting, Vertices.Num() will be
 * somewhat larger than the "geometric" vertex count of the icosphere -
 * this is expected and required for correct texture mapping.
 */
struct SOLARORBZ_API FSolarOrbzIcoSphereMeshData
{
	/** Vertex positions in local space, centered on the origin (UE units / cm). */
	TArray<FVector> Vertices;

	/** Per-vertex outward normals (unit length). */
	TArray<FVector> Normals;

	/** Per-vertex tangents (unit length). Basic longitude-aligned tangent; refine later if needed. */
	TArray<FVector> Tangents;

	/** Per-vertex UV0, equirectangular (lat/long) mapping with seam and pole correction. */
	TArray<FVector2D> UVs;

	/** Triangle index buffer, 3 indices per triangle, CCW winding (outward-facing). */
	TArray<int32> Triangles;

	void Reset()
	{
		Vertices.Reset();
		Normals.Reset();
		Tangents.Reset();
		UVs.Reset();
		Triangles.Reset();
	}
};

/**
 * Parametric icosphere generator.
 *
 * Recursively subdivides a regular icosahedron and re-projects new
 * vertices onto the target sphere. This keeps triangle sizes far more
 * uniform than a UV/lat-long sphere - important once vertices start
 * getting displaced for terrain.
 */
class SOLARORBZ_API FSolarOrbzIcoSphereGenerator
{
public:
	/**
	 * Generate an icosphere sized so the average edge length is close to
	 * (100 / VerticesPerMeter) UE units - i.e. VerticesPerMeter is verts
	 * per meter of surface, since UE units are centimeters.
	 *
	 * @param Radius            Sphere radius, in UE units (cm).
	 * @param VerticesPerMeter  Desired linear vertex density along the surface.
	 * @param OutMeshData       Receives the generated mesh.
	 * @param MaxSubdivisions   Safety clamp (each level ~4x's the triangle count).
	 * @return The subdivision level actually used.
	 */
	static int32 Generate(float Radius, float VerticesPerMeter, FSolarOrbzIcoSphereMeshData& OutMeshData, int32 MaxSubdivisions = 8);

	/** Generate an icosphere at an explicit subdivision level (0 = base icosahedron, 12 verts, 20 tris). */
	static void GenerateAtSubdivisionLevel(float Radius, int32 SubdivisionLevel, FSolarOrbzIcoSphereMeshData& OutMeshData);

	/** Returns the subdivision level whose average edge length best matches TargetEdgeLength, clamped to MaxSubdivisions. */
	static int32 ComputeSubdivisionLevelForEdgeLength(float Radius, float TargetEdgeLength, int32 MaxSubdivisions = 8);

	/** Vertex count of a base icosahedron subdivided N times (handy for UI feedback before generating). */
	static int64 EstimateVertexCount(int32 SubdivisionLevel);

	/**
	 * Recomputes per-vertex normals from the current triangle positions via area-weighted face-normal
	 * averaging. Call this after displacing Vertices (e.g. for terrain) - the original analytic sphere
	 * normals are no longer correct once the surface isn't a sphere anymore.
	 */
	static void RecomputeSmoothNormals(FSolarOrbzIcoSphereMeshData& MeshData);

	/**
	 * Equirectangular UV for a point on the unit sphere (U = longitude around Z, V = latitude from +Z).
	 * Shared by mesh UV generation and any code sampling a heightmap/biome texture, so both stay
	 * in exact agreement about which pixel corresponds to which point on the sphere.
	 */
	static FVector2D ComputeSphericalUV(const FVector& UnitSpherePosition);

	/**
	 * Recomputes area-weighted smooth vertex normals (and simple tangents) from the mesh's current
	 * vertex positions and triangle list. Call this after displacing vertices for terrain - the
	 * original sphere normals no longer describe the displaced surface.
	 */
	static void RecomputeSmoothNormalsAndTangents(FSolarOrbzIcoSphereMeshData& MeshData);

private:
	// Working data during subdivision - unit-sphere positions, radius is applied at finalize time.
	struct FBuildContext
	{
		TArray<FVector> Positions;
		TArray<uint32> Indices;
		TMap<uint64, int32> MidpointCache;
	};

	static void BuildBaseIcosahedron(FBuildContext& Context);
	static void SubdivideOnce(FBuildContext& Context);
	static int32 GetOrCreateMidpoint(FBuildContext& Context, int32 IndexA, int32 IndexB);
	static void FixUVSeamsAndFinalize(const FBuildContext& Context, float Radius, FSolarOrbzIcoSphereMeshData& OutMeshData);
};
