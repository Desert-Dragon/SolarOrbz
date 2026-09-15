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
class USolarOrbzPlanetProfile;
class USolarOrbzBiomeMask;

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

	/**
	 * Multiplier on how strong this layer's contribution is, applied to its raw output before
	 * blending - the simplest way to fade a layer in/out. 1.0 = full strength (the raw output
	 * unchanged), 0.5 = half strength, 2.0 = double, negative = inverts. Independent of Mask below -
	 * this scales the layer everywhere, Mask controls where it applies at all.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Layer")
	float Strength = 1.0f;

	/**
	 * Optional. Scopes this layer's contribution to wherever this mask applies (0..1) - e.g. an
	 * Erosion Layer masked to a wet Climate Mask only carves valleys in wet biomes; a Noise Layer
	 * masked to a latitude range only adds terrain above/below that band. Reuses the same mask
	 * types (Climate Mask, Composite Mask) a Biome's own Mask uses - a Composite Mask here can
	 * reference an existing Biome asset directly, scoping this layer to "wherever that Biome
	 * applies" without redefining its condition. Leave unset for "always applies everywhere" -
	 * every layer's behavior before this existed, so existing planets are unaffected.
	 */
	UPROPERTY(EditAnywhere, Instanced, Category = "SolarOrbz|Layer|Mask")
	TObjectPtr<USolarOrbzBiomeMask> Mask;

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
	virtual void Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, double RadiusCm) {}

	/**
	 * Some layers read part of a planet's physical identity from outside their own asset rather than
	 * being hardcoded per-instance - e.g. Continent reading Profile's continent count, or Planetary
	 * Noise/Continent needing Sea Level so "meters above/below sea level" means the same thing
	 * regardless of where Sea Level actually sits. Called once per regenerate, before
	 * PrepareLayers()/Bake(), with whatever the actor currently has. Profile may be null (none
	 * assigned) or not a Planet Profile at all (Star/Asteroid instead - always check for null before
	 * dereferencing). SeaLevelCm is 0 if no ClimateSimulation is assigned, matching its own default.
	 * No-op by default; only layers that actually care override this. Implementations should NOT
	 * write the result into their own UPROPERTY fields - this layer asset may be shared/reused
	 * across many planets with different Profiles/Sea Levels, so mutating a UPROPERTY here would
	 * corrupt that shared asset's saved data the next time it's edited or the project is saved.
	 * Cache resolved values in a private, non-UPROPERTY member instead (see
	 * USolarOrbzContinentTerrainLayer for the pattern).
	 */
	virtual void ApplyPlanetaryContext(const USolarOrbzPlanetProfile* Profile, float SeaLevelCm) {}

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
	 * Call once per regenerate, before PrepareLayers() - forwards Profile and Sea Level to every
	 * layer's own ApplyPlanetaryContext(). No-op for layers that don't override it.
	 */
	void ApplyPlanetaryContext(const USolarOrbzPlanetProfile* Profile, float SeaLevelCm) const;

	/**
	 * Call once per regenerate, before any EvaluateHeight calls (including from a ClimateSimulation
	 * sampling this same stack) - gives layers that need whole-surface data (e.g. erosion) a chance
	 * to bake it. No-op for layers that don't override RequiresWholeSurfaceBake(). Also caches
	 * RadiusCm for any per-layer Mask that needs it to estimate slope (see EstimateSlopeUpTo).
	 */
	void PrepareLayers(double RadiusCm) const;

	/**
	 * Evaluates every enabled layer in order and returns the combined height, in UE units (cm).
	 * @param ClimateGrid  Optional. Only needed when at least one layer's Mask reads Temperature/
	 *                      Moisture (see AnyLayerNeedsClimateData) - null is the correct default
	 *                      everywhere else, including every call site that predates per-layer masks.
	 */
	float EvaluateHeight(const FVector& UnitDirection, const FVector2D& UV, const FSolarOrbzClimateGrid* ClimateGrid = nullptr) const;

	/** Same as EvaluateHeight, but only accumulates layers with index < EndIndexExclusive - i.e. what a layer at that index would see as "everything below it". */
	float EvaluateHeightUpTo(int32 EndIndexExclusive, const FVector& UnitDirection, const FVector2D& UV, const FSolarOrbzClimateGrid* ClimateGrid = nullptr) const;

	/** True if any layer's Mask needs a Climate Simulation grid to be meaningful (see USolarOrbzBiomeMask::NeedsClimateData) - tells a caller whether it's worth the cost of a climate-aware re-evaluation pass. False (the common case, and free) when no layer has a Mask that cares. */
	bool AnyLayerNeedsClimateData() const;

	/** Same idea as AnyLayerNeedsClimateData, for Context.Slope (see USolarOrbzBiomeMask::NeedsSlope). */
	bool AnyLayerNeedsSlope() const;

private:
	/**
	 * Finite-difference slope estimate (0 = flat, 1 = vertical) for a layer's Mask that needs one
	 * mid-Pass-A, where no real mesh normal exists yet (RecomputeSmoothNormals only runs after the
	 * whole pass finishes). Perturbs UnitDirection a small angle along a tangent direction and
	 * compares EvaluateHeightUpTo before/after - a known simplification (single-tangent-direction
	 * estimate, not a true 2-axis gradient), same spirit as Erosion's own equatorial-only cell
	 * spacing approximation. Only ever paid when a masked layer's Mask::NeedsSlope() is true.
	 */
	float EstimateSlopeUpTo(int32 EndIndexExclusive, const FVector& UnitDirection, const FVector2D& UV, const FSolarOrbzClimateGrid* ClimateGrid) const;

	// Cached once per regenerate - by ApplyPlanetaryContext (Sea Level) and PrepareLayers (Radius) -
	// so EvaluateHeightUpTo can build a full FSolarOrbzBiomeSampleContext for a masked layer without
	// needing either threaded through every call site.
	mutable float CachedSeaLevelCmForMasking = 0.0f;
	mutable double CachedRadiusCmForMasking = 0.0;

	// Same order as Layers, populated once by PrepareLayers() rather than calling Mask->NeedsSlope()
	// (which, for a Composite Mask, walks a whole tree of referenced Biomes) on every single vertex
	// inside EvaluateHeightUpTo - the answer is invariant for the whole regenerate.
	mutable TArray<bool> CachedLayerNeedsSlope;
};

// ================================================================================================
// ESolarOrbzNoiseType / USolarOrbzFractalNoiseTerrainLayerBase - shared fractal (multi-octave)
// noise machinery. Concrete layers (Noise, Planetary Noise below) share this exact noise field,
// domain warp, and seeding, and only differ in how the normalized -1..1 result gets scaled into
// an actual height.
// ================================================================================================

/** Which basis function each octave samples from - changes the terrain's character, not just its scale. */
UENUM(BlueprintType)
enum class ESolarOrbzNoiseType : uint8
{
	/** Smooth, flowing gradient noise - the general-purpose default. Rolling hills, continents. */
	Perlin,
	/** 1-abs(noise), squared to sharpen - sharp mountain ridgelines with V-shaped valleys between them. */
	Ridged,
	/** abs(noise) folded upward - rounded, billowy humps. Softer and more rounded than Perlin, good for plains-like terrain. */
	Billow,
	/** Lattice-interpolated random values rather than gradients - blockier and less "flowy" than Perlin, a distinct visual character rather than a variation on it. */
	Value,
	/** Cellular/Worley noise - distance to the nearest randomly-placed feature point. Produces cell-like patterns rather than smooth ridges/hills - blobby plateaus with sharper boundaries between cells. */
	Voronoi,
};

UCLASS(Abstract, EditInlineNew)
class SOLARORBZ_API USolarOrbzFractalNoiseTerrainLayerBase : public USolarOrbzTerrainLayer
{
	GENERATED_BODY()

public:
	/** Which basis function each octave uses. Only affects the fractal sum below - domain warp always uses Perlin, regardless of this setting. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise")
	ESolarOrbzNoiseType NoiseType = ESolarOrbzNoiseType::Perlin;

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

	/**
	 * When true (default), rescales the fractal sum so its TYPICAL variation matches what you'd
	 * expect from Amplitude/MaxElevationMeters at Strength 1.0. Without this, multi-octave fractal
	 * noise's typical output is naturally much smaller than its nominal peak - a well-known property
	 * of summing octaves with decreasing amplitude - and it gets worse the more Octaves you use.
	 * That's why Strength often needed cranking to 10-15+ to see appreciable terrain before this
	 * existed. Calibrated once per regenerate via Monte Carlo sampling of this exact Seed/Octaves/
	 * Persistence/Lacunarity/NoiseType combination (not a fixed formula), so it stays accurate no
	 * matter how you tune them. Turn off to get the old, uncompensated behavior.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise")
	bool bCompensateAmplitude = true;

	//~ Begin USolarOrbzTerrainLayer interface
	virtual bool RequiresWholeSurfaceBake() const override { return true; }
	virtual void Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, double RadiusCm) override;
	virtual void ApplyPlanetaryContext(const USolarOrbzPlanetProfile* Profile, float SeaLevelCm) override { CachedSeaLevelCm = SeaLevelCm; }
	//~ End USolarOrbzTerrainLayer interface

protected:
	/**
	 * Fractal sum with seeding, optional domain warp, and (if enabled) amplitude compensation
	 * applied, normalized so its typical range is roughly -1..1. Subclasses scale this into an
	 * actual height however makes sense for them (raw amplitude, sea-level-relative meters, etc).
	 */
	float ComputeNormalizedNoise(const FVector& UnitDirection) const;

	/**
	 * Sea Level, UE units (cm), cached from the last ApplyPlanetaryContext() call - 0 if never
	 * called or no ClimateSimulation is assigned. Every concrete layer built on this base (Noise,
	 * Planetary Noise) offsets its own output by this so "meters above/below sea level" means the
	 * same thing regardless of where Sea Level actually sits - the whole system is planet-focused,
	 * so there's no exempt/scale-agnostic layer anymore. Canyon Layer is the one exception: it
	 * carves relative to whatever terrain is already there, not an absolute position, so it doesn't
	 * read this even though it shares this same base class.
	 */
	mutable float CachedSeaLevelCm = 0.0f;

private:
	/** The raw fractal sum before amplitude compensation - what ComputeNormalizedNoise used to return outright. Also what Bake()'s calibration pass samples to measure the compensation this specific Seed/Octaves/Persistence/Lacunarity/NoiseType combination actually needs. */
	float ComputeNormalizedNoiseUncompensated(const FVector& UnitDirection) const;

	/** Set by Bake() each regenerate - 1.0 (no change) when bCompensateAmplitude is false. */
	mutable float CachedAmplitudeScale = 1.0f;

	/** Set by Bake() each regenerate - depends only on Seed, so it's computed once here rather than on every ComputeNormalizedNoiseUncompensated call (i.e. every vertex). */
	mutable FVector CachedSeedOffset = FVector::ZeroVector;
};

// ================================================================================================
// USolarOrbzNoiseTerrainLayer - scaled by a raw, symmetric amplitude in meters (peaks and troughs
// go equally far), rather than Planetary Noise Layer's separate Max Elevation/Max Depth. Genuinely
// offset by Sea Level like Planetary Noise Layer - the system is planet-focused, so there's no
// separate scale-agnostic variant to reach for here.
// ================================================================================================
UCLASS(EditInlineNew, meta = (DisplayName = "Noise Layer"))
class SOLARORBZ_API USolarOrbzNoiseTerrainLayer : public USolarOrbzFractalNoiseTerrainLayerBase
{
	GENERATED_BODY()

public:
	/** Final height scale, in meters - the peak-to-peak range the fractal sum is mapped onto, symmetric above and below Sea Level. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Noise", meta = (ClampMin = "0.0", ClampMax = "1000000.0", Units = "m"))
	float AmplitudeMeters = 5.0f;

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
};

// ================================================================================================
// USolarOrbzPlanetaryNoiseTerrainLayer - scaled by real-world meters above/below sea level rather
// than a raw symmetric amplitude, with independent Max Elevation/Max Depth so peaks and trenches
// can go different distances - the same value means the same thing whether your planet is
// Earth-scale or a 50m test sphere, so changing Radius never requires re-tuning this layer.
// Genuinely offset by Sea Level (see ApplyPlanetaryContext on the base class) - moving Sea Level
// shifts this layer's whole output with it.
// ================================================================================================
UCLASS(EditInlineNew, meta = (DisplayName = "Planetary Noise Layer"))
class SOLARORBZ_API USolarOrbzPlanetaryNoiseTerrainLayer : public USolarOrbzFractalNoiseTerrainLayerBase
{
	GENERATED_BODY()

public:
	/**
	 * How high above sea level (meters) this layer's tallest peaks should reach - authored in real,
	 * planet-scale-independent meters rather than a raw amplitude. Genuinely relative to Sea Level
	 * now (via ApplyPlanetaryContext, called once per regenerate) - not just the base radius - so
	 * moving Sea Level on your ClimateSimulation asset shifts this layer's whole output with it.
	 * Reference: Earth's Everest is ~8,850m.
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
 * at the default Add. Strength (inherited) scales the overall erosion intensity without re-baking.
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
	virtual void Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, double RadiusCm) override;
	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
	//~ End USolarOrbzTerrainLayer interface

private:
	int32 BakedWidth = 0;
	int32 BakedHeight = 0;

	/** Height AFTER erosion minus height BEFORE erosion, per cell - this is what GetRawHeight samples, since this layer's contribution is a delta on top of the layers it eroded. */
	TArray<float> BakedDeltaHeightCm;
};

// ================================================================================================
// USolarOrbzTerraceTerrainLayer - quantizes the height of everything below it into flat plateaus
// with steps between them, like Erosion this needs the combined height of every layer below it
// across the whole planet (not just a single point), so it bakes a delta grid the same way. Gives
// genuine flat plains/plateaus rather than terrain that only happens to look flat in places.
//
// Matches World Creator's Terrace filter family: Simple/Steep (Irregularity 0, just a different
// Step Height) and Irregular (Irregularity > 0) aren't separate layer types here, just different
// values of the same properties.
// ================================================================================================
UCLASS(EditInlineNew, meta = (DisplayName = "Terrace Layer"))
class SOLARORBZ_API USolarOrbzTerraceTerrainLayer : public USolarOrbzTerrainLayer
{
	GENERATED_BODY()

public:
	/** Bake grid resolution, longitude axis. Independent of mesh density. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Terrace|Grid", meta = (ClampMin = "8"))
	int32 GridWidth = 256;

	/** Bake grid resolution, latitude axis. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Terrace|Grid", meta = (ClampMin = "4"))
	int32 GridHeight = 128;

	/** Vertical distance between plateaus, meters. Smaller values give more, closer-together steps; larger values give fewer, wider plains between bigger jumps. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Terrace", meta = (ClampMin = "1.0", Units = "m"))
	float StepHeightMeters = 200.0f;

	/** How much of each step's height band is a smooth ramp up to the next plateau, rather than perfectly flat. 0 = sharp stair-step cliffs between plateaus. 1 = no flat area at all - effectively disables terracing. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Terrace", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float EdgeSoftness = 0.1f;

	/** Blends between the original (untouched) height and the fully terraced result - 0 disables this layer's effect entirely, 1 is fully terraced. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Terrace", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TerraceStrength = 0.75f;

	/** 0 = perfectly uniform steps (World Creator's "Simple"/"Steep" presets - just tune Step Height Meters for the difference). >0 jitters step boundaries with noise so they don't look like perfectly uniform contour lines (World Creator's "Irregular"). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Terrace|Irregularity", meta = (ClampMin = "0.0"))
	float IrregularityStrength = 0.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Terrace|Irregularity", meta = (ClampMin = "0.01", EditCondition = "IrregularityStrength > 0.0"))
	float IrregularityFrequency = 4.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Terrace|Irregularity", meta = (EditCondition = "IrregularityStrength > 0.0"))
	int32 IrregularitySeed = 0;

	//~ Begin USolarOrbzTerrainLayer interface
	virtual bool RequiresWholeSurfaceBake() const override { return true; }
	virtual void Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, double RadiusCm) override;
	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
	//~ End USolarOrbzTerrainLayer interface

private:
	int32 BakedWidth = 0;
	int32 BakedHeight = 0;

	/** Terraced height minus original height, per cell - same delta-grid pattern as Erosion. */
	TArray<float> BakedDeltaHeightCm;
};

// ================================================================================================
// USolarOrbzCanyonTerrainLayer - carves narrow, sharp-edged grooves following ridge-like noise
// patterns, rather than the broad drainage networks Erosion's hydraulic pass produces. A pure
// function of position (like Noise/Stamp), not a whole-surface bake - it doesn't need to know
// about surrounding terrain, just where its own ridged pattern peaks.
//
// Built on the same fractal noise machinery as Noise/Planetary Noise Layer, so Seed/Octaves/
// Frequency/Lacunarity/Persistence/Warp all work identically here. Defaults to the Ridged basis,
// since canyons following true ridgelines is the useful case, but any Noise Type can be selected -
// a Voronoi-based canyon carves along cell boundaries instead, for a different (more angular) look.
// ================================================================================================
UCLASS(EditInlineNew, meta = (DisplayName = "Canyon Layer"))
class SOLARORBZ_API USolarOrbzCanyonTerrainLayer : public USolarOrbzFractalNoiseTerrainLayerBase
{
	GENERATED_BODY()

public:
	USolarOrbzCanyonTerrainLayer();

	/** How deep canyons carve at their sharpest point, meters. Always carves downward regardless of Blend Mode's sign convention elsewhere. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Canyon", meta = (ClampMin = "0.0", ClampMax = "1000000.0", Units = "m"))
	float DepthMeters = 500.0f;

	/** How much of the underlying noise's peak range actually carves a canyon, 0-1. Small values give a few narrow, isolated canyons; larger values give wider or more frequent canyon networks. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Canyon", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float CanyonWidth = 0.15f;

	/** Shapes the canyon's cross-section profile. 1 = linear V-shape. Higher values give flatter canyon floors with steeper walls near the rim. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Canyon", meta = (ClampMin = "0.1"))
	float Sharpness = 2.0f;

	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
};

// ================================================================================================
// FSolarOrbzContinentSeedData / USolarOrbzContinentTerrainLayer - places a controllable number of
// discrete landmasses via seeded/grown regions instead of noise-derived coastlines, so an actual
// continent COUNT is directly authorable rather than an emergent side effect of noise frequency.
//
// A pure function of position (like Noise/Stamp) - seed positions/radii don't depend on anything
// below it in the stack, so it opts into the whole-surface-bake hook purely as a "run once per
// regenerate" moment to regenerate its seed list deterministically, not because it needs
// PriorLayersHeight (it's ignored). Place this FIRST in a stack, before your mountain/detail noise
// layers - it defines the base land/ocean shape those layers then add detail on top of.
//
// Islands are just smaller-radius versions of the same seed-growth mechanism as continents, not a
// separate algorithm or a post-hoc size classification - Min/Max Continent Radius vs Min/Max
// Island Radius is what actually distinguishes them, so both counts are independently authorable.
//
// Known simplification: seed placement is pure uniform-random on the sphere, not blue-noise/
// Poisson-disc - so seeds can occasionally cluster closer together than a hand-placed layout would,
// though this is what re-rolling Seed is for in practice.
// ================================================================================================
struct SOLARORBZ_API FSolarOrbzContinentSeedData
{
	FVector Direction = FVector::UpVector; // unit vector, seed center
	float RadiusRadians = 0.0f; // great-circle angular radius before coastline noise perturbation
	FVector NoiseOffset = FVector::ZeroVector; // decorrelates this seed's coastline wiggle from every other seed's
};

UCLASS(EditInlineNew, meta = (DisplayName = "Continent Layer"))
class SOLARORBZ_API USolarOrbzContinentTerrainLayer : public USolarOrbzTerrainLayer
{
	GENERATED_BODY()

public:
	/** Same seed always resolves to the same continent layout - change this to reroll. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent")
	int32 Seed = 0;

	/** How many continent-scale landmasses to place, at random positions on the sphere. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent", meta = (ClampMin = "0"))
	int32 NumContinents = 5;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent", meta = (ClampMin = "1.0", ClampMax = "90.0"))
	float MinContinentRadiusDegrees = 15.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent", meta = (ClampMin = "1.0", ClampMax = "90.0"))
	float MaxContinentRadiusDegrees = 35.0f;

	/** How many smaller islands to scatter, at random positions on the sphere - independent of, and in addition to, the continents above. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Islands", meta = (ClampMin = "0"))
	int32 NumIslands = 10;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Islands", meta = (ClampMin = "0.1", ClampMax = "90.0"))
	float MinIslandRadiusDegrees = 1.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Islands", meta = (ClampMin = "0.1", ClampMax = "90.0"))
	float MaxIslandRadiusDegrees = 5.0f;

	/** Places a landmass centered exactly on the north pole (an Antarctica-analogue, just at the other end) - not randomly positioned like continents/islands above. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Poles")
	bool bHasNorthPolarContinent = false;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Poles")
	bool bHasSouthPolarContinent = false;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Poles", meta = (ClampMin = "1.0", ClampMax = "90.0", EditCondition = "bHasNorthPolarContinent || bHasSouthPolarContinent"))
	float PolarContinentRadiusDegrees = 20.0f;

	/** How much each landmass's coastline wiggles away from a perfect circle, as a fraction of that landmass's own radius. 0 = perfect circles. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Coastline", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CoastlineNoiseAmplitude = 0.3f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Coastline", meta = (ClampMin = "0.01"))
	float CoastlineNoiseFrequency = 3.0f;

	/** Shapes the ocean-to-land transition. 1 = a gradual continental shelf. Higher values give a sharper, more sudden coastline. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Coastline", meta = (ClampMin = "0.1"))
	float CoastlineSharpness = 1.5f;

	/** How deep the ocean floor sits, meters, relative to sea level - the height this layer outputs everywhere no landmass reaches. Genuinely offset by Sea Level now (via ApplyPlanetaryContext) - moving Sea Level shifts the whole ocean floor/land plateau with it. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Height", meta = (ClampMax = "0.0", Units = "m"))
	float OceanFloorDepthMeters = -4000.0f;

	/** How high the base land plateau sits, meters, relative to sea level - before any mountain/detail noise layers stacked on top of this one add real terrain. Keep this modest; it's a base shelf, not the final peak height. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Height", meta = (ClampMin = "0.0", Units = "m"))
	float LandPlateauHeightMeters = 200.0f;

	/**
	 * When true (default), Num Continents / Num Islands / the pole flags above are overridden each
	 * regenerate by the actor's assigned Planet Profile - IF one is actually assigned AND
	 * ApplyPlanetaryContext() was actually called (the actor's RegenerateMesh does this
	 * automatically; a standalone/test use of this layer outside that flow won't). The properties
	 * above stay as the fallback used whenever no override is in effect, and are never overwritten
	 * by the override - this layer asset may be shared across many planets with different Profiles,
	 * so mutating its own saved data here would corrupt that sharing. Turn this off to always use
	 * this layer's own values regardless of any assigned Profile - useful for a secondary/detail
	 * continent layer that shouldn't track the planet's "official" landmass count. Note this only
	 * covers Num Continents/Islands/poles - Sea Level offsetting (below) always applies regardless
	 * of this toggle, since it's a units/reference-point correction, not a per-planet override.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent")
	bool bOverrideFromProfile = true;

	//~ Begin USolarOrbzTerrainLayer interface
	virtual void ApplyPlanetaryContext(const USolarOrbzPlanetProfile* Profile, float SeaLevelCm) override;
	virtual bool RequiresWholeSurfaceBake() const override { return true; }
	virtual void Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, double RadiusCm) override;
	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
	//~ End USolarOrbzTerrainLayer interface

private:
	/** Set by ApplyPlanetaryContext() each regenerate - null if no Profile was assigned, it wasn't a Planet Profile, ApplyPlanetaryContext() was never called, or bOverrideFromProfile is false. Bake() reads Num Continents/etc through this when valid, falling back to this layer's own authored properties otherwise. Weak, not a hard reference - a Profile's lifetime isn't tied to this layer's. */
	TWeakObjectPtr<const USolarOrbzPlanetProfile> ProfileOverride;

	/** Sea Level, UE units (cm), cached from the last ApplyPlanetaryContext() call - 0 if never called or no ClimateSimulation is assigned. GetRawHeight adds this to the Ocean Floor Depth/Land Plateau Height result, so both are genuinely relative to Sea Level, not just the raw base radius. */
	float CachedSeaLevelCm = 0.0f;

	/** Regenerated each Bake() from Seed/NumContinents/NumIslands/poles - GetRawHeight only ever reads this, never regenerates it, so per-vertex cost stays a cheap linear scan. */
	TArray<FSolarOrbzContinentSeedData> CachedSeeds;
};
