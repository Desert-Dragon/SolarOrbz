// SolarOrbz - Whole-planet climate simulation. Runs once per RegenerateMesh
// (not per-vertex) on an independent lat/long grid, sampling elevation from
// the same TerrainLayerStack used for the mesh. Produces:
//
//   - Temperature: latitude baseline minus an elevation lapse rate.
//   - Moisture:    wind advected around each latitude band, picking up
//                   moisture over ocean cells and raining it out over land,
//                   with extra rainfall on uphill slopes (orographic effect,
//                   i.e. real rain shadows behind mountain ranges).
//
// Both fields land in FSolarOrbzBiomeSampleContext for ClimateBiomeMask to
// read, replacing its old noise-based moisture stand-in.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SolarOrbzClimateSimulation.generated.h"

class USolarOrbzTerrainLayerStack;

/**
 * Baked simulation result: two equirectangular grids (temperature, moisture),
 * both 0..1, sampled with bilinear interpolation and longitude wraparound.
 * Uses the exact same UV convention as FSolarOrbzIcoSphereMeshData (U =
 * longitude/azimuth wrapping 0..1, V = polar angle 0 at north pole .. 1 at
 * south pole) so a grid cell's UnitDirection maps onto the same TerrainStack
 * evaluation a mesh vertex there would get.
 */
struct SOLARORBZ_API FSolarOrbzClimateGrid
{
	int32 Width = 0;
	int32 Height = 0;

	TArray<float> Temperature01;
	TArray<float> Moisture01;

	bool IsValid() const
	{
		return Width > 0 && Height > 0
			&& Temperature01.Num() == Width * Height
			&& Moisture01.Num() == Width * Height;
	}

	void Reset()
	{
		Width = 0;
		Height = 0;
		Temperature01.Reset();
		Moisture01.Reset();
	}

	/** Bilinear-samples both fields at a point on the unit sphere, wrapping across the longitude seam. */
	void Sample(const FVector& UnitDirection, float& OutTemperature01, float& OutMoisture01) const;
};

/**
 * Author this once and reference it from as many SolarOrbz IcoSphere actors as you like -
 * same pattern as USolarOrbzTerrainLayerStack. Assign it to an actor's ClimateSimulation
 * property; it runs automatically as part of RegenerateMesh, after the base terrain stack.
 */
UCLASS(BlueprintType)
class SOLARORBZ_API USolarOrbzClimateSimulationAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Simulation grid resolution, longitude axis. Independent of mesh density - this is a separate, coarser grid. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Grid", meta = (ClampMin = "8"))
	int32 GridWidth = 256;

	/** Simulation grid resolution, latitude axis. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Grid", meta = (ClampMin = "4"))
	int32 GridHeight = 128;

	/** Normalized temperature (0..1) at the equator, at sea level. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Temperature", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float EquatorTemperature = 1.0f;

	/** Normalized temperature (0..1) at the poles, at sea level. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Temperature", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PoleTemperature = 0.0f;

	/** How much normalized temperature drops per km of elevation above sea level. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Temperature", meta = (ClampMin = "0.0"))
	float LapseRatePerKm = 0.15f;

	/** Elevation (cm, relative to base radius) below which a cell counts as ocean - a moisture source and a warmth-moderated zone. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate")
	float SeaLevel = 0.0f;

	/**
	 * Simplified three-cell atmospheric circulation, mirrored across both hemispheres by absolute
	 * latitude: tropical easterlies (0..TradeWindEdgeDegrees) blow east-to-west, mid-latitude
	 * westerlies (TradeWindEdgeDegrees..WesterliesEdgeDegrees) blow west-to-east, and polar
	 * easterlies (WesterliesEdgeDegrees..90) blow east-to-west again - the same three-band pattern
	 * that gives Earth its trade winds, prevailing westerlies, and polar easterlies. This is what
	 * makes a temperate west coast wet and its east side a rain shadow, while the reverse can be
	 * true in the tropics.
	 */

	/** Latitude (degrees from the equator) where the tropical easterlies give way to the mid-latitude westerlies. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Wind", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float TradeWindEdgeDegrees = 30.0f;

	/** Latitude (degrees from the equator) where the mid-latitude westerlies give way to the polar easterlies. Should be greater than TradeWindEdgeDegrees. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Wind", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float WesterliesEdgeDegrees = 60.0f;

	/** Flips every band's direction at once - for a planet with retrograde rotation, without having to re-tune each band by hand. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Wind")
	bool bReverseAllBands = false;

	/**
	 * How many full circuits the wind simulation makes around each latitude band before the
	 * final circuit is recorded. Because the sphere wraps, moisture carried needs a few loops
	 * to settle into a repeating steady state - otherwise there's an arbitrary seam at longitude 0.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Wind", meta = (ClampMin = "1"))
	int32 WindLoops = 2;

	/** Maximum moisture the simulated air can carry at once. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Wind", meta = (ClampMin = "0.0"))
	float MoistureCapacity = 1.0f;

	/** Moisture gained per grid step while passing over an ocean cell. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Wind", meta = (ClampMin = "0.0"))
	float EvaporationRate = 0.15f;

	/** Fraction of carried moisture that always rains out over land each step, even on flat ground - keeps moisture from crossing an entire flat continent undiminished. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Wind", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BaseRainfallRate = 0.05f;

	/** Extra rainfall per km of uphill elevation gain along the wind direction - the orographic effect that produces rain shadows on the far side of mountain ranges. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Climate|Wind", meta = (ClampMin = "0.0"))
	float OrographicRainfallFactor = 2.0f;

	/** Runs the simulation and fills OutGrid. TerrainStack may be null (flat sea-level planet - useful for testing wind/temperature settings in isolation). */
	void Simulate(const USolarOrbzTerrainLayerStack* TerrainStack, float RadiusCm, FSolarOrbzClimateGrid& OutGrid) const;
};
