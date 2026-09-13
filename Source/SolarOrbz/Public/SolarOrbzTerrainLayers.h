// SolarOrbz - Terrain Layers subsystem. Combines the layer base class/blend-mode enum, the
// ordered layer stack, and every concrete layer type (Noise, Planetary Noise, Heightmap, Stamp,
// Erosion) into one file. Grouped together because they form a single tightly-coupled inheritance
// hierarchy - USolarOrbzTerrainLayerStack::EvaluateHeight walks an array of USolarOrbzTerrainLayer
// pointers, and every concrete class below exists purely to be an entry in that array.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Templates/Function.h"
#include "Engine/DataAsset.h"
#include "SolarOrbzIcoSphere.h"
#include "SolarOrbzTerrainLayers.generated.h"

class UTexture2D;

// ================================================================================================
// ESolarOrbzTerrainBlendMode / USolarOrbzTerrainLayer - the base class every layer below derives
// from. Mirrors the "layer stack" approach used by tools like World Machine / World Creator: each
// layer is either procedural (noise) or authored (heightmap/stamp), and a stack of them composites
// top-to-bottom via blend modes.
// ================================================================================================
UENUM(BlueprintType)
enum class ESolarOrbzTerrainBlendMode : uint8
{
	Add,
	Subtract,
	Multiply,
	Max,
	Min,
	Replace,
};

UCLASS(Abstract, EditInlineNew, DefaultToInstanced, BlueprintType)
class SOLARORBZ_API USolarOrbzTerrainLayer : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Layer")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Layer")
	ESolarOrbzTerrainBlendMode BlendMode = ESolarOrbzTerrainBlendMode::Add;

	/** Multiplies this layer's raw output before blending - the simplest way to fade a layer in/out. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Layer")
	float Weight = 1.0f;

	/**
	 * Most layers (noise, heightmap, stamp) are pure functions of a single point - GetRawHeight
	 * needs nothing but the point itself. Erosion is different: carving a valley requires knowing
	 * about the terrain around a point, not just the point. Layers that need this override this to
	 * return true and implement Bake(), which the stack calls once per regenerate, before any
	 * per-vertex GetRawHeight calls, giving them a chance to build whatever whole-surface data they need.
	 */
	virtual bool RequiresWholeSurfaceBake() const { return false; }

	/**
	 * Called once per regenerate, before GetRawHeight is ever called for this layer, only when
	 * RequiresWholeSurfaceBake() returns true. PriorLayersHeight evaluates every layer below this
	 * one in the stack (not including this layer) at an arbitrary point - i.e. exactly the terrain
	 * this layer should treat as its starting point to erode, stamp around, etc.
	 * @param PriorLayersHeight  Callable: (UnitDirection, UV) -> combined height of layers below this one, in cm.
	 * @param RadiusCm           The planet's base radius, for layers that need real physical distances (e.g. slope).
	 */
	virtual void Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, float RadiusCm) {}

	/**
	 * Returns this layer's height contribution, in UE units (cm), at a point on the unit sphere.
	 * @param UnitDirection  Normalized direction from the planet center (position on a unit sphere).
	 * @param UV             The mesh's spherical UV at this point (matches FSolarOrbzIcoSphereMeshData::UVs).
	 */
	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const { return 0.0f; }
};

// ================================================================================================
// USolarOrbzTerrainLayerStack - an ordered, saveable recipe combining any of the concrete layers
// below into one planet's terrain.
// ================================================================================================

/**
 * Author this once and reference it from as many SolarOrbz IcoSphere actors as you like -
 * e.g. one stack for "Earthlike", one for "Mars", one for a procedural gas giant moon.
 */
UCLASS(BlueprintType)
class SOLARORBZ_API USolarOrbzTerrainLayerStack : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Instanced, Category = "SolarOrbz|Terrain")
	TArray<TObjectPtr<USolarOrbzTerrainLayer>> Layers;

	/**
	 * Call once per regenerate, before any EvaluateHeight calls (including from a ClimateSimulation
	 * sampling this same stack) - gives layers that need whole-surface data (e.g. erosion) a chance
	 * to bake it. No-op for layers that don't override RequiresWholeSurfaceBake().
	 */
	void PrepareLayers(float RadiusCm) const;

	/** Evaluates every enabled layer in order and returns the combined height, in UE units (cm). */
	float EvaluateHeight(const FVector& UnitDirection, const FVector2D& UV) const;

	/** Same as EvaluateHeight, but only accumulates layers with index < EndIndexExclusive - i.e. what a layer at that index would see as "everything below it". */
	float EvaluateHeightUpTo(int32 EndIndexExclusive, const FVector& UnitDirection, const FVector2D& UV) const;
};

// ================================================================================================
// USolarOrbzFractalNoiseTerrainLayerBase - shared fractal (multi-octave) Perlin noise machinery.
// Concrete layers (Noise, Planetary Noise below) share this exact noise field, domain warp, and
// seeding, and only differ in how the normalized -1..1 result gets scaled into an actual height.
// ================================================================================================
UCLASS(Abstract, EditInlineNew)
class SOLARORBZ_API USolarOrbzFractalNoiseTerrainLayerBase : public USolarOrbzTerrainLayer
{
	GENERATED_BODY()

public:
	/** Randomizes the noise pattern without changing its statistical character. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise")
	int32 Seed = 0;

	/** Number of fractal octaves summed together - more octaves add finer detail at increasing cost. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "1", ClampMax = "10"))
	int32 Octaves = 5;

	/**
	 * Base frequency, roughly "noise cycles per trip around the sphere". Small values (0.5-3) give
	 * continent-scale features; large values (20+) give fine surface roughness.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "0.01"))
	float Frequency = 2.0f;

	/** Frequency multiplier applied each octave. >1 makes each successive octave finer. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "1.0"))
	float Lacunarity = 2.0f;

	/** Amplitude multiplier applied each octave. <1 makes finer octaves contribute progressively less. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Persistence = 0.5f;

	/** Domain warp strength - pushes the sample position through a second noise field first, breaking up regular noise "grid" patterning. 0 disables it. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise|Warp", meta = (ClampMin = "0.0"))
	float WarpStrength = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise|Warp", meta = (ClampMin = "0.01", EditCondition = "WarpStrength > 0.0"))
	float WarpFrequency = 1.0f;

protected:
	/**
	 * Fractal Perlin sum with seeding and optional domain warp applied, normalized to roughly -1..1.
	 * Subclasses scale this into an actual height however makes sense for them (raw amplitude,
	 * sea-level-relative meters, etc).
	 */
	float ComputeNormalizedNoise(const FVector& UnitDirection) const;
};

// ================================================================================================
// USolarOrbzNoiseTerrainLayer - scaled by a raw amplitude in meters. Good for small or irregular
// bodies (asteroids, tiny moons) where "meters above sea level" isn't a meaningful concept - just
// dial in a height range directly. For planet-scale terrain where you want elevation to mean the
// same thing regardless of Radius, use Planetary Noise Layer instead.
// ================================================================================================
UCLASS(EditInlineNew, meta = (DisplayName = "Noise Layer"))
class SOLARORBZ_API USolarOrbzNoiseTerrainLayer : public USolarOrbzFractalNoiseTerrainLayerBase
{
	GENERATED_BODY()

public:
	/** Final height scale, in meters - the peak-to-peak range the fractal sum is mapped onto. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "0.0", ClampMax = "1000000.0", Units = "m"))
	float AmplitudeMeters = 5.0f;

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
};

// ================================================================================================
// USolarOrbzPlanetaryNoiseTerrainLayer - scaled by real-world meters above/below sea level rather
// than a raw amplitude - the same value means the same thing whether your planet is Earth-scale or
// a 50m test sphere, so changing Radius never requires re-tuning this layer. For small or irregular
// bodies where "sea level" isn't a meaningful concept (asteroids, tiny moons), use Noise Layer instead.
// ================================================================================================
UCLASS(EditInlineNew, meta = (DisplayName = "Planetary Noise Layer"))
class SOLARORBZ_API USolarOrbzPlanetaryNoiseTerrainLayer : public USolarOrbzFractalNoiseTerrainLayerBase
{
	GENERATED_BODY()

public:
	/**
	 * How high above sea level (meters) this layer's tallest peaks should reach - authored in real,
	 * planet-scale-independent meters rather than a raw amplitude. "Sea level" here is the same zero
	 * point ClimateSimulation's Sea Level measures from: the planet's base radius. Reference: Earth's
	 * Everest is ~8,850m.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "0.0", ClampMax = "200000.0", Units = "m"))
	float MaxElevationMeters = 8000.0f;

	/**
	 * How far below sea level (meters, entered as a positive depth) this layer's lowest points should
	 * reach. Reference: Earth's Mariana Trench is ~10,900m deep; average ocean depth is closer to 3,700m.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "0.0", ClampMax = "200000.0", Units = "m"))
	float MaxDepthMeters = 4000.0f;

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
};

// ================================================================================================
// USolarOrbzHeightmapTerrainLayer - authored heightmap layer. Samples an equirectangular heightmap
// texture (e.g. a real-world DEM for Earth or Mars) using the mesh's spherical UV.
// ================================================================================================

/**
 * Editor-only: reads the texture's source pixel data directly rather than sampling the GPU
 * resource, so it works regardless of compression settings. Import with compression set to
 * None so full precision survives.
 */
UCLASS(EditInlineNew, meta = (DisplayName = "Heightmap Layer"))
class SOLARORBZ_API USolarOrbzHeightmapTerrainLayer : public USolarOrbzTerrainLayer
{
	GENERATED_BODY()

public:
	/** Equirectangular (lat/long) heightmap - X = longitude, Y = latitude (north pole at top). Grayscale; 16-bit or float source recommended for real-world DEMs. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Heightmap")
	TObjectPtr<UTexture2D> HeightmapTexture;

	/** World-space height (meters) that black (0.0) in the heightmap maps to. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Heightmap", meta = (ClampMin = "-200000.0", ClampMax = "200000.0", Units = "m"))
	float MinHeightMeters = -11000.0f; // Mariana Trench

	/** World-space height (meters) that white (1.0) in the heightmap maps to. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Heightmap", meta = (ClampMin = "-200000.0", ClampMax = "200000.0", Units = "m"))
	float MaxHeightMeters = 8848.0f; // Everest

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;

private:
	mutable FSolarOrbzTextureHeightSampler Sampler;
};

// ================================================================================================
// USolarOrbzStampTerrainLayer - places a single feature (a heightmap texture, or a procedural
// dome/crater) at a specific point on the sphere, with an angular radius and soft edge falloff.
// Mirrors World Creator's terrain stamping - use several of these for individual mountain ranges,
// craters, or impact basins instead of covering the whole globe with noise.
// ================================================================================================
UCLASS(EditInlineNew, meta = (DisplayName = "Stamp Layer"))
class SOLARORBZ_API USolarOrbzStampTerrainLayer : public USolarOrbzTerrainLayer
{
	GENERATED_BODY()

public:
	/** Where the stamp is centered, in degrees. Latitude: -90 (south pole) .. 90 (north pole). Longitude: -180 .. 180. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "-90.0", ClampMax = "90.0"))
	float Latitude = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "-180.0", ClampMax = "180.0"))
	float Longitude = 0.0f;

	/** How far the stamp reaches from its center, in degrees of arc along the sphere's surface. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "0.1", ClampMax = "180.0"))
	float AngularRadius = 15.0f;

	/** Fraction of AngularRadius (0..1) over which the stamp fades out at its edge, so it blends instead of cliffing off. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float EdgeFalloff = 0.25f;

	/** Rotates the stamp around its own center - only matters with a heightmap assigned. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "-180.0", ClampMax = "180.0"))
	float StampRotationDegrees = 0.0f;

	/** Optional grayscale heightmap sampled across the stamp's footprint (orthographic projection, not equirectangular - no seam concerns). Leave unset for a procedural dome/crater instead. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp")
	TObjectPtr<UTexture2D> StampHeightmap;

	/** Peak height (meters) - the dome/crater's extremum, or the heightmap's white value. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (ClampMin = "0.0", ClampMax = "1000000.0", Units = "m"))
	float AmplitudeMeters = 10.0f;

	/** Only used without a heightmap: carves a crater (dips down at the center, optionally with a rim) instead of a dome. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (EditCondition = "StampHeightmap == nullptr"))
	bool bCrater = false;

	/** Only used with bCrater: adds a raised rim at the crater's edge, as a fraction of Amplitude Meters. 0 disables the rim. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Stamp", meta = (EditCondition = "bCrater && StampHeightmap == nullptr", ClampMin = "0.0"))
	float CraterRimHeight = 0.3f;

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;

private:
	mutable FSolarOrbzTextureHeightSampler Sampler;

	FVector GetStampDirection() const;
};

// ================================================================================================
// USolarOrbzErosionTerrainLayer - unlike Noise/Heightmap/Stamp layers, this one needs the combined
// height of every layer below it across the WHOLE planet (not just a single point) to simulate
// material moving downhill - so it bakes a delta-height grid once per regenerate (see
// USolarOrbzTerrainLayer::Bake) and bilinear-samples that grid per-vertex afterward, exactly like
// USolarOrbzClimateSimulationAsset does for climate.
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
// ================================================================================================

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
