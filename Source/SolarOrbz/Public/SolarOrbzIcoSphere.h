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
	static int32 Generate(double Radius, float VerticesPerMeter, FSolarOrbzIcoSphereMeshData& OutMeshData, int32 MaxSubdivisions = 8, int32* OutUnclampedLevel = nullptr);

	/** Generate an icosphere at an explicit subdivision level (0 = base icosahedron, 12 verts, 20 tris). */
	static void GenerateAtSubdivisionLevel(double Radius, int32 SubdivisionLevel, FSolarOrbzIcoSphereMeshData& OutMeshData);

	/**
	 * Returns the subdivision level whose average edge length best matches TargetEdgeLength, clamped to MaxSubdivisions.
	 * @param OutUnclampedLevel  Optional. Receives the level before clamping - compare to the return value
	 *                           to detect when MaxSubdivisions, not the density target, determined the result.
	 */
	static int32 ComputeSubdivisionLevelForEdgeLength(double Radius, float TargetEdgeLength, int32 MaxSubdivisions = 8, int32* OutUnclampedLevel = nullptr);

	/** Vertex count of a base icosahedron subdivided N times (handy for UI feedback before generating). */
	static int64 EstimateVertexCount(int32 SubdivisionLevel);

	/**
	 * The 12 unit-sphere vertices and 20 triangular faces (index triples into those vertices, wound
	 * CCW viewed from outside) of the base icosahedron this generator subdivides - exposed so
	 * FSolarOrbzIcoSphereChunkGenerator (SolarOrbzIcoSphereChunk.h) can address "which of the 20 base
	 * faces does this chunk descend from" against the exact same table BuildBaseIcosahedron uses
	 * internally, rather than a second, possibly-drifting copy of the same 12 numbers.
	 */
	static void GetBaseIcosahedron(TArray<FVector>& OutVertices, TArray<FIntVector>& OutFaces);

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
	static void FixUVSeamsAndFinalize(const FBuildContext& Context, double Radius, FSolarOrbzIcoSphereMeshData& OutMeshData);
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

	/**
	 * Sphere radius, in meters. No upper limit - performance is your only ceiling at extreme
	 * scales/densities. Double precision so planet-scale radii (Earth is ~6.37 million meters)
	 * don't lose the fractional-meter precision terrain layers still author in.
	 *
	 * IMPORTANT: a single mesh cannot show ground-level (meter-scale) detail at planetary radius -
	 * see Max Subdivisions below. This actor is a good fit for a bounded preview/bake radius, or
	 * for zoomed-in testing; a full Earth-scale planet with real ground-level terrain needs a
	 * chunked/streaming LOD terrain system (planned separately), not a bigger single mesh.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "0.01"))
	double RadiusMeters = 1000.0;

	/**
	 * Desired vertex density along the surface, vertices per meter. No upper limit, but the
	 * subdivision level actually needed to hit this at your current Radius is clamped by Max
	 * Subdivisions below - watch the log/stats line to see whether the density target was reached.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "0.001"))
	float VerticesPerMeter = 1.0f;

	/**
	 * Subdivision level cap - each +1 is roughly 4x the triangle count. Hard-clamped at 11
	 * (~42 million vertices before UV-seam/pole duplication) since this actor recomputes its
	 * whole mesh live on every property change; going higher risks freezing or crashing the
	 * editor rather than just being slow. For reference, level 9-10 is already heavy for live
	 * editing - prefer keeping this low and using Bake To Static Mesh for a denser one-off result.
	 * Raising this will NOT make ground-level detail visible at planetary radius - triangle edge
	 * length scales with Radius, so the same subdivision level covers far more physical distance
	 * per triangle on a bigger planet. Watch the log for a warning when the gap is this large.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|IcoSphere", meta = (ClampMin = "0", ClampMax = "11", UIMax = "9"))
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

	/**
	 * Optional. This body's physical identity - gravity, atmosphere, landmass counts for a planet;
	 * different data entirely for a star or asteroid. Assign a Planet/Star/Asteroid Profile asset
	 * here. When a Planet Profile is assigned, its Atmosphere Density At Sea Level feeds the Climate
	 * Simulation above automatically - Climate Simulation no longer authors its own copy, so this is
	 * the one place that number lives. Leave unset to fall back to Earth's atmosphere density (1.225
	 * kg/m^3) for Climate Simulation, and 9.81 m/s^2 gravity if you read GetResolvedSurfaceGravity().
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|Profile")
	TObjectPtr<class USolarOrbzCelestialBodyProfile> Profile;

	/** This body's gravity, m/s^2 - resolved from Profile if it's a Planet Profile, otherwise Earth's 9.81 as a sane fallback. Read this for player physics so gravity is correct without a separate volume to keep in sync. */
	UFUNCTION(BlueprintCallable, Category = "SolarOrbz|Profile")
	float GetResolvedSurfaceGravity() const;

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

#if WITH_EDITOR
	/**
	 * Editor-only. When true, editing ANY property this actor's terrain actually depends on - not
	 * just this actor's own fields above (already regenerate immediately via RegenTriggers below),
	 * but also any layer inside the assigned Terrain Stack, Biome Stack and the Biome assets it
	 * references, Climate Simulation, or Profile - automatically regenerates this preview a short
	 * pause after the last edit. No more needing to click Regenerate after a tweak made in the
	 * Terrain Graph Editor or on another asset's own Details panel. Purely an editor convenience -
	 * the whole mechanism (a global FCoreUObjectDelegates::OnObjectPropertyChanged listener) is
	 * compiled out of packaged builds via WITH_EDITOR, so it can never fire at runtime. Turn this off
	 * if you're about to make several edits to an expensive stack and would rather batch them before
	 * one manual Regenerate click.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Live Preview")
	bool bLiveRegenerateInEditor = true;

	/**
	 * Editor-only. How long to wait, in seconds, after the last qualifying edit before actually
	 * regenerating - restarted by every new qualifying edit, so a rapid run of edits (or a slider
	 * drag, which fires one property-changed event per frame) pays for one regenerate after you stop,
	 * not one per edit.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Live Preview", meta = (ClampMin = "0.0", ClampMax = "5.0", EditCondition = "bLiveRegenerateInEditor"))
	float LiveRegenerateDebounceSeconds = 0.3f;
#endif

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
	virtual void BeginDestroy() override;
#endif

	UPROPERTY(VisibleAnywhere, Category = "SolarOrbz|IcoSphere")
	TObjectPtr<UProceduralMeshComponent> ProcMesh;

private:
	// Cached from the last RegenerateMesh() call so Bake doesn't need to redo the generation work.
	FSolarOrbzIcoSphereMeshData CachedMeshData;
	FSolarOrbzClimateGrid CachedClimateGrid;
	int32 LastSubdivisionLevelUsed = 0;
	int32 LastRequestedSubdivisionLevel = 0;

	// Per-triangle material assignment cached from the last RegenerateMesh() call, built right after
	// Pass B (see RegenerateMesh's own comment at that pass) when BiomeStack is assigned - one entry
	// in CachedTriangleMaterialIndex per triangle (CachedMeshData.Triangles.Num() / 3), either an
	// index into CachedBakeMaterials or INDEX_NONE ("no biome applies here, use DefaultMaterial").
	// Read by BakeToStaticMeshAsset to build one real material slot per unique biome-selected
	// material instead of baking with a single empty slot - replaces the old shared Biome Blend
	// Material/BiomeTextureArray/vertex-color-weight system entirely (see ROADMAP.md), which blended
	// several biomes' textures per point in a shader; this instead gives each triangle exactly one
	// real UMaterialInterface, since that's what a baked static mesh's material slots actually are.
	// NOT consumed by the live ProcMesh preview, which still renders as a single section (either
	// DefaultMaterial, or DebugBiomeMaterial's per-vertex-color boundary view) - deepening the BAKE
	// pipeline specifically was the ask, not a live multi-material preview.
	TArray<int32> CachedTriangleMaterialIndex;
	TArray<TObjectPtr<class UMaterialInterface>> CachedBakeMaterials;

#if WITH_EDITOR
	/**
	 * Handles a property change that can be satisfied WITHOUT re-running the whole
	 * geometry/terrain/climate/biome pipeline - today, just DefaultMaterial changing while it's
	 * actually the live material slot (not debug-color mode). Returns true if it fully handled the
	 * change (caller should NOT also call RegenerateMesh()); false means "not a case this knows how
	 * to fast-path, fall back to a full regenerate" - always correct, just not always the fastest
	 * path. Deliberately conservative: bShowBiomeDebugColors toggling and DebugBiomeMaterial changes
	 * always fall through to a full regenerate, since BiomeDebugColors isn't cached across calls
	 * today - nothing to reuse for the debug-color material. A biome's own Materials/slope settings
	 * live on the Biome/BiomeStack assets, not as a property of this actor, so there's no equivalent
	 * fast path for them either way - changing those always needs an explicit Regenerate/Bake, same
	 * as any other asset-level (not actor-property) edit already does.
	 */
	bool TryApplyCosmeticOnlyChange(FName ChangedPropertyName);

	/**
	 * Bound to FCoreUObjectDelegates::OnObjectPropertyChanged (a global, editor-only delegate that
	 * fires after ANY UObject's PostEditChangeProperty runs, anywhere in the editor) from
	 * OnConstruction - the only way this actor can find out a property changed on an object it
	 * doesn't own outright (a layer inside Terrain Stack, a Biome Stack entry's Biome asset, Climate
	 * Simulation, Profile), since none of those classes have any "notify changed" hook of their own.
	 * Ignores this actor's own properties (ModifiedObject == this) - those already go through
	 * PostEditChangeProperty/RegenTriggers above, immediately and not debounced; this only ever
	 * schedules a DEBOUNCED regenerate for everything else.
	 */
	void HandleAnyObjectPropertyChanged(UObject* ModifiedObject, FPropertyChangedEvent& PropertyChangedEvent);

	/**
	 * True if Object is (or is nested inside, via its Outer chain) TerrainStack, BiomeStack, any
	 * Biome asset BiomeStack's Layers currently reference, ClimateSimulation, or Profile - i.e. "this
	 * is something regenerating this preview should actually react to," not just any object anywhere
	 * being edited. Known gap, not fixed here: doesn't walk further than one hop into cross-asset
	 * references (e.g. a Composite Mask inside one referenced Biome pointing at a second, different
	 * Biome asset) - editing that second Biome directly won't be noticed. A deliberate simplification,
	 * not exhaustive transitive-reference tracking.
	 */
	bool IsObjectRelevantToLivePreview(const UObject* Object) const;

	/**
	 * (Re)starts LiveRegenerateTimerHandle for LiveRegenerateDebounceSeconds from now, calling
	 * RegenerateMesh() once it fires - repeated calls before it fires just push the fire time back
	 * out, which is what actually debounces a run of edits into one regenerate. Regenerates
	 * immediately instead if this actor has no World to own a timer (e.g. an asset-preview context).
	 */
	void ScheduleDebouncedRegenerate();

	bool bRegisteredForLivePreview = false;
	FTimerHandle LiveRegenerateTimerHandle;
#endif
};
