// Portable port of the UE plugin's USolarOrbzContinentTerrainLayer (SolarOrbzTerrainLayers.h/.cpp) -
// all three algorithms (RadialSeeds/VoronoiGrowth/PlateTectonics) plus the full tectonic simulation
// (persistent plates, mountain-building stress, plate splitting). Scope cut from the real plugin,
// documented here and in the tool's README: no Profile/ClimateSimulation integration (this tool has
// no such concepts - Num Continents/Islands/Sea Level are plain config values, not resolved from an
// external asset), and RNG is NOT bit-identical to UE's FRandomStream (see Random.h).
#pragma once

#include "Vec3.h"
#include "NoiseBasis.h"
#include <vector>
#include <cstdint>

struct FContinentSeedData
{
    Vec3 Direction = Vec3::Up();
    double RadiusRadians = 0.0;
    Vec3 NoiseOffset = Vec3::Zero();
};

/** Port of FSolarOrbzTectonicPlate. */
struct FTectonicPlate
{
    int32_t PlateId = -1;
    Vec3 SeedDirection = Vec3::Up();
    Vec3 RotationAxis = Vec3::Up();
    float AngularRateRadPerMyr = 0.0f;
    bool bOceanic = false;
    float AgeMyr = 0.0f;

    Vec3 GetInstantaneousDrift() const
    {
        return Vec3::Cross(RotationAxis, SeedDirection) * AngularRateRadPerMyr;
    }
};

/** Port of FSolarOrbzTectonicBoundaryState. */
struct FTectonicBoundaryState
{
    int32_t PlateIdA = -1;
    int32_t PlateIdB = -1;
    float AccumulatedConvergenceMyr = 0.0f;
    float AccumulatedDivergenceMyr = 0.0f;
};

/** Every authored property USolarOrbzContinentTerrainLayer exposes, mirrored 1:1 by name so this reads like the real plugin. Defaults match the plugin's own defaults. */
struct FContinentConfig
{
    int32_t Seed = 0;
    EContinentAlgorithm Algorithm = EContinentAlgorithm::RadialSeeds;

    int32_t NumContinents = 5;
    float MinContinentRadiusDegrees = 15.0f;
    float MaxContinentRadiusDegrees = 35.0f;
    int32_t NumIslands = 10;
    float MinIslandRadiusDegrees = 1.0f;
    float MaxIslandRadiusDegrees = 5.0f;

    bool bHasNorthPolarContinent = false;
    bool bHasSouthPolarContinent = false;
    float PolarContinentRadiusDegrees = 20.0f;

    // Radial Seeds shape
    float CoastlineWarpStrength = 0.6f;
    float CoastlineWarpFrequency = 1.2f;
    int32_t CoastlineNoiseOctaves = 3;
    float CoastlineNoisePersistence = 0.5f;
    float CoastlineNoiseLacunarity = 2.0f;
    int32_t SubSeedsPerLandmass = 2;
    float SubSeedScatterFraction = 0.7f;
    float MinSubSeedRadiusFraction = 0.3f;
    float MaxSubSeedRadiusFraction = 0.7f;

    // Voronoi Growth / Plate Tectonics bake grid
    int32_t BakeGridWidth = 256;
    int32_t BakeGridHeight = 128;

    // Voronoi Growth
    float GrowthDecayMin = 0.90f;
    float GrowthDecayMax = 0.985f;
    float GrowthJitter = 0.35f;

    // Plate Tectonics
    int32_t NumPlates = 12;
    float OceanicPlateFraction = 0.55f;
    float BoundaryInfluenceDegrees = 8.0f;
    float MountainHeightMeters = 3000.0f;
    float TrenchDepthMeters = -3000.0f;
    float RidgeHeightMeters = 600.0f;
    float RiftDepthMeters = -500.0f;
    float InitialDriftSpeedDegreesPerMyr = 2.0f;
    int32_t StepsPerRun = 10;
    float MillionYearsPerStep = 5.0f;
    float StressMatureTimescaleMyr = 50.0f;
    float MaxStressMultiplier = 1.0f;
    float PlateSplitThresholdMyr = 200.0f;

    // Coastline texture (shared)
    float CoastlineNoiseAmplitude = 0.3f;
    float CoastlineNoiseFrequency = 3.0f;
    float CoastlineSharpness = 1.5f;
    float OceanFloorDepthMeters = -4000.0f;
    float LandPlateauHeightMeters = 200.0f;
};

/** Read-only, for the GUI/CLI to report back to the user - mirrors the plugin's own read-only properties. */
struct FContinentStatus
{
    float SimulatedAgeMyr = 0.0f;
    int32_t CurrentPlateCount = 0;
};

/** Port of USolarOrbzContinentTerrainLayer. One instance per "planet" being previewed. */
class ContinentLayer
{
public:
    FContinentConfig Config;

    /** Equivalent of Bake() - (re)computes CachedSeeds or BakedHeightCm depending on Algorithm. Call after changing Config, before GetRawHeight(). */
    void Bake();

    /** Height in meters above/below sea level at a point on the unit sphere - equivalent of GetRawHeight() (already meters, not cm - this tool has no UE-units convention to match). */
    float GetRawHeightMeters(const Vec3& UnitDirection) const;

    /** Equivalent of AdvanceTectonicSimulation() - only meaningful when Algorithm == PlateTectonics. */
    void AdvanceTectonicSimulation(int32_t NumSteps, float DeltaMyr);

    FContinentStatus GetStatus() const { return Status; }

    /** Diagnostic sample, matching the UE plugin's own log line shape - land coverage % at a small independent grid. */
    float EstimateLandCoveragePercent() const;

private:
    void BakeRadialSeeds();
    void BakeVoronoiGrowth();
    void BakePlateTectonics();

    float GetRawHeightRadialSeeds(const Vec3& UnitDirection) const;
    float GetRawHeightFromBakedGrid(const Vec3& UnitDirection) const;

    void EnsurePlatesInitialized();
    void UpdateBoundaryStress(float DeltaMyr);
    void SplitPlate(int32_t PlateIdToSplit, int32_t OtherPlateId);

    // "Closure" (two plates' instantaneous drift, each projected onto the boundary axis and
    // subtracted) classifies a boundary convergent/divergent/transform, but its magnitude is bounded
    // by the plates' own drift speed - GetInstantaneousDrift() is Cross(axis, dir) * AngularRate, so
    // |Closure| can never exceed roughly 2x the fastest single plate's drift magnitude, which itself
    // scales directly with Config.InitialDriftSpeedDegreesPerMyr (radians). A threshold with no
    // relationship to that speed is either permanently unreachable (every boundary reads as
    // "transform," contributing zero height, no matter how long the simulation runs - confirmed via
    // direct instrumentation: at the 2.0 deg/Myr default, |Closure| samples cluster around 0.005-0.03,
    // nowhere near a hardcoded 0.25) or, at the opposite extreme, so small that EVERY boundary reads
    // as convergent/divergent regardless of the plates' actual relative motion. Scaling it to the
    // configured drift speed keeps it meaningful at any InitialDriftSpeedDegreesPerMyr value.
    double ComputeTransformThreshold() const
    {
        const double MaxDriftRad = MathUtil::DegreesToRadians((double)Config.InitialDriftSpeedDegreesPerMyr);
        return MaxDriftRad * 0.3;
    }
    FTectonicBoundaryState& FindOrAddBoundaryState(int32_t PlateIdA, int32_t PlateIdB);
    const FTectonicBoundaryState* FindBoundaryState(int32_t PlateIdA, int32_t PlateIdB) const;

    std::vector<FContinentSeedData> CachedSeeds;

    std::vector<float> BakedHeightCm; // kept in cm (like the plugin) purely so the ported math/constants transfer verbatim; GetRawHeightMeters() divides by 100 at the boundary
    int32_t BakedGridWidth = 0;
    int32_t BakedGridHeight = 0;

    std::vector<FTectonicPlate> Plates;
    std::vector<FTectonicBoundaryState> BoundaryStates;
    int32_t NextPlateId = 0;
    int32_t PlatesSeededFrom = 0;
    int32_t PlatesSeededFromNumPlates = 0;
    bool bPlatesEverSeeded = false;

    FContinentStatus Status;
};
