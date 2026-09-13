// SolarOrbz - Erosion terrain layer. Unlike Noise/Heightmap/Stamp layers, this one needs the
// combined height of every layer below it across the WHOLE planet (not just a single point) to
// simulate material moving downhill - so it bakes a delta-height grid once per regenerate
// (see USolarOrbzTerrainLayer::Bake) and bilinear-samples that grid per-vertex afterward, exactly
// like USolarOrbzClimateSimulationAsset does for climate.
//
// Two passes, matching classic terrain-erosion tooling:
//   - Thermal erosion: loose material above a talus angle slides downhill each iteration until
//     slopes stabilize - turns sharp noise-generated cliffs into believable scree slopes.
//   - Hydraulic erosion: water is routed downhill (steepest-descent, 8-neighbor) across the whole
//     grid each pass, carving material where flow is strong and depositing it where flow stalls -
//     carves valleys and drainage networks instead of leaving noise looking uniformly bumpy. This
//     is a deterministic grid-based flow-routing model, not particle/droplet-based - chosen so a
//     full spherical bake is one pass rather than tracking thousands of random-walking droplets
//     across a wrapped grid.
//
// Both operate on an independent lat/long grid (GridWidth/GridHeight), decoupled from mesh density.
//
// Known simplification: slope/talus math uses a single equatorial cell-spacing estimate uniformly
// across the whole grid, rather than correcting for longitude cells shrinking toward the poles -
// erosion is therefore slightly less physically accurate near the poles than the equator.

#pragma once

#include "CoreMinimal.h"
#include "SolarOrbzTerrainLayer.h"
#include "SolarOrbzErosionTerrainLayer.generated.h"

/**
 * Simulates erosion on top of whatever layers sit below it in the stack - place it after your
 * Noise/Heightmap layers so it has real terrain to erode. Its own output is a height DELTA (usually
 * negative in carved valleys, occasionally positive where sediment deposits), so leave Blend Mode
 * at the default Add. Weight (inherited) scales the overall erosion intensity without re-baking.
 */
UCLASS(EditInlineNew, meta = (DisplayName = "Erosion Layer"))
class SOLARORBZ_API USolarOrbzErosionTerrainLayer : public USolarOrbzTerrainLayer
{
	GENERATED_BODY()

public:
	/** Bake grid resolution, longitude axis. Independent of mesh density - higher gives finer drainage detail at the cost of a slower bake. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Erosion|Grid", meta = (ClampMin = "8"))
	int32 GridWidth = 256;

	/** Bake grid resolution, latitude axis. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Erosion|Grid", meta = (ClampMin = "4"))
	int32 GridHeight = 128;

	// --- Thermal erosion ---

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Erosion|Thermal")
	bool bEnableThermalErosion = true;

	/** Maximum stable slope, degrees from horizontal. Loose scree/sand settles around 30-35 degrees; bare rock can hold much steeper. Anything steeper redistributes toward the lowest neighbor each iteration. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Erosion|Thermal", meta = (ClampMin = "1.0", ClampMax = "89.0", EditCondition = "bEnableThermalErosion"))
	float TalusAngleDegrees = 33.0f;

	/** How many relaxation passes to run. Most of the visible change happens in the first several; more iterations let slopes settle further toward the talus angle. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Erosion|Thermal", meta = (ClampMin = "0", EditCondition = "bEnableThermalErosion"))
	int32 ThermalIterations = 20;

	/** Fraction of the excess-over-talus height moved to the lowest neighbor per iteration. Low = gradual slumping; high = aggressive and can overshoot. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Erosion|Thermal", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bEnableThermalErosion"))
	float ThermalStrength = 0.5f;

	// --- Hydraulic erosion ---

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Erosion|Hydraulic")
	bool bEnableHydraulicErosion = true;

	/** How many full downhill-routing passes to run. Each pass can deepen existing channels and lets water re-route across terrain reshaped by the previous pass. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Erosion|Hydraulic", meta = (ClampMin = "0", EditCondition = "bEnableHydraulicErosion"))
	int32 HydraulicIterations = 4;

	/** Water added at every cell at the start of each pass, before routing downhill. Uniform across the whole planet for now - not yet driven by a ClimateSimulation's real rainfall. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Erosion|Hydraulic", meta = (ClampMin = "0.0", EditCondition = "bEnableHydraulicErosion"))
	float RainfallAmount = 0.02f;

	/** How readily flowing water carves material - higher cuts deeper channels for the same flow and slope. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Erosion|Hydraulic", meta = (ClampMin = "0.0", EditCondition = "bEnableHydraulicErosion"))
	float ErosionRate = 0.3f;

	/** How readily water drops carried sediment once it can no longer carry it (e.g. reaching a local basin) - higher piles sediment up faster in low ground. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Erosion|Hydraulic", meta = (ClampMin = "0.0", EditCondition = "bEnableHydraulicErosion"))
	float DepositionRate = 0.3f;

	//~ Begin USolarOrbzTerrainLayer interface
	virtual bool RequiresWholeSurfaceBake() const override { return true; }
	virtual void Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, float RadiusCm) override;
	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
	//~ End USolarOrbzTerrainLayer interface

private:
	int32 BakedWidth = 0;
	int32 BakedHeight = 0;

	/** Height AFTER erosion minus height BEFORE erosion, per cell - this is what GetRawHeight samples, since this layer's contribution is a delta on top of the layers it eroded. */
	TArray<float> BakedDeltaHeightCm;
};
