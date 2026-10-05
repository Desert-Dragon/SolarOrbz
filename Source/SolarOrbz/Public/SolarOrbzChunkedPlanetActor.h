// AASolarOrbzChunkedPlanetActor - checklist item 7, the last of Docs/ChunkedPlanetTerrain.md's
// "Phase 1, continued" section: the actor that actually OWNS an update cadence and wires items 1-6
// together to run in-game. Everything before this item (point-location, the LOD policy, the
// unrestricted residency walk, the restricted-quadtree fixpoint, the resident-set diff/lifecycle
// manager, and skirts) existed only as callable pieces with nothing invoking them - this is that
// "nothing" becoming "something," not a new algorithm of its own.
//
// New actor, PARALLEL to ASolarOrbzIcoSphereActor (SolarOrbzIcoSphere.h) - not a replacement. The
// whole-sphere actor remains the right tool for a bounded preview/bake radius; this actor is for a
// true planetary-radius body that needs real ground-level detail, which a single mesh fundamentally
// cannot provide (see ASolarOrbzIcoSphereActor's own RadiusMeters comment).
//
// ================================================================================================
// What this actor actually does, each update tick.
// ================================================================================================
// On an interval (UpdateIntervalSeconds, not necessarily every frame - residency doesn't need to
// recompute every tick, see the design doc's own framing of this item), this actor:
//   1. Resolves a viewer world position (ResolveViewerWorldPosition, see below).
//   2. Calls FSolarOrbzChunkResidentSetManager::UpdateResidentSet(ViewerWorldPosition, LODSettings),
//      which internally already wires together GatherDesiredLeaves (item 3) ->
//      ApplyNeighborDepthRestriction (item 4) -> FSolarOrbzChunkResidentSetDiff::ComputeDiff (item 5)
//      -> GenerateChunk + FSolarOrbzChunkSkirtBuilder::AppendSkirts (item 6, run in the resident-set
//      manager's own Phase 1) -> real UProceduralMeshComponent spawn/despawn (item 5's Phase 2).
// That's the entire integration - this actor's own code is deliberately thin: own the properties, own
// the timer, own the resident-set manager instance, resolve a viewer position, call one function.
//
// ================================================================================================
// Viewer resolution (Phase 1 testing shape - the design doc explicitly allows "just an explicit
// world position for Phase 1 testing" instead of a real camera-manager integration).
// ================================================================================================
// Three ways to tell this actor where the viewer is, checked in this order:
//   1. ViewerActor set (a Pawn, a camera actor, anything with a world location) - uses
//      ViewerActor->GetActorLocation() relative to this actor's own location.
//   2. bUseViewerWorldPositionOverride is true - uses ViewerWorldPositionOverride directly (already
//      expected to be relative to this planet's center, same convention GatherDesiredLeaves/
//      FSolarOrbzChunkResidentSetManager already use) - the explicit-position path the design doc
//      calls out for Phase 1 testing without a real camera/pawn in the scene yet.
//   3. Neither configured - falls back to treating the viewer as sitting at this planet's own
//      center (FVector::ZeroVector relative position). This is NOT a sensible real viewer position,
//      but it IS a well-defined, non-crashing fallback: every chunk's distance-to-viewer becomes
//      ~Radius (its own distance from the center), producing a shallow, roughly-uniform desired
//      depth across the whole planet rather than undefined behavior or a crash. Logged once (not
//      every update) so a forgotten viewer configuration is noticeable without spamming the log.
//
// ================================================================================================
// Properties mirror ASolarOrbzIcoSphereActor's own shape (Radius/TerrainStack/BiomeStack/
// ClimateSimulation/Profile) for parity and future use - NOT all of them are actually wired into
// chunk generation yet.
// ================================================================================================
// RadiusMeters and TerrainStack ARE forwarded into FSolarOrbzChunkResidentSetManager and therefore
// into every generated chunk, exactly the way ASolarOrbzIcoSphereActor forwards its own RadiusMeters/
// TerrainStack into RunTerrainPassA. BiomeStack, ClimateSimulation, and Profile are declared here
// for parity with the whole-sphere actor's own property set (same framing the design doc's own item
// 7 description uses: "owns the same kind of references the preview actor does") and so a designer
// configuring this actor sees a familiar, consistent property set - but NONE of the three are
// actually read or forwarded into chunk generation yet. This is a real, deliberate gap, not an
// oversight: GenerateChunk's own output (FSolarOrbzIcoSphereMeshData) has no per-vertex color field
// yet (see FSolarOrbzChunkResidentSetManager's own header comment), so there is nothing for
// BiomeStack to actually feed into per-chunk, and running a whole-planet ClimateSimulation pass (the
// way ASolarOrbzIcoSphereActor's own RegenerateMesh does, to build the FSolarOrbzClimateGrid that
// feeds Climate Biome Masks and TerrainStack's own climate-masked layers) is real, separate plumbing
// work not attempted here - this actor currently forwards nullptr for ClimateGridForMasking on
// every chunk, same as leaving ClimateSimulation unset would do for the whole-sphere actor. Profile
// (gravity/atmosphere) is likewise just held here for a future ASN_MK1 integration pass (see the
// design doc's own "ASN_MK1 integration" section) - nothing in this actor reads it yet.
//
// ================================================================================================
// Verification - this is pure actor/engine wiring, held to the same bar as the rest of this
// subsystem's engine-dependent pieces (item 5's Part B), not the strict Python-verified bar the
// pure algorithmic pieces got.
// ================================================================================================
// There is no meaningful Python-equivalent ground truth for "does a UE timer fire on schedule" or
// "does this actor's BeginPlay/EndPlay sequence correctly construct/destroy a
// FSolarOrbzChunkResidentSetManager" - this is held to a code-review-correctness bar only (matching
// documented UE actor-lifecycle/timer-manager API usage) and has NOT been compiled or run - no
// UE5.8 compiler is available in this environment. The pieces this actor calls into
// (FSolarOrbzChunkResidentSetManager and everything underneath it) each carry their own,
// substantially stronger verification already - see their own files and Docs/ChunkedPlanetTerrain.md.
//
// ================================================================================================
// Known limitations / out of scope (see the design doc's own lists for this whole subsystem).
// ================================================================================================
// - No per-chunk spawn budget for a large viewer jump - inherited unchanged from
//   FSolarOrbzChunkResidentSetManager (item 5's own gap).
// - No BiomeStack/ClimateSimulation/Profile wiring into chunk generation yet - see above.
// - No baking/Nanite path, no real vertex-stitching (skirts only) - unchanged from every earlier
//   item in this checklist.
// - UpdateChunks runs synchronously on the game thread when its timer fires (the heavy per-chunk
//   mesh generation inside it is parallelized via FSolarOrbzChunkResidentSetManager's own Phase 1,
//   but the call itself still blocks the calling thread until that ParallelFor completes) - no
//   frame-spread/async dispatch of the update itself. Fine for Phase 1 (same assumption every piece
//   up the chain already makes), a real concern only once update intervals/chunk counts get large
//   enough for a single UpdateResidentSet call to cost a noticeable fraction of a frame.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzChunkLODPolicy.h"
#include "GameFramework/Actor.h"
#include "Engine/EngineTypes.h" // FTimerHandle - a value member below, needs the full type, not just a forward declaration.
#include "SolarOrbzChunkedPlanetActor.generated.h"

class FSolarOrbzChunkResidentSetManager;

/**
 * Owns an update cadence that wires the whole chunked-planet-terrain streaming pipeline (point-
 * location, the LOD policy, the residency walk, the restricted-quadtree fixpoint, the resident-set
 * diff/lifecycle manager, and skirts) together to run in-game - see this header's own top-of-file
 * comment for the full picture and what is/isn't actually wired up yet.
 */
UCLASS()
class SOLARORBZ_API AASolarOrbzChunkedPlanetActor : public AActor
{
	GENERATED_BODY()

public:
	AASolarOrbzChunkedPlanetActor();

	// Explicitly declared (defined in the .cpp, where FSolarOrbzChunkResidentSetManager's full type
	// is visible via its own header) rather than left implicit - ResidentSetManager below is a
	// TUniquePtr to a type only FORWARD-declared here. An implicitly-generated destructor would need
	// to instantiate that TUniquePtr's own destructor wherever THIS class's destructor is first
	// needed, which could happen from a context that only sees the forward declaration (an incomplete
	// type) and fail to compile. Declaring it here and defining it out-of-line in the .cpp guarantees
	// the instantiation happens only where the full type is already included.
	virtual ~AASolarOrbzChunkedPlanetActor() override;

	/**
	 * Planet radius, in meters - same convention as ASolarOrbzIcoSphereActor::RadiusMeters (double
	 * precision so planet-scale radii don't lose fractional-meter precision). Converted to UE units
	 * (cm) internally before being forwarded to FSolarOrbzChunkResidentSetManager/GenerateChunk,
	 * same *100.0 conversion RegenerateMesh already performs for the whole-sphere actor.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet", meta = (ClampMin = "0.01"))
	double RadiusMeters = 6371000.0;

	/** Optional terrain recipe, forwarded into every generated chunk exactly as ASolarOrbzIcoSphereActor forwards its own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Terrain")
	TObjectPtr<class USolarOrbzTerrainLayerStack> TerrainStack;

	/**
	 * Held for parity with ASolarOrbzIcoSphereActor's own property set - NOT yet wired into chunk
	 * generation (GenerateChunk's output has no per-vertex color field yet). See this header's own
	 * top-of-file comment.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Biome")
	TObjectPtr<class USolarOrbzBiomeStack> BiomeStack;

	/** Held for parity - NOT yet wired into chunk generation (no whole-planet climate grid is built here yet). See this header's own top-of-file comment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Climate")
	TObjectPtr<class USolarOrbzClimateSimulationAsset> ClimateSimulation;

	/** Held for a future ASN_MK1 gravity/atmosphere integration pass - NOT read by this actor yet. See Docs/ChunkedPlanetTerrain.md's "ASN_MK1 integration" section. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Profile")
	TObjectPtr<class USolarOrbzCelestialBodyProfile> Profile;

	/** Material assigned to every spawned chunk component's slot 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Terrain")
	TObjectPtr<class UMaterialInterface> Material;

	/** Subdivisions per chunk edge, forwarded to GenerateChunk for every newly spawned chunk - keep modest (16-64), see GenerateChunk's own header. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Chunking", meta = (ClampMin = "1"))
	int32 ChunkResolution = 16;

	/** Tunable LOD split/merge thresholds - see FSolarOrbzChunkLODSettings' own comment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Chunking")
	FSolarOrbzChunkLODSettings LODSettings;

	/**
	 * How far inward (along each boundary vertex's own normal, in UE units/cm), skirt geometry
	 * extends - see FSolarOrbzChunkSkirtBuilder's own header. 0.0 disables skirts (every skirt
	 * vertex coincides with its original). 100.0 (1 meter) is a first-pass placeholder, not tuned
	 * against this actor's own chunk sizes at any particular depth - a skirt much deeper than the
	 * smallest resident chunk's own edge length is wasted, and one much shallower than the actual
	 * worst-case 1-level depth mismatch (item 4) can still show a gap; revisit once real chunks are
	 * actually on screen to look at, same framing as FSolarOrbzChunkLODSettings' own thresholds.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Chunking", meta = (ClampMin = "0.0"))
	double SkirtDepth = 100.0;

	/** How often (seconds) the resident chunk set is recomputed - does not need to be every tick, see this header's own top-of-file comment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Chunking", meta = (ClampMin = "0.01"))
	double UpdateIntervalSeconds = 1.0;

	/** Optional. If set, the viewer's position each update is this actor's own world location (relative to this planet). See this header's own "Viewer resolution" comment for the full fallback order. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Viewer")
	TObjectPtr<AActor> ViewerActor;

	/** If true (and ViewerActor is unset), ViewerWorldPositionOverride below is used directly - the explicit-position path for Phase 1 testing without a real camera/pawn in the scene. See this header's own "Viewer resolution" comment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Viewer")
	bool bUseViewerWorldPositionOverride = false;

	/** Used only when bUseViewerWorldPositionOverride is true - relative to this planet's own center, same convention FSolarOrbzChunkResidentSetManager/GatherDesiredLeaves already use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SolarOrbz|ChunkedPlanet|Viewer")
	FVector ViewerWorldPositionOverride = FVector::ZeroVector;

	/**
	 * Forces an immediate resident-set recompute without waiting for the next timer tick - useful
	 * for testing from the editor/Blueprint. Lazily constructs the resident-set manager first if it
	 * doesn't exist yet (see RebuildChunkedPlanetNow()'s own comment on why that's needed at all) -
	 * this makes CallInEditor genuinely work on an actor sitting in the editor that has never been
	 * through BeginPlay, not just in PIE/a packaged game. Does NOT pick up changed RadiusMeters/
	 * TerrainStack/ChunkResolution/Material/SkirtDepth on an already-constructed manager - those are
	 * baked into the manager at construction time with no setters (see
	 * FSolarOrbzChunkResidentSetManager's own header); call RebuildChunkedPlanetNow() instead after
	 * changing any of those.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "SolarOrbz|ChunkedPlanet|Chunking")
	void UpdateChunksNow();

	/**
	 * Tears down the resident-set manager (destroying every currently-resident chunk component) and
	 * reconstructs it fresh from this actor's CURRENT RadiusMeters/TerrainStack/ChunkResolution/
	 * Material/SkirtDepth, then immediately recomputes the resident set - this is the entry point
	 * for "I changed the terrain recipe/radius/chunk settings, show me the new result," the same role
	 * ASolarOrbzIcoSphereActor::RegenerateMesh plays for the whole-sphere actor. UpdateChunksNow()
	 * alone is NOT enough for this: FSolarOrbzChunkResidentSetManager has no setters, so an
	 * already-constructed one keeps using whatever values it was built with regardless of what this
	 * actor's own properties change to afterward. Also the actor-side half of making CallInEditor
	 * genuinely usable standalone in the editor (no BeginPlay has ever run, so there's no existing
	 * manager to tear down the first time this is called) - mirrors UpdateChunksNow()'s own lazy
	 * construction for that same reason.
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "SolarOrbz|ChunkedPlanet|Chunking")
	void RebuildChunkedPlanetNow();

	/** Currently-resident chunk count, for a debug overlay/HUD. */
	UFUNCTION(BlueprintCallable, Category = "SolarOrbz|ChunkedPlanet|Chunking")
	int32 GetResidentChunkCount() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(EEndPlayReason::Type EndPlayReason) override;

private:
	/** Resolves where the viewer currently is, relative to this planet's own center - see this header's own "Viewer resolution" comment for the exact fallback order. */
	FVector ResolveViewerWorldPosition() const;

	/** Timer callback - resolves the viewer position and calls FSolarOrbzChunkResidentSetManager::UpdateResidentSet. */
	void UpdateChunks();

	/** (Re)constructs ResidentSetManager from this actor's current properties - shared by BeginPlay() and RebuildChunkedPlanetNow()/UpdateChunksNow()'s lazy-construction path, so there is exactly one place that reads RadiusMeters/TerrainStack/etc into the manager's constructor. */
	void ConstructResidentSetManager();

	/** Non-UObject, so a plain TUniquePtr rather than a UPROPERTY - see FSolarOrbzChunkResidentSetManager's own header for why it's a plain C++ class, not a UObject/component itself. Constructed in BeginPlay, destroyed (along with every resident component it owns) in EndPlay. */
	TUniquePtr<FSolarOrbzChunkResidentSetManager> ResidentSetManager;

	FTimerHandle UpdateTimerHandle;

	/** Set once ResolveViewerWorldPosition has logged its "no viewer configured" fallback warning, so it logs once per play session rather than once per update tick. */
	mutable bool bHasLoggedNoViewerWarning = false;
};
