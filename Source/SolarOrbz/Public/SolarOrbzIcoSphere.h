// SolarOrbz - IcoSphere subsystem. Combines the CPU-side texture sampler (used by Heightmap and
// Stamp terrain layers), the parametric icosphere mesh generator, and the editor preview actor
// that ties everything (Terrain/Climate/Biome) together into one live ProceduralMeshComponent.
// These three are grouped because the actor directly owns/drives the other two - there's no
// meaningful way to use the generator or sampler without eventually going through the actor.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "SolarOrbzClimateSimulation.h"
#include "SolarOrbzIcoSphere.generated.h"

class UTexture2D;

// ================================================================================================
// FSolarOrbzTextureHeightSampler - Shared CPU-side texture sampler. Decodes a UTexture2D's editor
// source data once and caches it, so multiple layers can sample real pixel values without
// depending on GPU compression settings or a render round trip. Not a UObject - just a plain
// helper struct embedded (usually as a mutable member) inside whichever layer needs it.
// ================================================================================================
struct SOLARORBZ_API FSolarOrbzTextureHeightSampler
{
	/** Decodes Texture's source data if it isn't already cached for this exact texture. Returns false if decoding failed or Texture is null. */
	bool EnsureDecoded(UTexture2D* Texture);

	/** Bilinear sample, 0..1. U wraps (for equirectangular/longitude use); V clamps (for latitude/local stamp use). */
	float SampleBilinear01(float U, float V) const;

private:
	TArray<float> CachedHeights01; // width*height, row-major, 0..1
	int32 CachedWidth = 0;
	int32 CachedHeight = 0;
	TWeakObjectPtr<UTexture2D> CachedTexture;
};

// ================================================================================================
// FSolarOrbzIcoSphereMeshData / FSolarOrbzIcoSphereGenerator - Parametric IcoSphere Generator.
// Produces a geodesic sphere by subdividing a regular icosahedron, with seam-corrected UVs so
// it's ready for texturing and terrain displacement without visible cracks or pinched poles.
// ================================================================================================

/**
 * Raw mesh data for a generated icosphere. Feed this into a UProceduralMeshComponent, a
 * MeshDescription-based UStaticMesh build, or any other mesh consumer.
 *
 * NOTE: Because of UV-seam and pole splitting, Vertices.Num() will be somewhat larger than the
 * "geometric" vertex count of the icosphere - this is expected and required for correct texture mapping.
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
 * Recursively subdivides a regular icosahedron and re-projects new vertices onto the target
 * sphere. This keeps triangle sizes far more uniform than a UV/lat-long sphere - important once
 * vertices start getting displaced for terrain.
 */
class SOLARORBZ_API FSolarOrbzIcoSphereGenerator
{
public:
	/**
	 * Generate an icosphere sized so the average edge length is close to
	 * (100 / VerticesPerMeter) UE units - i.e. VerticesPerMeter is verts
	 * per meter of surface, since UE units are centimeters.
	 *
	 * @param Radius              Sphere radius, in UE units (cm).
	 * @param VerticesPerMeter    Desired linear vertex density along the surface.
	 * @param OutMeshData         Receives the generated mesh.
	 * @param MaxSubdivisions     Safety clamp (each level ~4x's the triangle count).
	 * @param OutUnclampedLevel   Optional. Receives the subdivision level that would have been needed
	 *                            to actually hit VerticesPerMeter, before clamping to MaxSubdivisions.
	 *                            Compare this to the return value: if they differ, the requested density
	 *                            was NOT reached - MaxSubdivisions was the binding constraint instead.
	 * @return The subdivision level actually used.
	 */
	static int32 Generate(float Radius, float VerticesPerMeter, FSolarOrbzIcoSphereMeshData& OutMeshData, int32 MaxSubdivisions = 8, int32* OutUnclampedLevel = nullptr);

	/** Generate an icosphere at an explicit subdivision level (0 = base icosahedron, 12 verts, 20 tris). */
	static void GenerateAtSubdivisionLevel(float Radius, int32 SubdivisionLevel, FSolarOrbzIcoSphereMeshData& OutMeshData);

	/**
	 * Returns the subdivision level whose average edge length best matches TargetEdgeLength, clamped to MaxSubdivisions.
	 * @param OutUnclampedLevel  Optional. Receives the level before clamping - compare to the return value
	 *                           to detect when MaxSubdivisions, not the density target, determined the result.
	 */
	static int32 ComputeSubdivisionLevelForEdgeLength(float Radius, float TargetEdgeLength, int32 MaxSubdivisions = 8, int32* OutUnclampedLevel = nullptr);

	/** Vertex count of a base icosahedron subdivided N times (handy for UI feedback before generating). */
	static int64 EstimateVertexCount(int32 SubdivisionLevel);

	/**
	 * Recomputes per-vertex normals from the current triangle positions via area-weighted face-normal
	 * averaging. Call this after displacing Vertices (e.g. for terrain) - the original analytic sphere
	 * normals are no longer correct once the surface isn't a sphere anymore.
	 */
	static void RecomputeSmoothNormals(FSolarOrbzIcoSphereMeshData& MeshData);

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

// ================================================================================================
// ASolarOrbzIcoSphereActor - Editor preview actor: builds a live ProceduralMeshComponent icosphere
// from FSolarOrbzIcoSphereGenerator above, applies Terrain/Climate/Biome on top, and can bake the
// result out to a UStaticMesh asset.
// ================================================================================================
UCLASS(BlueprintType, meta = (DisplayName = "SolarOrbz IcoSphere"))
class SOLARORBZ_API ASolarOrbzIcoSphereActor : public AActor
{
	GENERATED_BODY()

public:
	ASolarOrbzIcoSphereActor();

	/** Sphere radius, in meters. No upper limit - performance is your only ceiling at extreme scales/densities. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "0.01"))
	float RadiusMeters = 1000.0f;

	/** Desired vertex density along the surface, vertices per meter. No upper limit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "0.001"))
	float VerticesPerMeter = 1.0f;

	/** Subdivision level cap. No upper limit - each +1 is roughly 4x the triangle count, so watch the stats line once you push this high. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "0"))
	int32 MaxSubdivisions = 6;

	/** Optional terrain recipe (procedural noise and/or an authored heightmap) applied as radial displacement after the base sphere is built. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Terrain")
	TObjectPtr<class USolarOrbzTerrainLayerStack> TerrainStack;

	/** Optional biome stack - adds biome-specific terrain detail on top of TerrainStack, masked by climate/composite conditions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Biome")
	TObjectPtr<class USolarOrbzBiomeStack> BiomeStack;

	/**
	 * Optional. Runs a whole-planet climate simulation once per regenerate, right after TerrainStack
	 * displaces the mesh - a wind/orographic moisture pass plus a latitude+elevation temperature model,
	 * both on an independent lat/long grid. Feeds real Temperature/Moisture values into every Climate
	 * Biome Mask's sample context instead of their noise-based fallback.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Climate")
	TObjectPtr<class USolarOrbzClimateSimulationAsset> ClimateSimulation;

	/** When enabled, colors each vertex by its dominant biome's Preview Color instead of the normal material, so you can see layer boundaries directly. Needs an unlit material that reads vertex color assigned to Debug Biome Material below. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Biome|Debug")
	bool bShowBiomeDebugColors = false;

	/** Simple unlit material with VertexColor wired to Emissive Color - create one once and assign it here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Biome|Debug")
	TObjectPtr<class UMaterialInterface> DebugBiomeMaterial;

	/** Material restored when Show Biome Debug Colors is turned back off. Leave unset to just fall back to the engine default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Biome|Debug")
	TObjectPtr<class UMaterialInterface> DefaultMaterial;

	/** Build simple collision on the preview mesh. Leave off for large/high-density previews - cheap to add later on the baked mesh instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|IcoSphere")
	bool bEnablePreviewCollision = false;

	/** Regenerates the ProceduralMeshComponent from the current parameters. Also runs automatically when a parameter above changes. */
	UFUNCTION(CallInEditor, Category = "SolarOrbz|IcoSphere")
	void RegenerateMesh();

	/** Content-browser folder the baked asset is written to, e.g. "/Game/SolarOrbz/Meshes". */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Bake")
	FString BakePackagePath = TEXT("/Game/SolarOrbz/Meshes");

	/** Asset name for the baked static mesh. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Bake")
	FString BakeAssetName = TEXT("SM_IcoSphere");

	/** Bakes the current preview mesh out to a UStaticMesh asset on disk. */
	UFUNCTION(CallInEditor, Category = "SolarOrbz|Bake", meta = (DisplayName = "Bake To Static Mesh"))
	void BakeToStaticMeshAsset();

	/** Subdivision level used for the last generated preview. */
	int32 GetLastSubdivisionLevelUsed() const { return LastSubdivisionLevelUsed; }

	/** Subdivision level Vertices Per Meter actually asked for, before the MaxSubdivisions clamp. */
	int32 GetLastRequestedSubdivisionLevel() const { return LastRequestedSubdivisionLevel; }

	/** True if MaxSubdivisions, not Vertices Per Meter, determined the last generation's resolution - i.e. the density slider is currently doing nothing because the cap is binding instead. */
	bool WasLastGenerationDensityLimited() const { return LastRequestedSubdivisionLevel > LastSubdivisionLevelUsed; }

	/** Vertex count of the last generated preview (post UV-seam/pole splitting). */
	int32 GetPreviewVertexCount() const { return CachedMeshData.Vertices.Num(); }

	/** Triangle count of the last generated preview. */
	int32 GetPreviewTriangleCount() const { return CachedMeshData.Triangles.Num() / 3; }

	/** Climate grid from the last RegenerateMesh() call. Invalid (Width/Height 0) if no ClimateSimulation is assigned. */
	const FSolarOrbzClimateGrid& GetCachedClimateGrid() const { return CachedClimateGrid; }

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UPROPERTY(VisibleAnywhere, Category = "SolarOrbz|IcoSphere")
	TObjectPtr<UProceduralMeshComponent> ProcMesh;

private:
	// Cached from the last RegenerateMesh() call so Bake doesn't need to redo the generation work.
	FSolarOrbzIcoSphereMeshData CachedMeshData;
	FSolarOrbzClimateGrid CachedClimateGrid;
	int32 LastSubdivisionLevelUsed = 0;
	int32 LastRequestedSubdivisionLevel = 0;
};
