// SolarOrbz - Whole-planet hydrology simulation. Runs once per RegenerateMesh (not per-vertex), on
// its own independent lat/long grid, same pattern as USolarOrbzClimateSimulationAsset - sampling
// elevation from the same TerrainLayerStack the mesh uses, after Climate Simulation has run (so it
// can optionally weight rainfall by real simulated Moisture instead of a flat per-cell amount).
//
// Produces FlowAccumulation01: how much water drains THROUGH each cell on its way downhill, derived
// via D8 (8-neighbor steepest-descent) flow routing - the same deterministic, non-droplet-based
// technique USolarOrbzErosionTerrainLayer's hydraulic pass already uses to carve valleys, just
// accumulating a flow volume per cell instead of carving height. This is the standard GIS "flow
// accumulation" technique real-world DEM analysis uses to algorithmically extract river networks:
// thresholding a flow accumulation grid (a few connected cells with enormous upstream drainage area)
// naturally produces thin, branching, realistic-looking channels - not something that needs a
// separate hand-authored path/width per river.
//
// Feeds FSolarOrbzBiomeSampleContext for USolarOrbzRiversBiomeMask to read (SolarOrbzBiomeSystem.h) -
// see that class's own comment for why it's Pass-B-only (a Biome's own Mask), same documented
// limitation USolarOrbzFilterChangeBiomeMask has in the other direction (Pass-A-only).

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SolarOrbzHydrologySimulation.generated.h"

class USolarOrbzTerrainLayerStack;
struct FSolarOrbzClimateGrid;

/**
 * Baked simulation result: one equirectangular grid, sampled with bilinear interpolation and
 * longitude wraparound - same UV convention as FSolarOrbzClimateGrid/FSolarOrbzIcoSphereMeshData.
 */
struct SOLARORBZ_API FSolarOrbzHydrologyGrid
{
	int32 Width = 0;
	int32 Height = 0;

	/**
	 * 0 = a ridge/divide cell that only ever sees its own rainfall, 1 = the single cell on this
	 * planet with the largest upstream drainage area (a major river mouth/outlet). Log-compressed
	 * then min-max normalized across the whole grid (see USolarOrbzHydrologySimulationAsset::
	 * Simulate) - raw accumulated flow spans many orders of magnitude between a headwater cell and a
	 * continental river mouth, so without compression almost the entire land surface would read as
	 * indistinguishable near-zero next to the few largest outlets.
	 */
	TArray<float> FlowAccumulation01;

	bool IsValid() const
	{
		return Width > 0 && Height > 0 && FlowAccumulation01.Num() == Width * Height;
	}

	void Reset()
	{
		Width = 0;
		Height = 0;
		FlowAccumulation01.Reset();
	}

	/** Bilinear-samples FlowAccumulation01 at a point on the unit sphere, wrapping across the longitude seam. */
	float Sample(const FVector& UnitDirection) const;
};

/**
 * Author this once and reference it from as many SolarOrbz IcoSphere actors as you like - same
 * pattern as USolarOrbzTerrainLayerStack/USolarOrbzClimateSimulationAsset. Assign it to an actor's
 * HydrologySimulation property; it runs automatically as part of RegenerateMesh, after the base
 * terrain stack and (if assigned) Climate Simulation.
 */
UCLASS(BlueprintType)
class SOLARORBZ_API USolarOrbzHydrologySimulationAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Simulation grid resolution, longitude axis. Independent of mesh density and of Climate/Erosion's own grids - this is a separate, independently-sized grid. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Hydrology|Grid", meta = (ClampMin = "8"))
	int32 GridWidth = 256;

	/** Simulation grid resolution, latitude axis. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Hydrology|Grid", meta = (ClampMin = "4"))
	int32 GridHeight = 128;

	/** Elevation (meters, relative to base radius) below which a cell is treated as ocean - a terminal sink that absorbs whatever flow reaches it without routing further (same role Sea Level plays for Climate Simulation/Erosion). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Hydrology", meta = (ClampMin = "-200000.0", ClampMax = "200000.0", Units = "m"))
	float SeaLevel = 0.0f;

	/**
	 * Flat per-cell rainfall contribution used when no ClimateGrid is passed to Simulate() (or no
	 * ClimateSimulation is assigned on the actor) - every cell starts by contributing this much flow
	 * before routing downhill. Purely relative (only ratios between cells matter, since the whole
	 * grid gets min-max normalized afterward), so the absolute value rarely needs tuning.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Hydrology", meta = (ClampMin = "0.0"))
	float BaseRainfallAmount = 1.0f;

	/**
	 * Runs D8 flow accumulation and fills OutGrid. TerrainStack may be null (flat sea-level planet,
	 * every cell then ties and nothing routes anywhere - useful for isolating a config problem, not
	 * a meaningful result).
	 * @param ClimateGridForRainfall  Optional. When valid, each cell's rainfall contribution is
	 *        BaseRainfallAmount scaled by that grid's own Moisture01 at the same point (wetter areas
	 *        genuinely drain more, rather than every cell contributing identically) - addresses the
	 *        same "not yet driven by real rainfall" gap USolarOrbzErosionTerrainLayer::RainfallAmount
	 *        still has. Null falls back to a flat BaseRainfallAmount everywhere.
	 */
	void Simulate(const USolarOrbzTerrainLayerStack* TerrainStack, double RadiusCm, const FSolarOrbzClimateGrid* ClimateGridForRainfall, FSolarOrbzHydrologyGrid& OutGrid) const;
};
