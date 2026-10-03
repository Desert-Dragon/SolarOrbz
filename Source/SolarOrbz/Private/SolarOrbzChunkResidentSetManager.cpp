// See SolarOrbzChunkResidentSetManager.h for the algorithm, the hard parallel/sequential ordering
// constraint, and this file's (weaker, review-only) verification bar.

#include "SolarOrbzChunkResidentSetManager.h"
#include "SolarOrbzChunkResidentSetDiff.h"
#include "SolarOrbzChunkResidencyWalk.h"
#include "SolarOrbzChunkRestrictedQuadtree.h"
#include "SolarOrbzChunkLODPolicy.h"
#include "SolarOrbzChunkSkirtBuilder.h"
#include "ProceduralMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "Async/ParallelFor.h"

FSolarOrbzChunkResidentSetManager::FSolarOrbzChunkResidentSetManager(
	AActor* InOwningActor,
	double InRadius,
	const USolarOrbzTerrainLayerStack* InTerrainStack,
	const FSolarOrbzClimateGrid* InClimateGridForMasking,
	int32 InChunkResolution,
	UMaterialInterface* InMaterial,
	double InSkirtDepth)
	: OwningActor(InOwningActor)
	, Radius(InRadius)
	, TerrainStack(InTerrainStack)
	, ClimateGridForMasking(InClimateGridForMasking)
	, ChunkResolution(InChunkResolution)
	, Material(InMaterial)
	, SkirtDepth(InSkirtDepth)
{
}

FSolarOrbzChunkResidentSetManager::~FSolarOrbzChunkResidentSetManager()
{
	ClearAll();
}

void FSolarOrbzChunkResidentSetManager::ClearAll()
{
	for (const TPair<FSolarOrbzChunkAddress, TObjectPtr<UProceduralMeshComponent>>& Entry : ResidentComponents)
	{
		if (UProceduralMeshComponent* Component = Entry.Value)
		{
			if (IsValid(Component))
			{
				Component->DestroyComponent();
			}
		}
	}
	ResidentComponents.Reset();
}

void FSolarOrbzChunkResidentSetManager::UpdateResidentSet(const FVector& ViewerWorldPosition, const FSolarOrbzChunkLODSettings& Settings)
{
	if (!IsValid(OwningActor))
	{
		// Nothing sensible to do without a live owning actor - no Outer for NewObject, no root
		// component to attach to. Matches this header's own documented assumption that OwningActor
		// outlives this manager; a caller that destroys its actor out from under this manager should
		// also destroy the manager (or at least stop calling UpdateResidentSet on it).
		return;
	}

	// --- Recompute the desired leaf-set (checklist items 1-4) ---
	TArray<FSolarOrbzChunkAddress> DesiredLeaves;
	FSolarOrbzChunkResidencyWalker::GatherDesiredLeaves(Radius, ViewerWorldPosition, Settings, DesiredLeaves);
	FSolarOrbzChunkRestrictedQuadtree::ApplyNeighborDepthRestriction(DesiredLeaves);

	// --- Diff against what's currently resident (FSolarOrbzChunkResidentSetDiff, pure/verified) ---
	TArray<FSolarOrbzChunkAddress> PreviousLeaves;
	ResidentComponents.GenerateKeyArray(PreviousLeaves);

	TArray<FSolarOrbzChunkAddress> ToSpawn;
	TArray<FSolarOrbzChunkAddress> ToDespawn;
	FSolarOrbzChunkResidentSetDiff::ComputeDiff(PreviousLeaves, DesiredLeaves, ToSpawn, ToDespawn);

	// --- Despawn no-longer-needed chunks first (cheap, game-thread, no reason to delay it) ---
	for (const FSolarOrbzChunkAddress& Address : ToDespawn)
	{
		if (TObjectPtr<UProceduralMeshComponent>* Found = ResidentComponents.Find(Address))
		{
			if (UProceduralMeshComponent* Component = *Found)
			{
				if (IsValid(Component))
				{
					Component->DestroyComponent();
				}
			}
		}
		ResidentComponents.Remove(Address);
	}

	// Addresses present in both the old and new desired sets are intentionally not visited anywhere
	// above or below this point - "leave unchanged chunks alone" per this file's own header comment.

	if (ToSpawn.Num() == 0)
	{
		return;
	}

	// ================================================================================================
	// PHASE 1 (parallel, PURE - no UObject/engine access anywhere in this lambda).
	// ================================================================================================
	// GenerateChunk takes no AActor/UObject/UWorld reference (see its own header comment) - safe to
	// ParallelFor across ToSpawn exactly the way ASolarOrbzIcoSphereActor::RegenerateMesh's own Pass
	// A/B loops already parallelize pure per-vertex work in SolarOrbzIcoSphere.cpp.
	// FSolarOrbzChunkSkirtBuilder::AppendSkirts is likewise pure (no UObject/engine access - see its
	// own header) and is run immediately after GenerateChunk, still inside the same parallel
	// iteration, so every spawned chunk's mesh data already has its skirt geometry by the time Phase
	// 2 below ever sees it. MeshDataForSpawn is indexed 1:1 with ToSpawn (MeshDataForSpawn[i] is
	// ToSpawn[i]'s generated-and-skirted mesh) - each ParallelFor iteration writes only to its own
	// index, so there is no shared mutable state between iterations and no UProceduralMeshComponent
	// is created, touched, or even referenced in here.
	TArray<FSolarOrbzIcoSphereMeshData> MeshDataForSpawn;
	MeshDataForSpawn.SetNum(ToSpawn.Num());

	ParallelFor(ToSpawn.Num(), [this, &ToSpawn, &MeshDataForSpawn](int32 Index)
	{
		FSolarOrbzIcoSphereChunkGenerator::GenerateChunk(
			ToSpawn[Index],
			ChunkResolution,
			Radius,
			TerrainStack,
			ClimateGridForMasking,
			MeshDataForSpawn[Index]);

		FSolarOrbzChunkSkirtBuilder::AppendSkirts(ChunkResolution, SkirtDepth, MeshDataForSpawn[Index]);
	});

	// ================================================================================================
	// PHASE 2 (sequential, game-thread - EVERY engine/UObject call for newly-spawned chunks lives here).
	// ================================================================================================
	// Phase 1 has fully completed by this point (ParallelFor blocks until every iteration is done) -
	// nothing below runs concurrently with anything above. One component per ToSpawn entry, fed that
	// entry's already-computed mesh data from Phase 1; no GenerateChunk call happens in this loop.
	USceneComponent* AttachTarget = OwningActor->GetRootComponent();

	for (int32 Index = 0; Index < ToSpawn.Num(); ++Index)
	{
		const FSolarOrbzChunkAddress& Address = ToSpawn[Index];
		const FSolarOrbzIcoSphereMeshData& MeshData = MeshDataForSpawn[Index];

		UProceduralMeshComponent* Component = NewObject<UProceduralMeshComponent>(OwningActor);
		Component->RegisterComponent();

		if (AttachTarget)
		{
			// KeepRelativeTransform: a freshly NewObject'd component has an identity relative
			// transform, which is exactly what's wanted here - MeshData's vertex positions are
			// already in the same planet-center-relative local space the owning actor's root sits
			// at (GenerateChunk scales unit directions by Radius directly, same convention
			// RegenerateMesh's own CachedMeshData uses), so the chunk component should sit exactly
			// at its parent's origin with no additional offset, not be moved to preserve some prior
			// WORLD position it never had.
			Component->AttachToComponent(AttachTarget, FAttachmentTransformRules::KeepRelativeTransform);
		}

		// Same exact parameter shape ASolarOrbzIcoSphereActor::RegenerateMesh uses for its own
		// ProcMesh->CreateMeshSection_LinearColor call (SolarOrbzIcoSphere.cpp) - SectionIndex,
		// Vertices, Triangles, Normals, UV0, UV1, UV2, a 4th (here always empty) UV channel,
		// VertexColors, Tangents, then a bool for collision. GenerateChunk's output has no per-
		// vertex color field yet (FSolarOrbzIcoSphereMeshData, SolarOrbzIcoSphere.h) - an empty
		// TArray<FLinearColor> is the correct, expected input for that, not a bug; biome-color
		// wiring for streamed chunks is explicitly out of scope for this pass (see this file's own
		// header comment).
		TArray<FProcMeshTangent> ProcTangents;
		ProcTangents.Reserve(MeshData.Tangents.Num());
		for (const FVector& Tangent : MeshData.Tangents)
		{
			ProcTangents.Add(FProcMeshTangent(Tangent, false));
		}

		const TArray<FVector2D> EmptyUVChannel;
		const TArray<FLinearColor> EmptyVertexColors;

		// Collision generation (the final bool) is left false here - not asked for by this task, and
		// per-chunk collision cost/policy (every streamed-in chunk cooking a collision mesh, vs. some
		// cheaper proxy) is a real future decision, not one to make silently inside this piece. Flagged
		// here rather than solved: a caller that needs the player to actually stand on streamed terrain
		// will need this revisited.
		Component->CreateMeshSection_LinearColor(
			0,
			MeshData.Vertices,
			MeshData.Triangles,
			MeshData.Normals,
			MeshData.UVs,
			EmptyUVChannel,
			EmptyUVChannel,
			EmptyUVChannel,
			EmptyVertexColors,
			ProcTangents,
			false);

		Component->SetMaterial(0, Material);

		ResidentComponents.Add(Address, Component);
	}
}
