// SolarOrbzChunkResidentSetManager - the other half of checklist item 5 of Docs/
// ChunkedPlanetTerrain.md's "Phase 1, continued" section: "keep a map of currently-resident
// FSolarOrbzChunkAddress -> UProceduralMeshComponent. Each update: recompute the desired leaf-set,
// diff against what's currently resident, spawn newly-needed chunks, despawn/destroy no-longer-
// needed ones, leave unchanged chunks alone." This is the ENGINE-DEPENDENT half (AActor,
// UProceduralMeshComponent, NewObject/RegisterComponent/CreateMeshSection_LinearColor) - the pure
// set-difference itself lives in SolarOrbzChunkResidentSetDiff.h/.cpp, deliberately kept separate,
// same "pure part stays pure, engine part consumes it" split this subsystem already uses elsewhere
// (FSolarOrbzChunkLODPolicy / FSolarOrbzChunkResidencyWalker).
//
// ================================================================================================
// HARD CONSTRAINT - parallel mesh generation vs. sequential component creation. Read this first.
// ================================================================================================
// FSolarOrbzIcoSphereChunkGenerator::GenerateChunk is pure CPU work (no UObject/engine access at
// all - see its own header comment) and is safe to ParallelFor, exactly the way
// ASolarOrbzIcoSphereActor::RegenerateMesh already parallelizes its own per-vertex Pass A/B work
// (SolarOrbzIcoSphere.cpp). But NewObject<UProceduralMeshComponent>, RegisterComponent(),
// AttachToComponent(...), and CreateMeshSection_LinearColor(...) all touch UObject/engine state and
// MUST run on the game thread ONLY - never from inside a ParallelFor lambda. Getting this wrong
// (creating components inside the parallel loop) is a real, serious, genuinely hard-to-notice
// threading bug - UE's UObject/component machinery is not thread-safe, and nothing in a debug build
// necessarily flags the violation immediately; it can run for a while before corrupting something.
//
// This file enforces the correct shape as two CLEARLY SEPARATE phases inside UpdateResidentSet,
// never interleaved:
//   PHASE 1 (parallel, pure): ParallelFor over ToSpawn, calling GenerateChunk for each address into
//   its own slot of a plain TArray<FSolarOrbzIcoSphereMeshData> (same index as ToSpawn) - no
//   UObject/engine call anywhere inside that lambda.
//   PHASE 2 (sequential, game-thread, engine-touching): a plain for-loop AFTER Phase 1 has fully
//   completed, which creates one component per spawned address and feeds it that address's already-
//   computed mesh data from Phase 1's output array. Every NewObject/RegisterComponent/
//   AttachToComponent/CreateMeshSection_LinearColor/SetMaterial call lives only in this loop.
// See UpdateResidentSet's own implementation (SolarOrbzChunkResidentSetManager.cpp) for exactly
// where this split happens in code - the two phases are two separate, sequential blocks with a
// comment banner at the start of each, specifically so this ordering can be checked at a glance
// during review rather than having to trace control flow to confirm it.
//
// ================================================================================================
// Verification bar for this file - DIFFERENT from, and WEAKER than, SolarOrbzChunkResidentSetDiff's.
// ================================================================================================
// SolarOrbzChunkResidentSetDiff.h's pure set-difference got this project's normal strict bar (a
// Python ground truth, exhaustive + fuzzed, exact counts, zero failures - see that file's own
// header). THIS file cannot be verified that way: "did NewObject construct a valid
// UProceduralMeshComponent", "did RegisterComponent succeed", "did CreateMeshSection_LinearColor
// build the render proxy correctly" are inherently engine-runtime questions with no meaningful
// Python-equivalent ground truth to check against. This file is instead held to a CODE-REVIEW
// correctness bar only:
//   - CreateMeshSection_LinearColor is called with the exact same parameter shape/order
//     ASolarOrbzIcoSphereActor::RegenerateMesh already uses (SolarOrbzIcoSphere.cpp, around its own
//     ProcMesh->CreateMeshSection_LinearColor call): SectionIndex, Vertices, Triangles, Normals,
//     UV0, UV1, UV2, a 4th (here, always empty) UV channel, VertexColors as TArray<FLinearColor>,
//     TArray<FProcMeshTangent>, then a bool for collision. An empty TArray<FLinearColor> is passed
//     for vertex colors here deliberately - GenerateChunk's output (FSolarOrbzIcoSphereMeshData) has
//     no per-vertex color field at all yet (see that struct's own fields in SolarOrbzIcoSphere.h) -
//     biome-color wiring for streamed chunks is not built in this pass, same "known out of scope"
//     framing as everything else not yet wired up in this subsystem.
//   - Component lifecycle API ordering follows Unreal's own documented contract: NewObject(Outer)
//     before RegisterComponent(), RegisterComponent() before any mesh-section call (an unregistered
//     primitive component has no scene proxy to build a mesh section into), and AttachToComponent
//     after RegisterComponent (attaching an unregistered component is undefined per UE's own
//     component lifecycle documentation).
//   - This has NOT been compiled or run - there is no UE5.8 compiler available in this environment.
//     Everything above was checked by careful reading against this project's own already-shipped,
//     presumably-working RegenerateMesh call site and Unreal's documented component APIs, not by
//     execution. This is a genuinely weaker verification claim than SolarOrbzChunkResidentSetDiff's
//     own 20,004-case zero-failure Python check, and is stated plainly here rather than implied to
//     carry the same rigor.
//
// ================================================================================================
// What this file deliberately does NOT do (see Docs/ChunkedPlanetTerrain.md's own scope list).
// ================================================================================================
// - No per-chunk budget/prioritization for a single update that requests many chunks at once (a
//   large viewer jump/teleport) - every address in ToSpawn is generated and created in the same
//   UpdateResidentSet call, however many that is. A frame-budget/queue system is explicitly later
//   work, not built here (see the design doc's own "explicitly out of scope" list for this piece).
// - Skirts (item 6, FSolarOrbzChunkSkirtBuilder::AppendSkirts) ARE now applied - see
//   UpdateResidentSet's own Phase 1 comment below - but only the flat, non-distance-aware
//   SkirtDepth this class is constructed with; no per-edge/mismatch-aware sizing.
// - No update cadence/owning actor (item 7) - this class is a plain, non-UObject manager meant to be
//   OWNED by something else (a future AASolarOrbzChunkedPlanetActor) that decides WHEN to call
//   UpdateResidentSet; it has no Tick, timer, or interval logic of its own.
// - No hysteresis-aware merge logic beyond what GatherDesiredLeaves/ApplyNeighborDepthRestriction
//   already produce - this class only ever diffs against whatever desired leaf-set those two
//   functions hand it on a given call; it does not itself decide to keep a borderline chunk resident
//   past what that desired set says (see FSolarOrbzChunkLODPolicy's own header for the one
//   remaining hysteresis gap already flagged there).
// - No handling of a resident component whose owning Actor has been destroyed/is pending-kill out
//   from under this manager - this class assumes the AActor* it was constructed with outlives it
//   (the manager is meant to be owned BY that same actor, or by something with an equally-scoped
//   lifetime) and does not itself re-validate OwningActor on every call beyond a basic IsValid check.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzIcoSphereChunk.h"

class AActor;
class UMaterialInterface;
class UProceduralMeshComponent;
class USolarOrbzTerrainLayerStack;
struct FSolarOrbzChunkLODSettings;
struct FSolarOrbzClimateGrid;

/**
 * Owns the set of currently-resident chunk UProceduralMeshComponents and keeps it in sync with
 * whatever a viewer position/LOD settings currently desire - see this header's own top-of-file
 * comment for the full algorithm, the hard parallel/sequential ordering constraint, and this file's
 * (weaker, review-only) verification bar.
 */
class SOLARORBZ_API FSolarOrbzChunkResidentSetManager
{
public:
	/**
	 * @param InOwningActor              The actor new chunk components are created under (NewObject's
	 *                                    Outer) and attached to (via InOwningActor->GetRootComponent()
	 *                                    - see UpdateResidentSet's own comment for why
	 *                                    KeepRelativeTransform is the right attachment rule here).
	 *                                    Must outlive this manager - see this header's own "what this
	 *                                    file does NOT do" section.
	 * @param InRadius                    Planet radius, forwarded straight through to
	 *                                    GatherDesiredLeaves and GenerateChunk - same units
	 *                                    requirement as both of those (see their own headers).
	 * @param InTerrainStack              Forwarded straight through to GenerateChunk for every newly
	 *                                    spawned chunk. May be null (GenerateChunk then produces an
	 *                                    undisplaced sphere patch) - see GenerateChunk's own header.
	 * @param InClimateGridForMasking     Forwarded straight through to GenerateChunk exactly as
	 *                                    RunTerrainPassA forwards it in SolarOrbzIcoSphere.cpp. May
	 *                                    be null.
	 * @param InChunkResolution           Forwarded straight through to GenerateChunk's own Resolution
	 *                                    parameter for every newly spawned chunk - see that
	 *                                    function's own header for recommended ranges.
	 * @param InMaterial                  Assigned to material slot 0 (SetMaterial(0, InMaterial)) on
	 *                                    every newly created component. May be null (component then
	 *                                    renders with the engine's default material, same fallback
	 *                                    UMeshComponent::SetMaterial always has).
	 * @param InSkirtDepth                Forwarded to FSolarOrbzChunkSkirtBuilder::AppendSkirts for
	 *                                    every newly spawned chunk, run in Phase 1 (pure, parallel)
	 *                                    right after GenerateChunk - see UpdateResidentSet's own
	 *                                    comment. 0.0 is a well-defined "no skirt" default (every
	 *                                    skirt vertex then coincides exactly with its original - see
	 *                                    AppendSkirts' own header), not a special case to avoid.
	 */
	FSolarOrbzChunkResidentSetManager(
		AActor* InOwningActor,
		double InRadius,
		const USolarOrbzTerrainLayerStack* InTerrainStack,
		const FSolarOrbzClimateGrid* InClimateGridForMasking,
		int32 InChunkResolution,
		UMaterialInterface* InMaterial,
		double InSkirtDepth = 0.0);

	/** Destroys every still-resident component (see ClearAll) before this manager itself is torn down. */
	~FSolarOrbzChunkResidentSetManager();

	// Non-copyable - this class owns live UProceduralMeshComponents via a TMap; copying it would
	// either double-own or silently share those components between two "owners", neither of which
	// is a sensible meaning for "copy a resident-set manager". Moving isn't needed by any current
	// caller either, so it's left undeclared (implicitly deleted alongside the copy ops, same as any
	// class with a user-declared destructor) rather than given a half-considered implementation.
	FSolarOrbzChunkResidentSetManager(const FSolarOrbzChunkResidentSetManager&) = delete;
	FSolarOrbzChunkResidentSetManager& operator=(const FSolarOrbzChunkResidentSetManager&) = delete;

	/**
	 * Recomputes the desired leaf-set for ViewerWorldPosition/Settings (GatherDesiredLeaves +
	 * ApplyNeighborDepthRestriction), diffs it against the currently-resident address set
	 * (FSolarOrbzChunkResidentSetDiff::ComputeDiff), destroys components for every address that fell
	 * out of the desired set, and creates components for every newly-desired address - see this
	 * header's own top-of-file comment for the mandatory parallel-generate-then-sequential-create
	 * ordering this performs internally. Addresses present in both the old and new desired sets are
	 * left completely untouched (no regeneration, no component churn) - this is expected to be the
	 * common case once a viewer stops moving.
	 *
	 * @param ViewerWorldPosition  Same meaning/frame as GatherDesiredLeaves' own parameter - relative
	 *                             to the planet's center, same units as the Radius this manager was
	 *                             constructed with.
	 * @param Settings             Tunable LOD thresholds forwarded straight through to
	 *                             GatherDesiredLeaves.
	 */
	void UpdateResidentSet(const FVector& ViewerWorldPosition, const FSolarOrbzChunkLODSettings& Settings);

	/** Read-only access to the currently-resident address -> component map, e.g. for a debug overlay. */
	const TMap<FSolarOrbzChunkAddress, TObjectPtr<UProceduralMeshComponent>>& GetResidentComponents() const
	{
		return ResidentComponents;
	}

	/**
	 * Destroys every currently-resident component and empties the map - called from the destructor,
	 * also exposed publicly for a caller that wants to tear down all chunk geometry without
	 * destroying this manager itself (e.g. an owning actor going through EndPlay).
	 */
	void ClearAll();

private:
	AActor* OwningActor = nullptr;
	double Radius = 1.0;
	const USolarOrbzTerrainLayerStack* TerrainStack = nullptr;
	const FSolarOrbzClimateGrid* ClimateGridForMasking = nullptr;
	int32 ChunkResolution = 16;
	UMaterialInterface* Material = nullptr;
	double SkirtDepth = 0.0;

	TMap<FSolarOrbzChunkAddress, TObjectPtr<UProceduralMeshComponent>> ResidentComponents;
};
