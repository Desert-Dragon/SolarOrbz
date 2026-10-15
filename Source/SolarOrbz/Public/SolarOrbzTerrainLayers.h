// SolarOrbz - Terrain Layers subsystem. Combines the layer base class/blend-mode enum, the
// ordered layer stack, and every concrete layer type (Noise, Planetary Noise, Heightmap, Stamp,
// Erosion) into one file. Grouped together because they form a single tightly-coupled inheritance
// hierarchy - USolarOrbzTerrainLayerStack::EvaluateHeight walks an array of USolarOrbzTerrainLayer
// pointers, and every concrete class below exists purely to be an entry in that array.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Templates/Function.h"
#include "Misc/Guid.h"
#include "Engine/DataAsset.h"
#include "SolarOrbzIcoSphere.h"
#include "SolarOrbzTerrainLayers.generated.h"

class UTexture2D;
class USolarOrbzPlanetProfile;
class USolarOrbzBiomeMask;
class USolarOrbzTerrainGraph;

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
	 * Called once per regenerate, before GetRawHeight is ever called for this layer, same timing as
	 * Bake() but unconditional (every enabled layer, not just RequiresWholeSurfaceBake() ones) - a
	 * place for a layer to populate any lazily-built mutable cache up front, single-threaded, so
	 * GetRawHeight's own per-vertex calls never race to populate it themselves. Added specifically
	 * because GetRawHeight is called from inside a ParallelFor (see
	 * ASolarOrbzIcoSphereActor::RegenerateMesh) - a layer whose GetRawHeight lazily fills a `mutable`
	 * member (e.g. Heightmap/Stamp's FSolarOrbzTextureHeightSampler) MUST pre-populate it here instead,
	 * or concurrent first-touch from multiple worker threads is a real data race. No-op by default;
	 * only layers with a lazy mutable cache need to override it.
	 */
	virtual void WarmCaches() const {}

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

	/**
	 * Stable identity for the Terrain Graph Editor only (see Docs/SolarOrbzTerrainGraphEditor.md) -
	 * "which saved node position belongs to which layer", so a saved layout survives reordering/
	 * inserting/removing layers in a way a plain array index would not. Never read by GetRawHeight/
	 * Bake/ApplyPlanetaryContext/anything evaluation-related. Invalid (FGuid::IsValid() == false)
	 * until EnsureEditorNodeId() is called - a freshly-created layer (e.g. a default-constructed
	 * Instanced entry, or one added via the graph's future "Add Node" menu) has no id yet.
	 */
	UPROPERTY()
	FGuid EditorNodeId;

	/** Assigns a new Guid if EditorNodeId isn't already valid. Safe to call repeatedly - a no-op once assigned. */
	void EnsureEditorNodeId()
	{
		if (!EditorNodeId.IsValid())
		{
			EditorNodeId = FGuid::NewGuid();
		}
	}
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
	 * Editor-only layout memory for the Terrain Graph Editor (see
	 * Docs/SolarOrbzTerrainGraphEditor.md) - keyed by each layer's EditorNodeId, not array index, so
	 * a saved node arrangement survives reordering/inserting/removing layers. Everything else about
	 * the graph (which nodes exist, their order, their properties) is already fully determined by
	 * Layers itself and needs no separate storage here.
	 */
	UPROPERTY()
	TMap<FGuid, FVector2D> EditorNodePositions;

	/**
	 * Transient view over Layers for the Terrain Graph Editor - built on first access by
	 * GetOrCreateTerrainGraph(), never serialized, never read by EvaluateHeight/Bake/
	 * ApplyPlanetaryContext/anything evaluation-related (only Layers is).
	 */
	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<USolarOrbzTerrainGraph> TerrainGraph;

	/**
	 * Returns the transient Terrain Graph Editor view over Layers, building and populating it via
	 * USolarOrbzTerrainGraph::RebuildFromLayers() on first call. Does NOT refresh an already-built
	 * graph on subsequent calls - call TerrainGraph->RebuildFromLayers(this) directly to force a
	 * refresh (e.g. after something outside the graph itself changed Layers). Never read by
	 * EvaluateHeight/Bake/ApplyPlanetaryContext/anything evaluation-related - Layers remains the only
	 * thing those ever see.
	 */
	USolarOrbzTerrainGraph* GetOrCreateTerrainGraph();

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
	virtual void WarmCaches() const override { Sampler.EnsureDecoded(HeightmapTexture); }

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
	virtual void WarmCaches() const override { Sampler.EnsureDecoded(StampHeightmap); }

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
// ESolarOrbzContinentAlgorithm / FSolarOrbzContinentSeedData / USolarOrbzContinentTerrainLayer -
// places a controllable number of discrete landmasses, so an actual continent COUNT is directly
// authorable rather than an emergent side effect of noise frequency. Three interchangeable
// algorithms (below), picked per-instance via Algorithm - researched against real procedural
// generation references rather than guessed, after "continents still look too circular" feedback
// on the original single-algorithm version:
//
// - RadialSeeds: Red Blob Games' island-shaping article (redblobgames.com/maps/terrain-from-noise/
//   islands.html) and Inigo Quilez's domain-warping article (iquilezles.org/articles/warp) - a
//   circle test is still the base shape, but the SAMPLE POINT is domain-warped before the test
//   (not just the output radius perturbed, which can never change the boundary's topology), the
//   boundary wobble is multi-octave fBm instead of one Perlin call, and each landmass can scatter a
//   few smaller "metaball" sub-seeds around its main seed so the union of several circles - not one
//   - makes the actual coastline. Cheap: a pure per-point function, no whole-surface bake.
// - VoronoiGrowth: Azgaar's Fantasy Map Generator (github.com/Azgaar/Fantasy-Map-Generator) - each
//   landmass starts at one cell of this layer's own bake grid and randomly floods outward into
//   unclaimed neighbor cells, losing "growth energy" by a randomized decay each hop, stopping once
//   energy runs out. The randomized, graph-walk (not radial-formula) growth order is what actually
//   produces bays/peninsulas/non-convex coastlines - and since a cell can only be claimed once,
//   overlap between landmasses (poles included) is prevented by construction, not rejection-sampling.
// - PlateTectonics: approximate, NOT a physics simulation, in the spirit of Cortial, Peytavie,
//   Galin & Guerin's "Procedural Tectonic Planets" (Computer Graphics Forum 38:2, 2019) - partitions
//   the sphere into randomly-drifting plates (nearest-seed/Worley assignment), classifies each
//   plate boundary as convergent/divergent/transform from the two plates' relative drift, and
//   shapes mountains/trenches/ridges/rifts from that classification. Continents are an emergent
//   side effect of which plates are flagged oceanic vs continental, not an authored count - Num
//   Continents/Islands do nothing in this mode.
//
// VoronoiGrowth and PlateTectonics share one bake grid (BakeGridWidth/Height) and one cached
// TArray<float> height raster, the same whole-surface-bake-then-bilinear-sample pattern Erosion/
// Terrace already use (see FSolarOrbzLatLongGrid) - RadialSeeds alone stays a pure per-point
// function with no grid, unchanged from the original design.
//
// Islands are just smaller-radius versions of the same seed-growth mechanism as continents in
// RadialSeeds (and the same growth mechanism at a smaller energy budget in VoronoiGrowth) - not a
// separate algorithm or a post-hoc size classification.
//
// Switching Algorithm, like changing Seed, re-rolls the whole layout - there is no cross-algorithm
// layout compatibility, nor any attempt at it.
// ================================================================================================

UENUM(BlueprintType)
enum class ESolarOrbzContinentAlgorithm : uint8
{
	RadialSeeds,
	VoronoiGrowth,
	PlateTectonics,
};

struct SOLARORBZ_API FSolarOrbzContinentSeedData
{
	FVector Direction = FVector::UpVector; // unit vector, seed center
	float RadiusRadians = 0.0f; // great-circle angular radius before coastline noise perturbation
	FVector NoiseOffset = FVector::ZeroVector; // decorrelates this seed's coastline wiggle from every other seed's
};

/**
 * Persistent per-plate state for ESolarOrbzContinentAlgorithm::PlateTectonics, carried forward across
 * every Bake() once seeded (see USolarOrbzContinentTerrainLayer::EnsurePlatesInitialized) rather than
 * rerandomized from Seed every regenerate the way this layer originally worked - that's what lets
 * this layer's own "Run Simulation" button actually accumulate movement over time.
 * Models each plate as drifting along a fixed great-circle path at a constant angular rate - a
 * deliberate simplification: RotationAxis/AngularRateRadPerMyr are rolled once at seed time (or at
 * split time, see SplitPlate) and never changed by simulation itself; no merging.
 */
USTRUCT()
struct SOLARORBZ_API FSolarOrbzTectonicPlate
{
	GENERATED_BODY()

	/**
	 * Stable identity, assigned once from USolarOrbzContinentTerrainLayer::NextPlateId at seed or
	 * split time and never reused - lets FSolarOrbzTectonicBoundaryState track a plate pair's
	 * accumulated stress across simulation steps even though this plate's own index in Plates shifts
	 * whenever an earlier plate is removed (e.g. by SplitPlate). INDEX_NONE on a default-constructed
	 * plate that hasn't been through EnsurePlatesInitialized/SplitPlate yet.
	 */
	UPROPERTY()
	int32 PlateId = INDEX_NONE;

	/** Current position - the point this plate's nearest-seed/Worley partition grows from. Moves every AdvanceTectonicSimulation() step; a never-simulated plate just sits at its original random seed point. */
	UPROPERTY()
	FVector SeedDirection = FVector::UpVector;

	/** Fixed unit vector - the pole SeedDirection orbits around. Unlike SeedDirection, never changes once rolled at seed (or split) time. */
	UPROPERTY()
	FVector RotationAxis = FVector::UpVector;

	/** How fast SeedDirection orbits RotationAxis, radians of simulated time per million years. */
	UPROPERTY()
	float AngularRateRadPerMyr = 0.0f;

	UPROPERTY()
	bool bOceanic = false;

	/** Simulated million years since this plate was seeded (or split off its parent, where it resets to 0 - a reasonable simplification; the crust itself isn't literally new, but this plate's identity is). */
	UPROPERTY()
	float AgeMyr = 0.0f;

	/**
	 * Instantaneous tangent-plane drift velocity at the CURRENT SeedDirection (v = RotationAxis x
	 * SeedDirection * AngularRate) - the real-time replacement for the old static "PlateDrift" vector
	 * BakePlateTectonics used to classify a boundary as convergent/divergent/transform.
	 */
	FVector GetInstantaneousDrift() const
	{
		return FVector::CrossProduct(RotationAxis, SeedDirection) * AngularRateRadPerMyr;
	}

	/** Ad-hoc binary serialization for USolarOrbzContinentTerrainLayer's tectonic-simulation temp-file backup (see SaveTectonicBackupToTempFile) - NOT the same path as this USTRUCT's normal tagged-property UPROPERTY serialization inside the owning asset. */
	friend FArchive& operator<<(FArchive& Ar, FSolarOrbzTectonicPlate& Plate)
	{
		return Ar << Plate.PlateId << Plate.SeedDirection << Plate.RotationAxis << Plate.AngularRateRadPerMyr << Plate.bOceanic << Plate.AgeMyr;
	}
};

/**
 * Persistent per-plate-PAIR state for ESolarOrbzContinentAlgorithm::PlateTectonics - how long
 * (simulated million years) two specific plates have been sustained-convergent or sustained-
 * divergent, keyed by their stable FSolarOrbzTectonicPlate::PlateId (PlateIdA < PlateIdB always, so
 * lookup doesn't care which order the two plates were found in). Built up incrementally by
 * USolarOrbzContinentTerrainLayer::UpdateBoundaryStress (called once per AdvanceTectonicSimulation
 * step, NOT on every ordinary Bake()) rather than recomputed fresh each time - this is what lets a
 * mountain range genuinely grow taller the longer two plates have been pushing into each other,
 * instead of every bake seeing the same instantaneous classification no matter how long it's been
 * true. AccumulatedConvergenceMyr drives USolarOrbzContinentTerrainLayer::MaxStressMultiplier (taller
 * mountains/deeper trenches); AccumulatedDivergenceMyr drives PlateSplitThresholdMyr (sustained rifts
 * eventually split one of the two plates in two - see SplitPlate). Neither value decays - once two
 * plates have spent time convergent or divergent, that history persists even if their relative motion
 * later changes, a deliberate simplification over tracking true geologic unloading/healing.
 */
USTRUCT()
struct SOLARORBZ_API FSolarOrbzTectonicBoundaryState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 PlateIdA = INDEX_NONE;

	UPROPERTY()
	int32 PlateIdB = INDEX_NONE;

	UPROPERTY()
	float AccumulatedConvergenceMyr = 0.0f;

	UPROPERTY()
	float AccumulatedDivergenceMyr = 0.0f;

	/** Ad-hoc binary serialization for USolarOrbzContinentTerrainLayer's tectonic-simulation temp-file backup (see SaveTectonicBackupToTempFile) - NOT the same path as this USTRUCT's normal tagged-property UPROPERTY serialization inside the owning asset. */
	friend FArchive& operator<<(FArchive& Ar, FSolarOrbzTectonicBoundaryState& State)
	{
		return Ar << State.PlateIdA << State.PlateIdB << State.AccumulatedConvergenceMyr << State.AccumulatedDivergenceMyr;
	}
};

UCLASS(EditInlineNew, meta = (DisplayName = "Continent Layer"))
class SOLARORBZ_API USolarOrbzContinentTerrainLayer : public USolarOrbzTerrainLayer
{
	GENERATED_BODY()

public:
	/** Same seed always resolves to the same continent layout - change this to reroll. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent")
	int32 Seed = 0;

	/** Which generation algorithm places/shapes the landmasses below - see the class comment above for what each one actually does and where it came from. Changing this re-rolls the whole layout, the same as changing Seed. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent")
	ESolarOrbzContinentAlgorithm Algorithm = ESolarOrbzContinentAlgorithm::RadialSeeds;

	/** How many continent-scale landmasses to place, at random positions on the sphere. Ignored by Plate Tectonics, where continents emerge from the plate partition instead. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent", meta = (ClampMin = "0", EditCondition = "Algorithm != ESolarOrbzContinentAlgorithm::PlateTectonics"))
	int32 NumContinents = 5;

	/** Radial Seeds: the circle's actual radius. Voronoi Growth: converted into a starting growth-energy budget (via this layer's bake grid resolution) so the two algorithms stay authored in the same units. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent", meta = (ClampMin = "1.0", ClampMax = "90.0", EditCondition = "Algorithm != ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float MinContinentRadiusDegrees = 15.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent", meta = (ClampMin = "1.0", ClampMax = "90.0", EditCondition = "Algorithm != ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float MaxContinentRadiusDegrees = 35.0f;

	/** How many smaller islands to scatter, at random positions on the sphere - independent of, and in addition to, the continents above. Ignored by Plate Tectonics. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Islands", meta = (ClampMin = "0", EditCondition = "Algorithm != ESolarOrbzContinentAlgorithm::PlateTectonics"))
	int32 NumIslands = 10;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Islands", meta = (ClampMin = "0.1", ClampMax = "90.0", EditCondition = "Algorithm != ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float MinIslandRadiusDegrees = 1.0f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Islands", meta = (ClampMin = "0.1", ClampMax = "90.0", EditCondition = "Algorithm != ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float MaxIslandRadiusDegrees = 5.0f;

	/** Places a landmass centered exactly on the north pole (an Antarctica-analogue, just at the other end) - not randomly positioned like continents/islands above. Works the same way under all three algorithms: Radial Seeds places an explicit polar seed, Voronoi Growth starts (and claims) a growth there before any other landmass, and Plate Tectonics overrides the polar cap's plate assignment to continental regardless of the Worley partition. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Poles")
	bool bHasNorthPolarContinent = false;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Poles")
	bool bHasSouthPolarContinent = false;

	/** Also defines the exclusion zone continents/islands can't end up inside: Radial Seeds rejection-samples against this radius plus a candidate's own radius; Voronoi Growth and Plate Tectonics simply claim/override this radius before anything else is placed, so overlap there is impossible rather than merely rejected. Raising this shrinks the sphere area still open to random placement - under Radial Seeds specifically, with both poles enabled and a large enough radius, random continents/islands may end up placed overlapping anyway (logged as a warning) once there's nowhere left that's clear. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Poles", meta = (ClampMin = "1.0", ClampMax = "90.0", EditCondition = "bHasNorthPolarContinent || bHasSouthPolarContinent"))
	float PolarContinentRadiusDegrees = 20.0f;

	// --- Radial Seeds shape (domain warp, multi-octave boundary, metaball sub-seeds) ---

	/** Radial Seeds only. How far each landmass's SAMPLE POINT is pushed, as a fraction of its own radius, before the circle test runs - this is what actually breaks the "circle with a wobbly edge" look (perturbing the output radius, below, cannot: it's still one smooth distance field around one center). 0 = no warp, a perfect (still boundary-noised) circle. See the class comment's Quilez reference. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::RadialSeeds"))
	float CoastlineWarpStrength = 0.6f;

	/** Radial Seeds only. Frequency of the warp field itself - low values warp in broad sweeps (big bays/peninsulas), high values warp in small, fine wrinkles. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Shape", meta = (ClampMin = "0.01", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::RadialSeeds && CoastlineWarpStrength > 0.0"))
	float CoastlineWarpFrequency = 1.2f;

	/** Radial Seeds only. How many octaves of fBm noise make up the boundary wobble - 1 is the original single-Perlin-call wobble (a fairly smooth, single-frequency ripple); several octaves layer finer detail on top the way every other noise-based layer in this plugin already does. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Shape", meta = (ClampMin = "1", ClampMax = "6", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::RadialSeeds"))
	int32 CoastlineNoiseOctaves = 3;

	/** Radial Seeds only. Amplitude falloff per boundary-noise octave - same meaning as every other fractal layer's Persistence. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::RadialSeeds && CoastlineNoiseOctaves > 1"))
	float CoastlineNoisePersistence = 0.5f;

	/** Radial Seeds only. Frequency multiplier per boundary-noise octave - same meaning as every other fractal layer's Lacunarity. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Shape", meta = (ClampMin = "1.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::RadialSeeds && CoastlineNoiseOctaves > 1"))
	float CoastlineNoiseLacunarity = 2.0f;

	/** Radial Seeds only. Extra smaller "metaball" seeds scattered around each continent/island's main seed - the union of several overlapping circles reads far less like a single circle than one circle alone, and costs nothing extra in GetRawHeight (it already takes the max influence across every seed). 0 = off, the original one-circle-per-landmass look. Doesn't apply to the two fixed polar continents. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Shape", meta = (ClampMin = "0", ClampMax = "5", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::RadialSeeds"))
	int32 SubSeedsPerLandmass = 2;

	/** Radial Seeds only. How far a sub-seed's center can land from its parent's, as a multiple of the parent's own radius - e.g. 0.7 lets a sub-seed's center land up to 70% of the parent's radius away. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Shape", meta = (ClampMin = "0.0", ClampMax = "2.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::RadialSeeds && SubSeedsPerLandmass > 0"))
	float SubSeedScatterFraction = 0.7f;

	/** Radial Seeds only. A sub-seed's own radius, as a fraction of its parent's radius - rolled independently per sub-seed between these two. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Shape", meta = (ClampMin = "0.05", ClampMax = "1.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::RadialSeeds && SubSeedsPerLandmass > 0"))
	float MinSubSeedRadiusFraction = 0.3f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Shape", meta = (ClampMin = "0.05", ClampMax = "1.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::RadialSeeds && SubSeedsPerLandmass > 0"))
	float MaxSubSeedRadiusFraction = 0.7f;

	// --- Voronoi Growth / Plate Tectonics bake grid (shared) ---

	/** Voronoi Growth and Plate Tectonics only. Bake grid resolution, longitude axis - same idea as Erosion/Terrace's own bake grid, independent of mesh density. Higher gives finer coastline/boundary detail at the cost of a slower (one-time, per-regenerate) bake. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Grid", meta = (ClampMin = "32", EditCondition = "Algorithm != ESolarOrbzContinentAlgorithm::RadialSeeds"))
	int32 BakeGridWidth = 256;

	/** Voronoi Growth and Plate Tectonics only. Bake grid resolution, latitude axis. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Grid", meta = (ClampMin = "16", EditCondition = "Algorithm != ESolarOrbzContinentAlgorithm::RadialSeeds"))
	int32 BakeGridHeight = 128;

	// --- Voronoi Growth ---

	/** Voronoi Growth only. Growth energy is multiplied by a random value in this range (plus Growth Jitter below) every time it floods into one more grid cell - lower values decay faster, giving smaller/rougher-edged landmasses for the same starting budget. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Growth", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::VoronoiGrowth"))
	float GrowthDecayMin = 0.90f;

	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Growth", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::VoronoiGrowth"))
	float GrowthDecayMax = 0.985f;

	/** Voronoi Growth only. Extra +/- random jitter applied to each hop's decay on top of Growth Decay Min/Max - this is what actually breaks growth-ring symmetry into organic, non-convex blobs rather than a smooth near-circle; 0 would make growth expand in nearly uniform rings. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Growth", meta = (ClampMin = "0.0", ClampMax = "0.5", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::VoronoiGrowth"))
	float GrowthJitter = 0.35f;

	// --- Plate Tectonics ---

	/** Plate Tectonics only. How many plates partition the sphere (nearest-seed/Worley assignment) - continents/oceans are an emergent side effect of this, not an authored count. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates", meta = (ClampMin = "2", ClampMax = "64", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	int32 NumPlates = 12;

	/** Plate Tectonics only. Fraction of plates randomly flagged oceanic (base height = Ocean Floor Depth) rather than continental (base height = Land Plateau Height), before any boundary mountain/trench/ridge/rift shaping on top. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float OceanicPlateFraction = 0.55f;

	/** Plate Tectonics only. How far from a plate boundary the mountain/trench/ridge/rift shaping below reaches before fading back to that plate's plain base height. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates", meta = (ClampMin = "1.0", ClampMax = "45.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float BoundaryInfluenceDegrees = 8.0f;

	/** Plate Tectonics only. Peak extra height, meters, where two continental plates converge (collision mountains - the Himalaya case). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates", meta = (ClampMin = "0.0", Units = "m", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float MountainHeightMeters = 3000.0f;

	/** Plate Tectonics only. Peak extra depth, meters, on the oceanic side where an oceanic plate converges with and subducts under another plate (the trench case). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates", meta = (ClampMax = "0.0", Units = "m", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float TrenchDepthMeters = -3000.0f;

	/** Plate Tectonics only. Peak extra height, meters, where two oceanic plates diverge (mid-ocean ridge). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates", meta = (ClampMin = "0.0", Units = "m", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float RidgeHeightMeters = 600.0f;

	/** Plate Tectonics only. Peak extra depth, meters, where two continental plates diverge (rift valley). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates", meta = (ClampMax = "0.0", Units = "m", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float RiftDepthMeters = -500.0f;

	/** Plate Tectonics only. Drift speed each plate rolls a random fraction of when first seeded (or reseeded by changing Seed/Num Plates) - only affects freshly-seeded plates; has no effect on a layout Run Simulation has already moved. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates", meta = (ClampMin = "0.0", ClampMax = "20.0", Units = "deg", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float InitialDriftSpeedDegreesPerMyr = 2.0f;

	/** Plate Tectonics only, read-only. Total simulated time (million years) this layer's plates have drifted via Run Simulation since they were last (re)seeded - 0 until the first simulation step ever runs. */
	UPROPERTY(VisibleAnywhere, Category = "SolarOrbz|Continent|Plates", meta = (EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float SimulatedAgeMyr = 0.0f;

	/** Plate Tectonics only, read-only. How many plates currently exist - starts at Num Plates, grows whenever a sustained rift splits one in two (see Plate Split Threshold below). */
	UPROPERTY(VisibleAnywhere, Category = "SolarOrbz|Continent|Plates", meta = (EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	int32 CurrentPlateCount = 0;

	/** Plate Tectonics only. How many simulation steps "Run Simulation" (below) advances per click. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates|Simulation", meta = (ClampMin = "1", ClampMax = "1000", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	int32 StepsPerRun = 10;

	/** Plate Tectonics only. Simulated time each step advances, in million years. */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates|Simulation", meta = (ClampMin = "0.01", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float MillionYearsPerStep = 5.0f;

	/**
	 * Plate Tectonics only. Advances this layer's own plates by Steps Per Run steps of Million Years
	 * Per Step each - see AdvanceTectonicSimulation(), which does the actual work; this is just the
	 * button wired to this layer's own authored step size. Does not itself trigger a regenerate -
	 * regenerate the planet afterward to see the result, same as any other property edit.
	 */
	UFUNCTION(CallInEditor, Category = "SolarOrbz|Continent|Plates|Simulation", meta = (EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	void RunSimulation();

	/**
	 * Plate Tectonics only. How many simulated million years of SUSTAINED convergence or divergence a
	 * boundary needs to reach its full extra stress multiplier (Max Stress Multiplier below) - a
	 * boundary that's only just started converging/diverging gets none of this bonus yet; one that's
	 * been doing so for many simulation steps approaches the full amount. Only ever grows via Run
	 * Simulation's steps, never during an ordinary Bake() - an ordinary regenerate reads whatever's
	 * already accumulated but never adds to it.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates|Simulation", meta = (ClampMin = "0.1", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float StressMatureTimescaleMyr = 50.0f;

	/** Plate Tectonics only. Extra multiplier a fully-matured (see Stress Mature Timescale above) boundary's mountain/trench/ridge/rift modifier gets on top of its base magnitude - 1.0 means a long-convergent boundary's mountains can reach up to double Mountain Height Meters. 0 disables mountain-building entirely (every boundary stays at its base magnitude regardless of how long it's been active, the original pre-simulation behavior). */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates|Simulation", meta = (ClampMin = "0.0", ClampMax = "5.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float MaxStressMultiplier = 1.0f;

	/**
	 * Plate Tectonics only. How many simulated million years of sustained divergence a boundary needs
	 * before the larger of its two plates splits in two at the rift (a supercontinent-rifting-apart
	 * event, not a physically complete model - see SplitPlate's own comment). The two new plates
	 * inherit the parent's oceanic/continental flag and drift apart along the original separation
	 * direction. 0 disables splitting entirely. Capped at 64 total plates regardless of this value
	 * (matches Num Plates' own authoring cap) - further divergence past the cap just keeps
	 * accumulating Max Stress Multiplier's bonus instead of splitting again.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent|Plates|Simulation", meta = (ClampMin = "0.0", EditCondition = "Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics"))
	float PlateSplitThresholdMyr = 200.0f;

	// --- Coastline texture (shared by all three algorithms) ---

	/** Radial Seeds: how much each landmass's coastline wiggles away from a perfect circle, as a fraction of that landmass's own radius - 0 = perfect circles (before the warp/sub-seeds above still break that up). Voronoi Growth/Plate Tectonics: a smaller cosmetic wobble layered on top of their own grid-driven coastlines. */
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
	 * of this toggle, since it's a units/reference-point correction, not a per-planet override. Has
	 * no effect on Plate Tectonics, which ignores Num Continents/Islands entirely regardless of
	 * where they'd otherwise come from.
	 */
	UPROPERTY(EditAnywhere, Category = "SolarOrbz|Continent")
	bool bOverrideFromProfile = true;

	//~ Begin UObject interface
	/** Plate Tectonics only: checks for a newer temp-file backup of Plates/BoundaryStates (see TryRestoreTectonicBackupFromTempFile) than whatever this layer just loaded from the asset itself - recovers simulation progress a crash or an unexpected editor restart would otherwise have lost since the last manual save. A no-op for the other two algorithms. */
	virtual void PostLoad() override;
	//~ End UObject interface

	//~ Begin USolarOrbzTerrainLayer interface
	virtual void ApplyPlanetaryContext(const USolarOrbzPlanetProfile* Profile, float SeaLevelCm) override;
	virtual bool RequiresWholeSurfaceBake() const override { return Algorithm != ESolarOrbzContinentAlgorithm::RadialSeeds; }
	virtual void Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, double RadiusCm) override;
	virtual float GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const override;
	//~ End USolarOrbzTerrainLayer interface

	/**
	 * Plate Tectonics only. Advances every plate NumSteps times by DeltaMyr each, moving SeedDirection
	 * along its fixed RotationAxis, updating every live plate pair's accumulated convergence/
	 * divergence stress (see UpdateBoundaryStress) - which can split a plate in two once a rift's
	 * divergence crosses Plate Split Threshold Myr - and reclassifying boundaries from the plates'
	 * new relative positions/drift. Called by this layer's own "Run Simulation" button (RunSimulation(),
	 * below) with StepsPerRun/MillionYearsPerStep. (Re)seeds Plates first if this is the first call
	 * since this layer was created or Seed/Num Plates last changed. No-op (with a log warning) if
	 * Algorithm isn't PlateTectonics. Does NOT itself re-bake height or trigger the owning actor's
	 * regenerate - regenerate the planet afterward to see the result, the same as changing any other
	 * layer property.
	 */
	void AdvanceTectonicSimulation(int32 NumSteps, float DeltaMyr);

private:
	/**
	 * (Re)seeds Plates from FRandomStream(Seed) if this is the first call ever (bPlatesEverSeeded is
	 * false), Seed changed since Plates was last seeded, or Num Plates (the AUTHORED property, not
	 * the live Plates.Num() - see PlatesSeededFromNumPlates) changed - otherwise leaves Plates (and
	 * anything AdvanceTectonicSimulation has already done to it, split plates included) untouched.
	 * Called by both BakePlateTectonics() and AdvanceTectonicSimulation().
	 */
	void EnsurePlatesInitialized();

	/**
	 * Called once per AdvanceTectonicSimulation() step (never from an ordinary Bake(), which only
	 * ever reads accumulated stress, not adds to it) - runs a cheap, low-resolution Worley partition
	 * purely to find which plate pairs are CURRENTLY adjacent, updates each pair's
	 * FSolarOrbzTectonicBoundaryState by DeltaMyr (convergence or divergence, whichever this pair's
	 * current relative drift classifies as - see BakePlateTectonics' own classification, duplicated
	 * here at lower resolution since this runs far more often), prunes any boundary state referencing
	 * a plate that no longer exists, and calls SplitPlate() for any pair whose divergence just crossed
	 * Plate Split Threshold Myr.
	 */
	void UpdateBoundaryStress(float DeltaMyr);

	/**
	 * Splits the plate identified by PlateIdToSplit into two new plates, triggered by sustained
	 * divergence against OtherPlateId (used only to find the rift's separation axis). The new
	 * boundary runs perpendicular to the original separation direction - geometrically, the two new
	 * seed points are placed symmetrically offset from the parent's original seed ALONG that
	 * direction, so the nearest-seed partition's bisector between them (the new rift) ends up
	 * perpendicular to it, same as a real spreading ridge. Each child's RotationAxis is set so its
	 * own GetInstantaneousDrift() points directly away from the other child along that same
	 * direction (verified via the vector triple-product identity (SxT)xS = T for unit, orthogonal
	 * S/T) - continuing to pull the rift open rather than inheriting the parent's original drift
	 * unchanged. Both children inherit the parent's oceanic/continental flag (splitting a continent
	 * makes two continents, not two fresh ocean floors - a deliberate simplification, not a full
	 * rift-forms-new-ocean-crust model) and reset AgeMyr to 0. No-op if either plate can't be found by
	 * id, or Plates is already at its 64-plate cap.
	 */
	void SplitPlate(int32 PlateIdToSplit, int32 OtherPlateId);

	/** Finds (adding a zeroed entry if needed) the FSolarOrbzTectonicBoundaryState for this plate pair - PlateIdA/PlateIdB order doesn't matter, the entry itself always stores the smaller id first. */
	FSolarOrbzTectonicBoundaryState& FindOrAddBoundaryState(int32 PlateIdA, int32 PlateIdB);

	/**
	 * The Closure threshold BakePlateTectonics()/UpdateBoundaryStress() classify a plate boundary
	 * convergent/divergent/transform against. Closure is a difference of two GetInstantaneousDrift()
	 * projections, and GetInstantaneousDrift() is Cross(axis, dir) * AngularRateRadPerMyr, so its
	 * magnitude can never exceed roughly the fastest single plate's drift speed, which itself scales
	 * directly with InitialDriftSpeedDegreesPerMyr (converted to rad/Myr). A hardcoded absolute
	 * threshold with no relationship to that speed is either permanently unreachable (every boundary
	 * reads as "transform," contributing zero height, no matter how long the simulation runs) or, at
	 * the opposite extreme, small enough that every boundary reads as convergent/divergent regardless
	 * of the plates' actual relative motion. This was confirmed to be the former, in production, at
	 * the 2.0 deg/Myr default: this class's own 1:1 port in Tools/StandaloneContinentGenerator (same
	 * formula, same defaults - see that tool's ContinentLayer.h/.cpp and its README/ROADMAP entry)
	 * had its live Closure values instrumented directly, clustering around 0.005-0.03 - nowhere near
	 * the 0.25 this threshold used to be hardcoded to, so every boundary there silently fell back to
	 * "transform" no matter what Mountain Height was set to or how long the simulation ran. Scaling
	 * this threshold to the configured drift speed instead keeps it meaningful at any
	 * InitialDriftSpeedDegreesPerMyr value.
	 */
	float ComputeTransformThreshold() const
	{
		const float MaxDriftRad = FMath::DegreesToRadians(InitialDriftSpeedDegreesPerMyr);
		return MaxDriftRad * 0.3f;
	}

	/** Const lookup only (BakePlateTectonics' use case - read accumulated stress, never create an entry for a pair that's never been seen by UpdateBoundaryStress). Returns null if this pair has no recorded history. */
	const FSolarOrbzTectonicBoundaryState* FindBoundaryState(int32 PlateIdA, int32 PlateIdB) const;

	/** See FSolarOrbzTectonicPlate. Empty under the other two algorithms. */
	UPROPERTY()
	TArray<FSolarOrbzTectonicPlate> Plates;

	/** See FSolarOrbzTectonicBoundaryState. Empty until AdvanceTectonicSimulation has run at least one step; ordinary Bake() never adds to this, only reads it. */
	UPROPERTY()
	TArray<FSolarOrbzTectonicBoundaryState> BoundaryStates;

	/** Next FSolarOrbzTectonicPlate::PlateId to hand out - incremented by both EnsurePlatesInitialized (initial seeding) and SplitPlate (each new child), never reused, so a stale FSolarOrbzTectonicBoundaryState can always be told apart from one that legitimately refers to a plate that still exists. */
	UPROPERTY()
	int32 NextPlateId = 0;

	/** Seed value Plates was last (re)seeded from - compared against Seed each EnsurePlatesInitialized() call to detect "the user changed Seed, reroll from scratch," the same way every other algorithm already does. */
	UPROPERTY()
	int32 PlatesSeededFrom = 0;

	/** Num Plates (clamped the same way Bake() clamps it) at the time Plates was last (re)seeded - compared against the CURRENT clamped Num Plates, not Plates.Num() itself, since splitting legitimately grows Plates.Num() beyond this without that meaning "the user asked to reroll." */
	UPROPERTY()
	int32 PlatesSeededFromNumPlates = 0;

	/** True once Plates has been seeded at least once - lets EnsurePlatesInitialized() tell "never seeded yet" apart from "legitimately reseeded at Seed == 0" without a magic sentinel on PlatesSeededFrom. */
	UPROPERTY()
	bool bPlatesEverSeeded = false;

	/**
	 * Stable identity for this layer's tectonic-simulation temp-file backup (see
	 * SaveTectonicBackupToTempFile/TryRestoreTectonicBackupFromTempFile below) - lazily assigned the
	 * same way EditorNodeId is, so renaming/moving the owning asset doesn't orphan an existing backup
	 * file. Only ever read/written by the backup mechanism; never read by GetRawHeight/Bake/anything
	 * evaluation-related.
	 */
	UPROPERTY()
	FGuid TectonicBackupId;

	/** Returns this layer's on-disk backup file path (Saved/SolarOrbzTectonicSimBackups/<TectonicBackupId>.bin), lazily assigning TectonicBackupId first if it isn't valid yet. */
	FString GetTectonicBackupFilePath();

	/**
	 * Writes Plates/BoundaryStates/SimulatedAgeMyr/etc to this layer's backup file - called once at
	 * the end of every AdvanceTectonicSimulation() that actually ran at least one step, so simulation
	 * progress survives an editor crash or restart that happens before the next manual asset save.
	 * Cheap (the whole payload is at most a few hundred small structs) and synchronous - no need for
	 * anything more elaborate for a safety-net write this infrequent.
	 */
	void SaveTectonicBackupToTempFile();

	/**
	 * Called from PostLoad() - reads this layer's backup file (if TectonicBackupId is valid and a
	 * file exists for it) and restores Plates/BoundaryStates/etc from it ONLY if the backup is for the
	 * same seed basis this layer was just loaded with (Seed/PlatesSeededFrom/PlatesSeededFromNumPlates
	 * all match - never apply a backup left over from a since-changed Seed) AND is more advanced
	 * (strictly greater SimulatedAgeMyr) than what the asset itself just loaded - never overwrites
	 * progress that's already safely saved. Logs when it actually recovers something. Safe to call on
	 * a layer that's never been backed up (TectonicBackupId invalid) or has no backup file on disk -
	 * both are the ordinary case and silently no-op.
	 */
	void TryRestoreTectonicBackupFromTempFile();

	/** Set by ApplyPlanetaryContext() each regenerate - null if no Profile was assigned, it wasn't a Planet Profile, ApplyPlanetaryContext() was never called, or bOverrideFromProfile is false. Bake() reads Num Continents/etc through this when valid, falling back to this layer's own authored properties otherwise. Weak, not a hard reference - a Profile's lifetime isn't tied to this layer's. */
	TWeakObjectPtr<const USolarOrbzPlanetProfile> ProfileOverride;

	/** Sea Level, UE units (cm), cached from the last ApplyPlanetaryContext() call - 0 if never called or no ClimateSimulation is assigned. GetRawHeight adds this to the Ocean Floor Depth/Land Plateau Height result, so both are genuinely relative to Sea Level, not just the raw base radius. */
	float CachedSeaLevelCm = 0.0f;

	/** Regenerated each Bake() from Seed/NumContinents/NumIslands/poles when Algorithm == RadialSeeds - GetRawHeight only ever reads this, never regenerates it, so per-vertex cost stays a cheap linear scan. Unused (empty) under the other two algorithms. */
	TArray<FSolarOrbzContinentSeedData> CachedSeeds;

	/** Baked height, UE units (cm) above/below Sea Level, one row-major Width*Height array - populated by Bake() when Algorithm == VoronoiGrowth or PlateTectonics, sampled bilinearly by GetRawHeight via FSolarOrbzLatLongGrid. Empty under RadialSeeds. */
	TArray<float> BakedHeightCm;
	int32 BakedGridWidth = 0;
	int32 BakedGridHeight = 0;

	void BakeRadialSeeds(bool bEffectiveNorthPolar, bool bEffectiveSouthPolar, int32 EffectiveNumContinents, int32 EffectiveNumIslands);
	void BakeVoronoiGrowth(bool bEffectiveNorthPolar, bool bEffectiveSouthPolar, int32 EffectiveNumContinents, int32 EffectiveNumIslands);
	void BakePlateTectonics(bool bEffectiveNorthPolar, bool bEffectiveSouthPolar);

	float GetRawHeightRadialSeeds(const FVector& UnitDirection) const;
	float GetRawHeightFromBakedGrid(const FVector& UnitDirection) const;
};
