// See SolarOrbzChunkedPlanetActor.h for the full picture, viewer-resolution fallback order, and
// what is/isn't wired up yet.

#include "SolarOrbzChunkedPlanetActor.h"
#include "SolarOrbzChunkResidentSetManager.h"
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

void AASolarOrbzChunkedPlanetActor::BeginPlay()
{
	Super::BeginPlay();

	const double RadiusCm = RadiusMeters * 100.0;

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
	UpdateChunks();
}

int32 AASolarOrbzChunkedPlanetActor::GetResidentChunkCount() const
{
	return ResidentSetManager.IsValid() ? ResidentSetManager->GetResidentComponents().Num() : 0;
}
