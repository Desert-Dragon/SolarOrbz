// See SolarOrbzChunkedPlanetActor.h for the full picture, viewer-resolution fallback order, and
// what is/isn't wired up yet.

#include "SolarOrbzChunkedPlanetActor.h"
#include "SolarOrbzChunkResidentSetManager.h"
#include "SolarOrbzTerrainLayers.h"
#include "SolarOrbzProfiles.h"
#include "SolarOrbzClimateSimulation.h"
#include "Components/SceneComponent.h"
#include "TimerManager.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzChunkedPlanet, Log, All);

AASolarOrbzChunkedPlanetActor::AASolarOrbzChunkedPlanetActor()
{
	PrimaryActorTick.bCanEverTick = false; // update cadence is a timer, not Tick - see header comment.

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

// Defined here (not left implicit) so TUniquePtr<FSolarOrbzChunkResidentSetManager>'s own destructor
// is instantiated only now that FSolarOrbzChunkResidentSetManager's full type (included above) is
// visible - see the header's own comment on this. '= default' is enough; there's no other cleanup
// to add here beyond what ResidentSetManager's own TUniquePtr reset/EndPlay already handle.
AASolarOrbzChunkedPlanetActor::~AASolarOrbzChunkedPlanetActor() = default;

void AASolarOrbzChunkedPlanetActor::ConstructResidentSetManager()
{
	const double RadiusCm = RadiusMeters * 100.0;

	if (TerrainStack)
	{
		// Same once-per-regenerate setup ASolarOrbzIcoSphereActor::RegenerateMesh performs before any
		// per-vertex EvaluateHeight call (SolarOrbzIcoSphere.cpp) - a real, hands-on-reported gap:
		// this actor never called it at all, so any whole-surface-baked layer (Erosion, Terrace,
		// Continent's VoronoiGrowth/PlateTectonics - anything whose RequiresWholeSurfaceBake()
		// returns true) had never run its Bake() pass by the time GenerateChunk started sampling
		// GetRawHeight per vertex. Each of those layers' own "Bake() hasn't run yet" fallback returns
		// a flat, direction-independent height (e.g. USolarOrbzContinentTerrainLayer::
		// GetRawHeightFromBakedGrid falls back to plain OceanFloorDepthMeters everywhere), so the
		// layer contributed a uniform offset instead of real per-point shape - exactly "not seeing a
		// lot of vertex motion" with those algorithms enabled, while RadialSeeds (which doesn't
		// require a bake) still worked. ApplyPlanetaryContext must run first - Bake() can read back
		// CachedSeaLevelCm/Profile-derived data through it, same ordering RegenerateMesh uses.
		const USolarOrbzPlanetProfile* PlanetProfile = Cast<USolarOrbzPlanetProfile>(Profile);
		const float SeaLevelCm = ClimateSimulation ? ClimateSimulation->SeaLevel * 100.0f : 0.0f;
		TerrainStack->ApplyPlanetaryContext(PlanetProfile, SeaLevelCm);
		TerrainStack->PrepareLayers(RadiusCm);
	}

	// ClimateGridForMasking is forwarded as nullptr - no whole-planet climate grid is built by this
	// actor yet, see this class's own header comment on ClimateSimulation/BiomeStack/Profile.
	ResidentSetManager = MakeUnique<FSolarOrbzChunkResidentSetManager>(
		this,
		RadiusCm,
		TerrainStack,
		nullptr,
		ChunkResolution,
		Material,
		SkirtDepth);
}

void AASolarOrbzChunkedPlanetActor::BeginPlay()
{
	Super::BeginPlay();

	ConstructResidentSetManager();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			UpdateTimerHandle,
			this,
			&AASolarOrbzChunkedPlanetActor::UpdateChunks,
			UpdateIntervalSeconds,
			/*bLoop=*/true,
			/*FirstDelay=*/0.0f);
	}
}

void AASolarOrbzChunkedPlanetActor::EndPlay(EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(UpdateTimerHandle);
	}

	// Destroys every still-resident component before this actor itself goes away - see
	// FSolarOrbzChunkResidentSetManager's own destructor/ClearAll comment.
	ResidentSetManager.Reset();

	Super::EndPlay(EndPlayReason);
}

FVector AASolarOrbzChunkedPlanetActor::ResolveViewerWorldPosition() const
{
	if (IsValid(ViewerActor))
	{
		return ViewerActor->GetActorLocation() - GetActorLocation();
	}

	if (bUseViewerWorldPositionOverride)
	{
		return ViewerWorldPositionOverride;
	}

	// Degenerate but well-defined fallback - see this class's own header comment on why "viewer at
	// the planet's center" is a sane, non-crashing default rather than undefined behavior. Logged
	// once per play session, not every update tick.
	if (!bHasLoggedNoViewerWarning)
	{
		UE_LOG(LogSolarOrbzChunkedPlanet, Warning,
			TEXT("SolarOrbz ChunkedPlanet: '%s' has no ViewerActor and bUseViewerWorldPositionOverride is false - falling back to treating the viewer as sitting at this planet's own center, which produces a shallow, roughly-uniform desired depth across the whole planet rather than meaningful LOD. Set ViewerActor or ViewerWorldPositionOverride to get real streaming behavior."),
			*GetName());
		bHasLoggedNoViewerWarning = true;
	}

	return FVector::ZeroVector;
}

void AASolarOrbzChunkedPlanetActor::UpdateChunks()
{
	if (!ResidentSetManager.IsValid())
	{
		return;
	}

	const FVector ViewerWorldPosition = ResolveViewerWorldPosition();
	ResidentSetManager->UpdateResidentSet(ViewerWorldPosition, LODSettings);
}

void AASolarOrbzChunkedPlanetActor::UpdateChunksNow()
{
	if (!ResidentSetManager.IsValid())
	{
		// Lazily construct here too - this is the whole fix for CallInEditor silently no-op'ing on
		// an actor that has never been through BeginPlay (i.e. always, outside PIE/a packaged game).
		// Only the FIRST call pays this cost; after that ResidentSetManager already exists and this
		// branch never runs again until RebuildChunkedPlanetNow() tears it back down.
		ConstructResidentSetManager();
	}

	UpdateChunks();
}

void AASolarOrbzChunkedPlanetActor::RebuildChunkedPlanetNow()
{
	// Destroys every currently-resident chunk component (via ResidentSetManager's own destructor) -
	// necessary because the manager has no setters for Radius/TerrainStack/ChunkResolution/Material/
	// SkirtDepth, so simply calling UpdateChunksNow() on an already-constructed manager would keep
	// using whatever values it was built with, ignoring anything changed on this actor since then.
	ResidentSetManager.Reset();
	ConstructResidentSetManager();
	UpdateChunks();
}

int32 AASolarOrbzChunkedPlanetActor::GetResidentChunkCount() const
{
	return ResidentSetManager.IsValid() ? ResidentSetManager->GetResidentComponents().Num() : 0;
}
