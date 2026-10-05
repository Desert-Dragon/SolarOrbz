// SolarOrbz - Terrain Layers subsystem implementation.

#include "SolarOrbzTerrainLayers.h"
#include "Engine/Texture2D.h"
#include "Math/RandomStream.h"
#include "SolarOrbzBiomeSystem.h"
#include "SolarOrbzLatLongGrid.h"
#include "SolarOrbzProfiles.h"
#include "SolarOrbzTerrainGraph.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzErosion, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzTerrace, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzContinent, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzNoise, Log, All);

// ================================================================================================
// USolarOrbzTerrainLayer - GetRawHeight has an inline default; nothing else to implement here.
// ================================================================================================

// ================================================================================================
// USolarOrbzTerrainLayerStack
// ================================================================================================
USolarOrbzTerrainGraph* USolarOrbzTerrainLayerStack::GetOrCreateTerrainGraph()
{
	if (!TerrainGraph)
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Log, TEXT("GetOrCreateTerrainGraph: building the Terrain Graph Editor view for %s for the first time this session"), *GetName());
		TerrainGraph = NewObject<USolarOrbzTerrainGraph>(this, NAME_None, RF_Transient);
		TerrainGraph->RebuildFromLayers(this);
	}
	return TerrainGraph;
}

void USolarOrbzTerrainLayerStack::ApplyPlanetaryContext(const USolarOrbzPlanetProfile* Profile, float SeaLevelCm) const
{
	CachedSeaLevelCmForMasking = SeaLevelCm;

	for (const TObjectPtr<USolarOrbzTerrainLayer>& Layer : Layers)
	{
		if (Layer)
		{
			Layer->ApplyPlanetaryContext(Profile, SeaLevelCm);
		}
	}
}

void USolarOrbzTerrainLayerStack::PrepareLayers(double RadiusCm) const
{
	CachedRadiusCmForMasking = RadiusCm;

	// Populated fully before the Bake() loop below, since Bake() can call back into
	// EvaluateHeightUpTo (via PriorLayersHeight) for any earlier layer, which needs this already
	// filled in for the indices it touches.
	CachedLayerNeedsSlope.SetNum(Layers.Num());
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		const USolarOrbzTerrainLayer* Layer = Layers[i];
		CachedLayerNeedsSlope[i] = Layer && Layer->Mask && Layer->Mask->NeedsSlope();
	}

	// Unconditional (every enabled layer, not just RequiresWholeSurfaceBake() ones) and single-
	// threaded, before EvaluateHeight is ever called for real below - see WarmCaches()'s own comment
	// for why this has to happen here rather than lazily inside GetRawHeight.
	for (const TObjectPtr<USolarOrbzTerrainLayer>& Layer : Layers)
	{
		if (Layer && Layer->bEnabled)
		{
			Layer->WarmCaches();
		}
	}

	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		USolarOrbzTerrainLayer* Layer = Layers[i];
		if (!Layer || !Layer->bEnabled || !Layer->RequiresWholeSurfaceBake())
		{
			continue;
		}

		// Capture by value (this, i) - safe to call from Bake() as many times as it likes, since it
		// only ever reaches back into layers strictly below index i, never itself or anything above it.
		// Deliberately no ClimateGrid - Bake() runs once per regenerate, before Climate Simulation has
		// produced one, so a whole-surface-baked layer's view of any masked layer below it is always
		// the seed-pass (climate-neutral) result, never the final climate-aware one. Accepted limitation
		// of the existing one-bake-per-regenerate design (see RegenerateMesh), not a new correctness gap.
		auto PriorLayersHeight = [this, i](const FVector& UnitDirection, const FVector2D& UV) -> float
		{
			return EvaluateHeightUpTo(i, UnitDirection, UV);
		};

		Layer->Bake(PriorLayersHeight, RadiusCm);
	}
}

float USolarOrbzTerrainLayerStack::EvaluateHeight(const FVector& UnitDirection, const FVector2D& UV, const FSolarOrbzClimateGrid* ClimateGrid) const
{
	return EvaluateHeightUpTo(Layers.Num(), UnitDirection, UV, ClimateGrid);
}

float USolarOrbzTerrainLayerStack::EvaluateHeightUpTo(int32 EndIndexExclusive, const FVector& UnitDirection, const FVector2D& UV, const FSolarOrbzClimateGrid* ClimateGrid) const
{
	auto ApplyBlendMode = [](float Base, float LayerHeight, ESolarOrbzTerrainBlendMode Mode) -> float
	{
		switch (Mode)
		{
		case ESolarOrbzTerrainBlendMode::Add:      return Base + LayerHeight;
		case ESolarOrbzTerrainBlendMode::Subtract: return Base - LayerHeight;
		case ESolarOrbzTerrainBlendMode::Multiply: return Base * LayerHeight;
		case ESolarOrbzTerrainBlendMode::Max:      return FMath::Max(Base, LayerHeight);
		case ESolarOrbzTerrainBlendMode::Min:      return FMath::Min(Base, LayerHeight);
		case ESolarOrbzTerrainBlendMode::Replace:  return LayerHeight;
		}
		return Base;
	};

	float Accum = 0.0f;
	bool bHasAccumulated = false;

	const int32 Count = FMath::Min(EndIndexExclusive, Layers.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		const TObjectPtr<USolarOrbzTerrainLayer>& Layer = Layers[i];
		if (!Layer || !Layer->bEnabled)
		{
			continue;
		}

		const float LayerHeight = Layer->GetRawHeight(UnitDirection, UV) * Layer->Strength;

		// A stack's first enabled layer has nothing to blend against yet - treat it as an implicit
		// Replace regardless of its own BlendMode, the same way a bottom-of-stack layer works in
		// every other layer-stack tool (World Creator included): "blend mode" is only meaningful
		// once there's a real prior value to blend against. Without this, a first layer set to
		// Multiply against an uninitialized Accum of 0.0 would always yield 0; Min/Max would
		// silently floor/clip all terrain to 0; Subtract would silently negate the layer's own
		// output instead of just being it. This is a deliberate, intentional behavior change from
		// the old "Accum starts at 0.0, every BlendMode applies from the first layer onward" logic -
		// any existing planet whose first enabled layer relied on that old Subtract-from-zero
		// behavior for a negative base terrain will now see it inverted; author that with a Strength
		// of -1 (applied before blending) on an Add/Replace layer instead.
		const float FullyAppliedAccum = bHasAccumulated ? ApplyBlendMode(Accum, LayerHeight, Layer->BlendMode) : LayerHeight;

		if (Layer->Mask)
		{
			FSolarOrbzBiomeSampleContext Context;
			Context.UnitDirection = UnitDirection;
			Context.UV = UV;
			Context.Elevation = Accum; // exactly the layers-so-far height - what this layer should treat as "the terrain here" for masking purposes
			Context.SeaLevel = CachedSeaLevelCmForMasking / 100.0f; // cm -> meters, matching FSolarOrbzBiomeSampleContext's documented convention

			// Reads the cache PrepareLayers() populated rather than calling Layer->Mask->NeedsSlope()
			// here directly - for a Composite Mask that walks a whole tree of referenced Biomes, the
			// answer is invariant for the whole regenerate, so it shouldn't be recomputed every vertex.
			if (CachedLayerNeedsSlope.IsValidIndex(i) && CachedLayerNeedsSlope[i])
			{
				Context.Slope = EstimateSlopeUpTo(i, UnitDirection, UV, ClimateGrid);
			}

			if (ClimateGrid && ClimateGrid->IsValid())
			{
				Context.bHasClimateData = true;
				ClimateGrid->Sample(UnitDirection, Context.Temperature, Context.Moisture);
			}

			const float MaskWeight = Layer->Mask->GetWeight(Context);
			// Lerp the RESULT toward FullyAppliedAccum, not the layer's raw height before blending -
			// pre-multiplying LayerHeight by MaskWeight only gives the right "this layer had no effect
			// here" behavior for Add/Subtract; at MaskWeight 0, Replace would still snap to a
			// pre-multiplied 0, and Min/Max would still clip/floor against it. Lerping the already-
			// blended result handles all six blend modes uniformly and correctly.
			Accum = FMath::Lerp(Accum, FullyAppliedAccum, MaskWeight);
		}
		else
		{
			Accum = FullyAppliedAccum;
		}

		bHasAccumulated = true;
	}

	return Accum;
}

bool USolarOrbzTerrainLayerStack::AnyLayerNeedsClimateData() const
{
	for (const TObjectPtr<USolarOrbzTerrainLayer>& Layer : Layers)
	{
		if (Layer && Layer->Mask && Layer->Mask->NeedsClimateData())
		{
			return true;
		}
	}
	return false;
}

bool USolarOrbzTerrainLayerStack::AnyLayerNeedsSlope() const
{
	for (const TObjectPtr<USolarOrbzTerrainLayer>& Layer : Layers)
	{
		if (Layer && Layer->Mask && Layer->Mask->NeedsSlope())
		{
			return true;
		}
	}
	return false;
}

float USolarOrbzTerrainLayerStack::EstimateSlopeUpTo(int32 EndIndexExclusive, const FVector& UnitDirection, const FVector2D& UV, const FSolarOrbzClimateGrid* ClimateGrid) const
{
	// A slope-masked layer earlier in the stack can itself trigger another EstimateSlopeUpTo call
	// (recursively, for a strictly smaller EndIndexExclusive), and each level does 2 EvaluateHeightUpTo
	// calls - so K stacked slope-masked layers cost up to 2^K per vertex without a bound. Cap it the
	// same way USolarOrbzBiome::GetWeight caps Composite Mask cycles: beyond this depth, report flat
	// (0) rather than recursing further - keeps the worst case bounded and just slightly under-masks
	// in the (already exotic) case of many stacked slope-gated layers, instead of hanging.
	static thread_local int32 RecursionDepth = 0;
	constexpr int32 MaxRecursionDepth = 4;
	if (RecursionDepth >= MaxRecursionDepth)
	{
		return 0.0f;
	}

	// Known simplification: a single-tangent-direction finite difference, not a true 2-axis
	// gradient - same spirit as Erosion's own equatorial-only cell spacing approximation. Good
	// enough to distinguish "flat" from "steep" for masking purposes without needing a real mesh
	// normal, which doesn't exist yet mid-Pass-A (RecomputeSmoothNormals only runs after the whole
	// pass finishes).
	FVector Tangent = FVector::CrossProduct(FVector::UpVector, UnitDirection);
	if (!Tangent.Normalize())
	{
		Tangent = FVector::ForwardVector; // degenerate at the poles - arbitrary but deterministic
	}

	constexpr double AngularEpsilonRadians = 0.001; // ~0.057 degrees - small enough to be local, large enough to avoid float noise
	const double RadiusCm = FMath::Max(CachedRadiusCmForMasking, 1.0);
	const FVector PerturbedDirection = (UnitDirection + Tangent * (float)AngularEpsilonRadians).GetSafeNormal();

	// Recomputed for the perturbed point rather than reusing UV verbatim - UnitDirection and UV can
	// disagree here (a real mesh vertex's UV is seam-fixed and may not match the raw spherical
	// formula exactly), but a perturbed point isn't a real vertex at all, so its UV has to be derived
	// from its own direction. Without this, a UV-driven layer (Heightmap Layer, which samples via UV
	// rather than UnitDirection) would see identical UV before/after perturbation and contribute
	// nothing to the slope estimate even on genuinely steep authored terrain.
	const double Azimuth = FMath::Atan2((double)PerturbedDirection.Y, (double)PerturbedDirection.X);
	const double PerturbedU = 0.5 + Azimuth / (2.0 * FSolarOrbzLatLongGrid::PI_D);
	const double PerturbedPolar = FMath::Acos(FMath::Clamp((double)PerturbedDirection.Z, -1.0, 1.0));
	const double PerturbedV = PerturbedPolar / FSolarOrbzLatLongGrid::PI_D;
	const FVector2D PerturbedUV((float)PerturbedU, (float)PerturbedV);

	++RecursionDepth;
	const float HeightHere = EvaluateHeightUpTo(EndIndexExclusive, UnitDirection, UV, ClimateGrid);
	const float HeightPerturbed = EvaluateHeightUpTo(EndIndexExclusive, PerturbedDirection, PerturbedUV, ClimateGrid);
	--RecursionDepth;

	const double ArcLengthCm = RadiusCm * AngularEpsilonRadians;
	const double SlopeRatio = ArcLengthCm > KINDA_SMALL_NUMBER ? FMath::Abs(HeightPerturbed - HeightHere) / ArcLengthCm : 0.0;

	// atan of the rise/run ratio, normalized so a vertical cliff (90 degrees) reads as 1.0 - matches
	// FSolarOrbzBiomeSampleContext::Slope's documented 0 (flat) .. 1 (vertical) convention.
	return FMath::Clamp((float)(FMath::Atan(SlopeRatio) / (PI * 0.5)), 0.0f, 1.0f);
}

// ================================================================================================
// USolarOrbzFractalNoiseTerrainLayerBase
// ================================================================================================
namespace SolarOrbzNoiseBasis
{
	// Deterministic integer hash -> 0..1, used by Value/Voronoi noise below. Not related to
	// Perlin's internal gradient hashing - this is a plain lattice-value hash (Bob Jenkins-style mix).
	static float Hash3D(int32 X, int32 Y, int32 Z, int32 Seed)
	{
		uint32 H = (uint32)(X * 374761393 + Y * 668265263 + Z * 2147483647 + Seed * 3266489917);
		H = (H ^ (H >> 13)) * 1274126177u;
		H = H ^ (H >> 16);
		return (float)(H & 0xFFFFFF) / (float)0xFFFFFF; // 0..1
	}

	// Quintic fade curve (same shape Perlin noise itself uses) - smoother, less grid-aligned-looking
	// than a plain Hermite/smoothstep would give.
	static float Fade(float T)
	{
		return T * T * T * (T * (T * 6.0f - 15.0f) + 10.0f);
	}

	/** 3D value noise: trilinear-interpolates pseudo-random values at the 8 corners of the lattice cell containing Pos, rather than gradients like Perlin - a genuinely different, blockier character. Returns roughly -1..1. */
	static float ValueNoise3D(const FVector& Pos, int32 Seed)
	{
		const int32 X0 = FMath::FloorToInt(Pos.X), Y0 = FMath::FloorToInt(Pos.Y), Z0 = FMath::FloorToInt(Pos.Z);
		const int32 X1 = X0 + 1, Y1 = Y0 + 1, Z1 = Z0 + 1;

		const float Tx = Fade(Pos.X - X0);
		const float Ty = Fade(Pos.Y - Y0);
		const float Tz = Fade(Pos.Z - Z0);

		const float C000 = Hash3D(X0, Y0, Z0, Seed), C100 = Hash3D(X1, Y0, Z0, Seed);
		const float C010 = Hash3D(X0, Y1, Z0, Seed), C110 = Hash3D(X1, Y1, Z0, Seed);
		const float C001 = Hash3D(X0, Y0, Z1, Seed), C101 = Hash3D(X1, Y0, Z1, Seed);
		const float C011 = Hash3D(X0, Y1, Z1, Seed), C111 = Hash3D(X1, Y1, Z1, Seed);

		const float X00 = FMath::Lerp(C000, C100, Tx);
		const float X10 = FMath::Lerp(C010, C110, Tx);
		const float X01 = FMath::Lerp(C001, C101, Tx);
		const float X11 = FMath::Lerp(C011, C111, Tx);
		const float Y0v = FMath::Lerp(X00, X10, Ty);
		const float Y1v = FMath::Lerp(X01, X11, Ty);
		const float Result01 = FMath::Lerp(Y0v, Y1v, Tz); // 0..1

		return Result01 * 2.0f - 1.0f; // -1..1
	}

	/**
	 * 3D cellular (Worley/Voronoi) noise: distance to the nearest randomly-placed feature point in
	 * the surrounding lattice, mapped to roughly -1..1. Produces cell-like patterns rather than
	 * smooth ridges/hills - blobby plateaus with sharper cell boundaries. This same distance-field
	 * technique is the natural building block for a future Continent Layer placing discrete landmasses.
	 */
	static float CellularNoise3D(const FVector& Pos, int32 Seed)
	{
		const int32 CX = FMath::FloorToInt(Pos.X), CY = FMath::FloorToInt(Pos.Y), CZ = FMath::FloorToInt(Pos.Z);

		float MinDistSq = TNumericLimits<float>::Max();

		// The nearest feature point is always within the current lattice cell or a directly
		// adjacent one, so a 3x3x3 neighborhood search is sufficient.
		for (int32 OffZ = -1; OffZ <= 1; ++OffZ)
		{
			for (int32 OffY = -1; OffY <= 1; ++OffY)
			{
				for (int32 OffX = -1; OffX <= 1; ++OffX)
				{
					const int32 CellX = CX + OffX, CellY = CY + OffY, CellZ = CZ + OffZ;

					// One pseudo-random feature point per cell, placed anywhere within that cell.
					const float FX = CellX + Hash3D(CellX, CellY, CellZ, Seed);
					const float FY = CellY + Hash3D(CellX, CellY, CellZ, Seed + 101);
					const float FZ = CellZ + Hash3D(CellX, CellY, CellZ, Seed + 202);

					const float DX = Pos.X - FX, DY = Pos.Y - FY, DZ = Pos.Z - FZ;
					const float DistSq = DX * DX + DY * DY + DZ * DZ;
					MinDistSq = FMath::Min(MinDistSq, DistSq);
				}
			}
		}

		const float Dist = FMath::Sqrt(MinDistSq); // roughly 0..~1.5 for a unit lattice cell
		return FMath::Clamp(Dist * 1.2f, 0.0f, 1.0f) * 2.0f - 1.0f; // normalize to roughly -1..1
	}

	/**
	 * Cheap deterministic hash from an integer seed to a 3D offset, so different seeds don't just
	 * look like the same field shifted by a fixed, obvious amount. Shared by the fractal noise base
	 * (its own field seeding) and Terrace Layer (its coastline-style irregularity jitter) - same
	 * formula, same "seed -> offset" need, previously duplicated verbatim in both places.
	 */
	static FVector ComputeSeedOffset(int32 Seed)
	{
		return FVector(
			FMath::Frac(FMath::Sin((float)Seed * 12.9898f) * 43758.5453f) * 1000.0f,
			FMath::Frac(FMath::Sin((float)Seed * 78.233f) * 43758.5453f) * 1000.0f,
			FMath::Frac(FMath::Sin((float)Seed * 37.719f) * 43758.5453f) * 1000.0f);
	}

	/** Samples one octave using the given basis type. All five return roughly -1..1, so they combine identically in the fractal sum regardless of which is chosen. */
	static float SampleBasis(ESolarOrbzNoiseType Type, const FVector& Pos, int32 Seed)
	{
		switch (Type)
		{
		case ESolarOrbzNoiseType::Ridged:
		{
			const float N = FMath::PerlinNoise3D(Pos);
			float Ridge = 1.0f - FMath::Abs(N); // 0..1, peak (1) where the underlying noise crosses zero
			Ridge *= Ridge; // square to sharpen the ridgelines
			return Ridge * 2.0f - 1.0f; // back to -1..1
		}
		case ESolarOrbzNoiseType::Billow:
		{
			const float N = FMath::PerlinNoise3D(Pos);
			return FMath::Abs(N) * 2.0f - 1.0f; // fold negative lobes upward - rounded, billowy humps instead of symmetric dips
		}
		case ESolarOrbzNoiseType::Value:
			return ValueNoise3D(Pos, Seed);
		case ESolarOrbzNoiseType::Voronoi:
			return CellularNoise3D(Pos, Seed);
		case ESolarOrbzNoiseType::Perlin:
		default:
			return FMath::PerlinNoise3D(Pos);
		}
	}
}

void USolarOrbzFractalNoiseTerrainLayerBase::Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, double RadiusCm)
{
	// PriorLayersHeight is deliberately unused - this Bake() exists purely as a "once per
	// regenerate" hook to calibrate amplitude compensation via Monte Carlo sampling of this
	// layer's OWN noise, not for whole-surface height data the way Erosion/Terrace use it.

	// Depends only on Seed, constant for the whole bake - compute once here rather than
	// recomputing it on every single ComputeNormalizedNoiseUncompensated call (i.e. every vertex).
	CachedSeedOffset = SolarOrbzNoiseBasis::ComputeSeedOffset(Seed);

	if (!bCompensateAmplitude)
	{
		CachedAmplitudeScale = 1.0f;
		return;
	}

	// Target: the standard deviation (typical variation around the mean, decoupled from any
	// constant offset a basis like Ridged/Billow naturally has) of the raw fractal sum should land
	// on this fixed, octave-independent constant, so Amplitude/MaxElevationMeters means "this much
	// typical variation" regardless of how many octaves are stacked. Chosen to closely match plain
	// single-octave Perlin's natural standard deviation (~0.29-0.30 empirically), so a simple,
	// few-octave layer is barely touched by this at all - only heavier octave stacks (which
	// naturally shrink toward the center the more octaves you sum) get meaningfully boosted back up.
	constexpr float TargetStdDev = 0.3f;
	constexpr int32 CalibrationSamples = 256;

	FRandomStream Stream(Seed ^ 0x5A17); // decorrelated from the noise field's own seed offset, still fully deterministic

	TArray<float, TInlineAllocator<CalibrationSamples>> Samples;
	Samples.Reserve(CalibrationSamples);
	double Sum = 0.0;

	for (int32 i = 0; i < CalibrationSamples; ++i)
	{
		const FVector RandomDir = FVector(
			Stream.FRandRange(-1.0f, 1.0f),
			Stream.FRandRange(-1.0f, 1.0f),
			Stream.FRandRange(-1.0f, 1.0f)).GetSafeNormal();

		const float RawSample = ComputeNormalizedNoiseUncompensated(RandomDir);
		Samples.Add(RawSample);
		Sum += RawSample;
	}

	const float Mean = (float)(Sum / CalibrationSamples);
	double SumSquaredDeviation = 0.0;
	for (const float S : Samples)
	{
		const float Deviation = S - Mean;
		SumSquaredDeviation += (double)Deviation * Deviation;
	}
	const float MeasuredStdDev = FMath::Sqrt((float)(SumSquaredDeviation / CalibrationSamples));

	// Clamped to [1, 10] - compensation only ever boosts, never reduces below the noise's natural
	// range, and no sane Octaves/Persistence combination should need anywhere close to a 10x boost;
	// this is a defensive ceiling, not a value expected to actually bind in practice.
	CachedAmplitudeScale = MeasuredStdDev > KINDA_SMALL_NUMBER ? FMath::Clamp(TargetStdDev / MeasuredStdDev, 1.0f, 10.0f) : 1.0f;

	UE_LOG(LogSolarOrbzNoise, Log,
		TEXT("SolarOrbz Noise (%s): amplitude compensation calibrated - measured std dev %.3f, applying %.2fx scale (Octaves=%d, Persistence=%.2f, NoiseType=%d)"),
		*GetName(), MeasuredStdDev, CachedAmplitudeScale, Octaves, Persistence, (int32)NoiseType);
}

float USolarOrbzFractalNoiseTerrainLayerBase::ComputeNormalizedNoise(const FVector& UnitDirection) const
{
	const float Raw = ComputeNormalizedNoiseUncompensated(UnitDirection);
	return bCompensateAmplitude ? FMath::Clamp(Raw * CachedAmplitudeScale, -1.0f, 1.0f) : Raw;
}

float USolarOrbzFractalNoiseTerrainLayerBase::ComputeNormalizedNoiseUncompensated(const FVector& UnitDirection) const
{
	// CachedSeedOffset is computed once per regenerate in Bake() - depends only on Seed, so
	// recomputing it per vertex (the old behavior) was pure wasted sin/Frac work in the hot loop.
	const FVector& SeedOffset = CachedSeedOffset;

	FVector SamplePos = UnitDirection * Frequency + SeedOffset;

	if (WarpStrength > 0.0f)
	{
		// Domain warp always uses Perlin regardless of NoiseType - it's an organic distortion
		// field, not the primary terrain shape, so it doesn't need to match the chosen basis.
		const FVector WarpPos = UnitDirection * WarpFrequency + SeedOffset;
		const FVector Warp(
			FMath::PerlinNoise3D(WarpPos),
			FMath::PerlinNoise3D(WarpPos + FVector(31.7f, 0.0f, 0.0f)),
			FMath::PerlinNoise3D(WarpPos + FVector(0.0f, 57.3f, 0.0f)));
		SamplePos += Warp * WarpStrength;
	}

	float Sum = 0.0f;
	float MaxPossible = 0.0f;
	float OctaveAmplitude = 1.0f;
	FVector OctavePos = SamplePos;

	for (int32 Octave = 0; Octave < Octaves; ++Octave)
	{
		Sum += SolarOrbzNoiseBasis::SampleBasis(NoiseType, OctavePos, Seed) * OctaveAmplitude;
		MaxPossible += OctaveAmplitude;
		OctaveAmplitude *= Persistence;
		OctavePos *= Lacunarity;
	}

	return MaxPossible > KINDA_SMALL_NUMBER ? FMath::Clamp(Sum / MaxPossible, -1.0f, 1.0f) : 0.0f;
}

// ================================================================================================
// USolarOrbzNoiseTerrainLayer
// ================================================================================================
float USolarOrbzNoiseTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	// Genuinely offset by Sea Level (cached via ApplyPlanetaryContext), not just the raw base
	// radius - the system is planet-focused, so Amplitude Meters means "this much above/below sea
	// level", symmetric, same as every other layer with an absolute reference point.
	return ComputeNormalizedNoise(UnitDirection) * AmplitudeMeters * 100.0f + CachedSeaLevelCm; // meters -> UE units (cm), then shift by Sea Level
}

// ================================================================================================
// USolarOrbzPlanetaryNoiseTerrainLayer
// ================================================================================================
float USolarOrbzPlanetaryNoiseTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	const float Normalized = ComputeNormalizedNoise(UnitDirection); // -1..1, zero-centered

	// Above the midpoint scales toward MaxElevationMeters; below scales toward MaxDepthMeters
	// (entered as a positive depth, so this stays negative here since Normalized is negative below).
	const float HeightMetersAboveSeaLevel = Normalized >= 0.0f
		? Normalized * MaxElevationMeters
		: Normalized * MaxDepthMeters;

	// Genuinely offset by Sea Level (cached via ApplyPlanetaryContext), not just the raw base
	// radius - so "8,850m peaks" means 8,850m above wherever Sea Level actually is.
	return HeightMetersAboveSeaLevel * 100.0f + CachedSeaLevelCm; // meters -> UE units (cm), then shift by Sea Level
}

// ================================================================================================
// USolarOrbzHeightmapTerrainLayer
// ================================================================================================
float USolarOrbzHeightmapTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	if (!HeightmapTexture || !Sampler.EnsureDecoded(HeightmapTexture))
	{
		return 0.0f;
	}

	const float Height01 = Sampler.SampleBilinear01(UV.X, UV.Y);
	return FMath::Lerp(MinHeightMeters, MaxHeightMeters, Height01) * 100.0f; // meters -> UE units (cm)
}

// ================================================================================================
// USolarOrbzStampTerrainLayer
// ================================================================================================
FVector USolarOrbzStampTerrainLayer::GetStampDirection() const
{
	const float LatRad = FMath::DegreesToRadians(Latitude);
	const float LongRad = FMath::DegreesToRadians(Longitude);
	const float CosLat = FMath::Cos(LatRad);
	// Matches the mesh's own UV convention: longitude wraps around Z, azimuth = atan2(Y, X).
	return FVector(CosLat * FMath::Cos(LongRad), CosLat * FMath::Sin(LongRad), FMath::Sin(LatRad));
}

float USolarOrbzStampTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	const float AmplitudeCm = AmplitudeMeters * 100.0f; // meters -> UE units (cm)

	const FVector StampDirection = GetStampDirection();
	const float CosAngle = FVector::DotProduct(UnitDirection, StampDirection);
	const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosAngle, -1.0f, 1.0f)));

	if (AngleDeg > AngularRadius)
	{
		return 0.0f;
	}

	const float NormalizedDist = AngleDeg / FMath::Max(AngularRadius, 0.1f); // 0 at center, 1 at edge

	// Fade out over the last EdgeFalloff fraction of the radius so the stamp blends rather than cliffs off.
	float EdgeWeight = 1.0f;
	const float FalloffStart = 1.0f - EdgeFalloff;
	if (NormalizedDist > FalloffStart)
	{
		EdgeWeight = 1.0f - FMath::SmoothStep(FalloffStart, 1.0f, NormalizedDist);
	}

	if (StampHeightmap && Sampler.EnsureDecoded(StampHeightmap))
	{
		// Orthographic-style projection into the stamp's local tangent plane - no lat/long seam
		// concerns since this only ever covers a small patch, not the whole sphere.
		FVector Tangent = FVector::CrossProduct(FVector::UpVector, StampDirection);
		if (!Tangent.Normalize())
		{
			Tangent = FVector::CrossProduct(FVector::ForwardVector, StampDirection).GetSafeNormal();
		}
		const FVector Bitangent = FVector::CrossProduct(StampDirection, Tangent);

		const FVector Local = UnitDirection - StampDirection * CosAngle;
		const float SinRadius = FMath::Sin(FMath::DegreesToRadians(FMath::Max(AngularRadius, 0.1f)));
		float NX = SinRadius > KINDA_SMALL_NUMBER ? FVector::DotProduct(Local, Tangent) / SinRadius : 0.0f;
		float NY = SinRadius > KINDA_SMALL_NUMBER ? FVector::DotProduct(Local, Bitangent) / SinRadius : 0.0f;

		const float RotRad = FMath::DegreesToRadians(StampRotationDegrees);
		const float CosR = FMath::Cos(RotRad);
		const float SinR = FMath::Sin(RotRad);
		const float RX = NX * CosR - NY * SinR;
		const float RY = NX * SinR + NY * CosR;

		const float HeightmapU = 0.5f + 0.5f * RX;
		const float HeightmapV = 0.5f + 0.5f * RY;

		const float Height01 = Sampler.SampleBilinear01(HeightmapU, HeightmapV);
		return Height01 * AmplitudeCm * EdgeWeight;
	}

	// No heightmap - procedural dome (or crater) using a smooth cosine profile so the peak is C1-continuous.
	const float Shape = 0.5f * (1.0f + FMath::Cos(PI * FMath::Clamp(NormalizedDist, 0.0f, 1.0f))); // 1 at center, 0 at edge

	float HeightValue;
	if (!bCrater)
	{
		HeightValue = Shape * AmplitudeCm;
	}
	else
	{
		const float CraterDepth = -Shape * AmplitudeCm;

		float RimShape = 0.0f;
		if (CraterRimHeight > 0.0f)
		{
			constexpr float RimCenter = 0.8f;
			constexpr float RimWidth = 0.15f;
			const float RimT = FMath::Clamp(1.0f - FMath::Abs(NormalizedDist - RimCenter) / RimWidth, 0.0f, 1.0f);
			RimShape = RimT * RimT * (3.0f - 2.0f * RimT); // smoothstep-shaped bump
		}

		HeightValue = CraterDepth + RimShape * CraterRimHeight * AmplitudeCm;
	}

	return HeightValue * EdgeWeight;
}

// ================================================================================================
// USolarOrbzErosionTerrainLayer
// ================================================================================================
namespace SolarOrbzErosion
{
	// Single canonical copy now lives on FSolarOrbzLatLongGrid - aliased here so the many other
	// SolarOrbzErosion::PI_D call sites throughout this file (Terrace, Continent, Erosion alike)
	// don't all need renaming.
	static constexpr double PI_D = FSolarOrbzLatLongGrid::PI_D;

	// 8-neighbor (D8) offsets on the (X = longitude, Y = latitude) grid.
	static constexpr int32 NeighborOffsetX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
	static constexpr int32 NeighborOffsetY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

	// Wraps a longitude index around the seam; latitude has no wrap (poles are edges, not a loop).
	static int32 WrapX(int32 X, int32 W) { return ((X % W) + W) % W; }
	static bool InBoundsY(int32 Y, int32 H) { return Y >= 0 && Y < H; }

	/** Finds the lowest of a cell's 8 neighbors. Returns INDEX_NONE if this cell is already a local minimum. */
	static int32 FindLowestNeighbor(const TArray<float>& Height, int32 X, int32 Y, int32 W, int32 H, float SelfHeight)
	{
		int32 LowestIdx = INDEX_NONE;
		float LowestHeight = SelfHeight;

		for (int32 N = 0; N < 8; ++N)
		{
			const int32 NX = WrapX(X + NeighborOffsetX[N], W);
			const int32 NY = Y + NeighborOffsetY[N];
			if (!InBoundsY(NY, H))
			{
				continue;
			}

			const int32 NIdx = NY * W + NX;
			if (Height[NIdx] < LowestHeight)
			{
				LowestHeight = Height[NIdx];
				LowestIdx = NIdx;
			}
		}

		return LowestIdx;
	}
}

void USolarOrbzErosionTerrainLayer::Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, double RadiusCm)
{
	using namespace SolarOrbzErosion;

	const int32 W = FMath::Max(GridWidth, 8);
	const int32 H = FMath::Max(GridHeight, 4);

	BakedWidth = W;
	BakedHeight = H;

	TArray<float> Height;
	Height.SetNumUninitialized(W * H);
	TArray<float> OriginalHeight;
	OriginalHeight.SetNumUninitialized(W * H);

	// --- Seed the bake grid from every layer below this one in the stack (see USolarOrbzTerrainLayerStack::PrepareLayers). ---
	FSolarOrbzLatLongGrid(W, H).ForEachCell([&](int32 Idx, const FVector& UnitDirection, const FVector2D& UV)
	{
		const float H0 = PriorLayersHeight(UnitDirection, UV);
		Height[Idx] = H0;
		OriginalHeight[Idx] = H0;
	});

	// Approximate physical spacing between adjacent grid cells at the equator - a uniform stand-in
	// for talus/slope thresholds everywhere on the grid (see the pole-accuracy note at the top of the header).
	const double CellSpacingCm = (2.0 * PI_D * FMath::Max(RadiusCm, 1.0)) / (double)W;

	// --- Thermal erosion: material above the talus angle slides toward its lowest neighbor. ---
	if (bEnableThermalErosion && ThermalIterations > 0)
	{
		const float TalusHeightPerCell = FMath::Tan(FMath::DegreesToRadians(TalusAngleDegrees)) * (float)CellSpacingCm;

		TArray<float> Delta;
		for (int32 Iter = 0; Iter < ThermalIterations; ++Iter)
		{
			// Accumulated into a separate buffer and applied after the full pass, so the order cells
			// happen to be visited in doesn't bias which direction material moves this iteration.
			Delta.Init(0.0f, W * H);

			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					const int32 Idx = Y * W + X;
					const float SelfHeight = Height[Idx];

					const int32 LowestIdx = FindLowestNeighbor(Height, X, Y, W, H, SelfHeight);
					if (LowestIdx == INDEX_NONE)
					{
						continue;
					}

					const float Diff = SelfHeight - Height[LowestIdx];
					if (Diff > TalusHeightPerCell)
					{
						const float Transfer = ThermalStrength * (Diff - TalusHeightPerCell) * 0.5f;
						Delta[Idx] -= Transfer;
						Delta[LowestIdx] += Transfer;
					}
				}
			}

			for (int32 Idx = 0; Idx < Height.Num(); ++Idx)
			{
				Height[Idx] += Delta[Idx];
			}
		}
	}

	// --- Hydraulic erosion: water routed downhill (steepest descent) each pass, carving where flow ---
	// is strong and depositing where it stalls.
	if (bEnableHydraulicErosion && HydraulicIterations > 0)
	{
		TArray<float> Water;
		TArray<float> Sediment;
		TArray<int32> SortedIndices;
		SortedIndices.SetNumUninitialized(W * H);

		for (int32 Iter = 0; Iter < HydraulicIterations; ++Iter)
		{
			Water.Init(RainfallAmount, W * H);
			Sediment.Init(0.0f, W * H);

			for (int32 i = 0; i < SortedIndices.Num(); ++i)
			{
				SortedIndices[i] = i;
			}
			// Highest first - by the time a cell is processed, every higher neighbor that could flow
			// into it this pass already has, so water/sediment only ever moves downhill in one pass.
			SortedIndices.Sort([&Height](int32 A, int32 B) { return Height[A] > Height[B]; });

			for (const int32 Idx : SortedIndices)
			{
				const int32 X = Idx % W;
				const int32 Y = Idx / W;
				const float SelfHeight = Height[Idx];

				const int32 LowestIdx = FindLowestNeighbor(Height, X, Y, W, H, SelfHeight);

				const float FlowWater = Water[Idx];
				float FlowSediment = Sediment[Idx];

				if (LowestIdx == INDEX_NONE)
				{
					// Local basin - nowhere lower to go. Drop everything being carried here.
					Height[Idx] += FlowSediment;
					continue;
				}

				const float Slope = FMath::Max((SelfHeight - Height[LowestIdx]) / (float)CellSpacingCm, 0.0f);
				const float Capacity = FlowWater * Slope * ErosionRate;

				if (FlowSediment < Capacity)
				{
					// Room to carry more - carve material out of this cell and pick it up. Never carve
					// past the neighbor's height, so a single step can't invert the slope it's carving along.
					const float Carve = FMath::Min(Capacity - FlowSediment, SelfHeight - Height[LowestIdx]);
					Height[Idx] -= Carve;
					FlowSediment += Carve;
				}
				else
				{
					// Overloaded - drop the excess here.
					const float Deposit = (FlowSediment - Capacity) * DepositionRate;
					Height[Idx] += Deposit;
					FlowSediment -= Deposit;
				}

				Water[LowestIdx] += FlowWater;
				Sediment[LowestIdx] += FlowSediment;
			}
		}
	}

	// --- Store the net change, not the absolute height - GetRawHeight returns a DELTA that gets ---
	// added on top of the same layers it eroded, via the stack's normal blend logic.
	BakedDeltaHeightCm.SetNumUninitialized(W * H);
	float MinDelta = TNumericLimits<float>::Max(), MaxDelta = TNumericLimits<float>::Lowest();
	for (int32 Idx = 0; Idx < Height.Num(); ++Idx)
	{
		const float Delta = Height[Idx] - OriginalHeight[Idx];
		BakedDeltaHeightCm[Idx] = Delta;
		MinDelta = FMath::Min(MinDelta, Delta);
		MaxDelta = FMath::Max(MaxDelta, Delta);
	}

	UE_LOG(LogSolarOrbzErosion, Log,
		TEXT("SolarOrbz Erosion: baked %dx%d grid (Thermal=%s x%d, Hydraulic=%s x%d) - delta height ranges %.1fcm (deposited) .. %.1fcm (carved: %.1fcm)"),
		W, H,
		bEnableThermalErosion ? TEXT("on") : TEXT("off"), ThermalIterations,
		bEnableHydraulicErosion ? TEXT("on") : TEXT("off"), HydraulicIterations,
		MaxDelta, MinDelta, -MinDelta);

	if (FMath::IsNearlyEqual(MinDelta, MaxDelta))
	{
		UE_LOG(LogSolarOrbzErosion, Warning,
			TEXT("SolarOrbz Erosion: delta height is completely flat (%.3fcm everywhere) - either both passes are disabled, iterations are 0, or the terrain below this layer has no elevation variation for erosion to act on."),
			MinDelta);
	}
}

float USolarOrbzErosionTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	if (BakedWidth <= 0 || BakedHeight <= 0 || BakedDeltaHeightCm.Num() != BakedWidth * BakedHeight)
	{
		return 0.0f; // Not baked yet (e.g. layer just added and RegenerateMesh hasn't run) - contribute nothing rather than garbage.
	}

	// Derived directly from UnitDirection rather than trusting the caller's UV, so this is robust
	// to any seam-fixing quirks the mesh's own UVs might have near the poles/seam.
	return FSolarOrbzLatLongGrid(BakedWidth, BakedHeight).SampleBilinear(BakedDeltaHeightCm, UnitDirection);
}

// ================================================================================================
// USolarOrbzTerraceTerrainLayer
// ================================================================================================
namespace SolarOrbzTerrace
{
	/**
	 * Quantizes Height into flat plateaus StepHeightCm apart, with a smooth ramp up to the next
	 * plateau over the top EdgeSoftness01 fraction of each step band. EdgeSoftness01=0 gives (nearly)
	 * sharp stair-step cliffs; EdgeSoftness01=1 gives a continuous ramp - i.e. no visible terracing.
	 */
	static float ApplyTerrace(float Height, float StepHeightCm, float EdgeSoftness01)
	{
		if (StepHeightCm <= KINDA_SMALL_NUMBER)
		{
			return Height;
		}

		const float StepIndex = FMath::FloorToFloat(Height / StepHeightCm);
		const float LocalFrac = Height / StepHeightCm - StepIndex; // 0..1 position within this step's band

		const float RampWidth = FMath::Clamp(EdgeSoftness01, 0.001f, 1.0f);
		const float RampStart = 1.0f - RampWidth;

		float SteppedFrac;
		if (LocalFrac <= RampStart)
		{
			SteppedFrac = 0.0f; // flat plateau
		}
		else
		{
			const float RampT = (LocalFrac - RampStart) / RampWidth; // 0..1 across the ramp to the next plateau
			SteppedFrac = FMath::SmoothStep(0.0f, 1.0f, RampT);
		}

		return (StepIndex + SteppedFrac) * StepHeightCm;
	}
}

void USolarOrbzTerraceTerrainLayer::Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, double RadiusCm)
{
	using namespace SolarOrbzTerrace;

	const int32 W = FMath::Max(GridWidth, 8);
	const int32 H = FMath::Max(GridHeight, 4);

	BakedWidth = W;
	BakedHeight = H;
	BakedDeltaHeightCm.SetNumUninitialized(W * H);

	const float StepHeightCm = StepHeightMeters * 100.0f; // meters -> UE units (cm)
	const FVector IrregularitySeedOffset = SolarOrbzNoiseBasis::ComputeSeedOffset(IrregularitySeed);

	float MinDelta = TNumericLimits<float>::Max(), MaxDelta = TNumericLimits<float>::Lowest();

	FSolarOrbzLatLongGrid(W, H).ForEachCell([&](int32 Idx, const FVector& UnitDirection, const FVector2D& UV)
	{
		const float H0 = PriorLayersHeight(UnitDirection, UV);

		float HeightForTerracing = H0;
		if (IrregularityStrength > 0.0f)
		{
			const float Jitter = FMath::PerlinNoise3D(UnitDirection * IrregularityFrequency + IrregularitySeedOffset);
			HeightForTerracing += Jitter * IrregularityStrength * StepHeightCm;
		}

		const float Terraced = ApplyTerrace(HeightForTerracing, StepHeightCm, EdgeSoftness);
		const float Final = FMath::Lerp(H0, Terraced, TerraceStrength);
		const float Delta = Final - H0;

		BakedDeltaHeightCm[Idx] = Delta;
		MinDelta = FMath::Min(MinDelta, Delta);
		MaxDelta = FMath::Max(MaxDelta, Delta);
	});

	UE_LOG(LogSolarOrbzTerrace, Log,
		TEXT("SolarOrbz Terrace: baked %dx%d grid (Step Height %.0fm, Edge Softness %.2f, Strength %.2f, Irregularity %.2f) - delta height ranges %.1fcm .. %.1fcm"),
		W, H, StepHeightMeters, EdgeSoftness, TerraceStrength, IrregularityStrength, MinDelta, MaxDelta);

	if (FMath::IsNearlyEqual(MinDelta, MaxDelta))
	{
		UE_LOG(LogSolarOrbzTerrace, Warning,
			TEXT("SolarOrbz Terrace: delta height is completely flat (%.3fcm everywhere) - either Terrace Strength is 0, or the terrain below this layer has no elevation variation for terracing to act on."),
			MinDelta);
	}
}

float USolarOrbzTerraceTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	if (BakedWidth <= 0 || BakedHeight <= 0 || BakedDeltaHeightCm.Num() != BakedWidth * BakedHeight)
	{
		return 0.0f; // not baked yet
	}

	return FSolarOrbzLatLongGrid(BakedWidth, BakedHeight).SampleBilinear(BakedDeltaHeightCm, UnitDirection);
}

// ================================================================================================
// USolarOrbzCanyonTerrainLayer
// ================================================================================================
USolarOrbzCanyonTerrainLayer::USolarOrbzCanyonTerrainLayer()
{
	// Ridged is the useful default here - true ridgeline-following canyons. Other bases still work
	// (e.g. Voronoi carves along cell boundaries instead), just a less "canyon-like" default look.
	NoiseType = ESolarOrbzNoiseType::Ridged;
}

float USolarOrbzCanyonTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	const float N = ComputeNormalizedNoise(UnitDirection); // -1..1, using whichever Noise Type is selected

	// Canyons carve only in a narrow band near the noise's peak (its ridgelines), not across the
	// whole terrain - CanyonWidth controls how much of that peak range actually carves.
	const float Threshold = 1.0f - FMath::Clamp(CanyonWidth, 0.001f, 1.0f) * 2.0f;
	if (N <= Threshold)
	{
		return 0.0f; // outside any canyon here
	}

	const float T = (N - Threshold) / FMath::Max(1.0f - Threshold, KINDA_SMALL_NUMBER); // 0..1 across the canyon band
	const float Carve = FMath::Pow(T, FMath::Max(Sharpness, 0.01f)); // shapes the canyon's cross-section profile

	return -Carve * DepthMeters * 100.0f; // negative = carves down; meters -> UE units (cm)
}

// ================================================================================================
// USolarOrbzContinentTerrainLayer
// ================================================================================================
namespace SolarOrbzContinent
{
	/** Uniformly-distributed random point on the unit sphere - NOT naive random lat/long, which clusters heavily at the poles. */
	static FVector RandomPointOnUnitSphere(FRandomStream& Stream)
	{
		const float Z = Stream.FRandRange(-1.0f, 1.0f);
		const float Theta = Stream.FRandRange(0.0f, 2.0f * (float)SolarOrbzErosion::PI_D);
		const float R = FMath::Sqrt(FMath::Max(1.0f - Z * Z, 0.0f));
		return FVector(R * FMath::Cos(Theta), R * FMath::Sin(Theta), Z);
	}

	/** Multi-octave fBm, same basis/accumulation convention as USolarOrbzFractalNoiseTerrainLayerBase::ComputeNormalizedNoiseUncompensated (always Perlin basis here - coastlines don't need a NoiseType picker of their own), normalized to roughly -1..1 regardless of octave count. */
	static float FractalNoise3D(const FVector& Pos, int32 Octaves, float Persistence, float Lacunarity, int32 Seed)
	{
		float Sum = 0.0f;
		float MaxPossible = 0.0f;
		float OctaveAmplitude = 1.0f;
		FVector OctavePos = Pos;
		for (int32 Octave = 0; Octave < FMath::Max(Octaves, 1); ++Octave)
		{
			Sum += SolarOrbzNoiseBasis::SampleBasis(ESolarOrbzNoiseType::Perlin, OctavePos, Seed) * OctaveAmplitude;
			MaxPossible += OctaveAmplitude;
			OctaveAmplitude *= Persistence;
			OctavePos *= Lacunarity;
		}
		return MaxPossible > KINDA_SMALL_NUMBER ? FMath::Clamp(Sum / MaxPossible, -1.0f, 1.0f) : 0.0f;
	}

	/** 4-connected neighbor cell indices for Idx on a GridW x GridH FSolarOrbzLatLongGrid - wraps longitude (X), clamps latitude (Y), matching that grid's own convention. Shared by BakeVoronoiGrowth and BakePlateTectonics below, each of which otherwise builds an unrelated per-cell dataset. */
	static void GetGridNeighbors4(int32 Idx, int32 GridW, int32 GridH, int32 (&OutNeighbors)[4])
	{
		const int32 X = Idx % GridW;
		const int32 Y = Idx / GridW;
		OutNeighbors[0] = Y * GridW + (X + 1) % GridW;
		OutNeighbors[1] = Y * GridW + (X - 1 + GridW) % GridW;
		OutNeighbors[2] = FMath::Clamp(Y + 1, 0, GridH - 1) * GridW + X;
		OutNeighbors[3] = FMath::Clamp(Y - 1, 0, GridH - 1) * GridW + X;
	}

	/** Nearest grid cell index for an arbitrary direction - good enough for picking a growth/plate seed's starting cell, not a substitute for FSolarOrbzLatLongGrid::SampleBilinear's actual interpolated sampling. */
	static int32 DirectionToNearestCellIdx(const FSolarOrbzLatLongGrid& Grid, const FVector& Dir)
	{
		const FSolarOrbzLatLongGrid::FBilinearCell Cell = Grid.ComputeBilinearCell(Dir);
		const int32 X = Cell.Tx < 0.5f ? Cell.X0 : Cell.X1;
		const int32 Y = Cell.Ty < 0.5f ? Cell.Y0 : Cell.Y1;
		return Y * Grid.Width + X;
	}
}

void USolarOrbzContinentTerrainLayer::ApplyPlanetaryContext(const USolarOrbzPlanetProfile* Profile, float SeaLevelCm)
{
	// Deliberately not written into this layer's own NumContinents/etc UPROPERTY fields - see the
	// warning in the class comment about why (shared-asset corruption). Bake() below resolves the
	// effective values from this cached pointer each regenerate instead.
	ProfileOverride = (bOverrideFromProfile && Profile) ? Profile : nullptr;

	// Sea Level offsetting always applies regardless of bOverrideFromProfile - it's a units/
	// reference-point correction (what "0" means), not a per-planet data override the way the
	// landmass counts above are.
	CachedSeaLevelCm = SeaLevelCm;
}

void USolarOrbzContinentTerrainLayer::Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, double RadiusCm)
{
	// PriorLayersHeight is deliberately unused by every algorithm below - seed/growth/plate
	// positions don't depend on anything below this layer in the stack. Bake() is only used as a
	// "once per regenerate" hook to regenerate the layout deterministically; Radial Seeds doesn't
	// even need the whole-surface pass this hook provides (see RequiresWholeSurfaceBake()).
	const USolarOrbzPlanetProfile* EffectiveProfile = ProfileOverride.Get();

	// Resolve once, here, rather than reading the authored properties directly below - this is the
	// one and only place Profile overriding actually happens; everything past this point behaves
	// identically whether these came from a Profile or from this layer's own fields. Plate
	// Tectonics ignores both counts entirely (continents emerge from the plate partition instead),
	// but resolving them unconditionally here costs nothing and keeps this one block the only place
	// that ever needs to know about Profile overriding.
	const int32 EffectiveNumContinents = EffectiveProfile ? EffectiveProfile->GetNumContinents() : NumContinents;
	const int32 EffectiveNumIslands = EffectiveProfile ? EffectiveProfile->GetNumIslands() : NumIslands;
	const bool bEffectiveNorthPolar = EffectiveProfile ? EffectiveProfile->HasNorthPolarContinent() : bHasNorthPolarContinent;
	const bool bEffectiveSouthPolar = EffectiveProfile ? EffectiveProfile->HasSouthPolarContinent() : bHasSouthPolarContinent;

	CachedSeeds.Reset();
	BakedHeightCm.Reset();
	BakedGridWidth = 0;
	BakedGridHeight = 0;

	switch (Algorithm)
	{
	case ESolarOrbzContinentAlgorithm::VoronoiGrowth:
		BakeVoronoiGrowth(bEffectiveNorthPolar, bEffectiveSouthPolar, EffectiveNumContinents, EffectiveNumIslands);
		break;
	case ESolarOrbzContinentAlgorithm::PlateTectonics:
		BakePlateTectonics(bEffectiveNorthPolar, bEffectiveSouthPolar);
		break;
	case ESolarOrbzContinentAlgorithm::RadialSeeds:
	default:
		BakeRadialSeeds(bEffectiveNorthPolar, bEffectiveSouthPolar, EffectiveNumContinents, EffectiveNumIslands);
		break;
	}

	const TCHAR* AlgorithmName = TEXT("Radial Seeds");
	if (Algorithm == ESolarOrbzContinentAlgorithm::VoronoiGrowth) AlgorithmName = TEXT("Voronoi Growth");
	else if (Algorithm == ESolarOrbzContinentAlgorithm::PlateTectonics) AlgorithmName = TEXT("Plate Tectonics");

	// Cheap diagnostic: estimate land coverage by sampling a small independent grid - not the same
	// grid resolution concept Voronoi Growth/Plate Tectonics's own Bake Grid is, just a coarse
	// average purely for this log line, same cost/purpose regardless of which algorithm just ran
	// (GetRawHeight below is already virtual-dispatched to whichever one it is).
	constexpr int32 CoverageSamplesW = 64, CoverageSamplesH = 32;
	int32 LandSamples = 0;
	FSolarOrbzLatLongGrid(CoverageSamplesW, CoverageSamplesH).ForEachCell([&](int32 Idx, const FVector& Dir, const FVector2D& UV)
	{
		if (GetRawHeight(Dir, UV) > 0.0f)
		{
			++LandSamples;
		}
	});
	const float LandCoveragePercent = 100.0f * LandSamples / (float)(CoverageSamplesW * CoverageSamplesH);

	UE_LOG(LogSolarOrbzContinent, Log,
		TEXT("SolarOrbz Continent (%s): approx %.1f%% land coverage (values from %s)"),
		AlgorithmName, LandCoveragePercent,
		EffectiveProfile ? TEXT("Planet Profile") : TEXT("this layer's own authored properties"));

	if (Algorithm == ESolarOrbzContinentAlgorithm::RadialSeeds && CachedSeeds.Num() == 0)
	{
		UE_LOG(LogSolarOrbzContinent, Warning, TEXT("SolarOrbz Continent: no continents, islands, or polar continents configured - the whole planet will be Ocean Floor Depth."));
	}
}

void USolarOrbzContinentTerrainLayer::BakeRadialSeeds(bool bEffectiveNorthPolar, bool bEffectiveSouthPolar, int32 EffectiveNumContinents, int32 EffectiveNumIslands)
{
	using namespace SolarOrbzContinent;

	FRandomStream Stream(Seed);
	const float PolarRadiusRadians = FMath::DegreesToRadians(PolarContinentRadiusDegrees);

	int32 SeedIndex = 0;
	auto AddSeedAt = [&](const FVector& Direction, float RadiusRadians) -> FSolarOrbzContinentSeedData
	{
		FSolarOrbzContinentSeedData S;
		S.Direction = Direction;
		S.RadiusRadians = RadiusRadians;
		// Small deterministic per-seed offset so each landmass's coastline noise looks independent
		// rather than every coastline sharing the exact same wiggle pattern.
		S.NoiseOffset = FVector(SeedIndex * 17.3f, SeedIndex * 29.7f, SeedIndex * 53.1f);
		CachedSeeds.Add(S);
		++SeedIndex;
		return S;
	};
	auto AddSeed = [&](const FVector& Direction, float MinRadiusDegrees, float MaxRadiusDegrees) -> FSolarOrbzContinentSeedData
	{
		return AddSeedAt(Direction, FMath::DegreesToRadians(Stream.FRandRange(MinRadiusDegrees, MaxRadiusDegrees)));
	};

	// A seed "steps on the toes" of an enabled polar continent when the great-circle distance
	// between their centers is less than the sum of their angular radii - i.e. the two landmasses
	// actually overlap, not just come close.
	auto OverlapsEnabledPole = [&](const FSolarOrbzContinentSeedData& S)
	{
		if (bEffectiveNorthPolar)
		{
			const float AngleToNorth = FMath::Acos(FMath::Clamp(FVector::DotProduct(S.Direction, FVector::UpVector), -1.0f, 1.0f));
			if (AngleToNorth < S.RadiusRadians + PolarRadiusRadians)
			{
				return true;
			}
		}
		if (bEffectiveSouthPolar)
		{
			const float AngleToSouth = FMath::Acos(FMath::Clamp(FVector::DotProduct(S.Direction, -FVector::UpVector), -1.0f, 1.0f));
			if (AngleToSouth < S.RadiusRadians + PolarRadiusRadians)
			{
				return true;
			}
		}
		return false;
	};

	// Scatters SubSeedsPerLandmass smaller "metaball" seeds around Parent (a just-placed continent/
	// island) - the union of several overlapping circles (GetRawHeightRadialSeeds already takes the
	// max influence across every seed, so this costs nothing extra there) reads far less like a
	// single circle than one circle alone. Skips (rather than rejection-samples) any sub-seed that
	// would overlap an enabled pole, since a parent that already passed that check can still have
	// its own edge close enough to a pole that an outward-scattered sub-seed pokes into it - a
	// slightly smaller-than-authored landmass there is a fine trade for never breaking the
	// pole-overlap guarantee. Never called for the two fixed polar continents themselves.
	auto AddSubSeeds = [&](const FSolarOrbzContinentSeedData& Parent)
	{
		for (int32 i = 0; i < SubSeedsPerLandmass; ++i)
		{
			const FVector RandomPoint = RandomPointOnUnitSphere(Stream);
			FVector Tangent = RandomPoint - Parent.Direction * FVector::DotProduct(RandomPoint, Parent.Direction);
			if (Tangent.IsNearlyZero())
			{
				continue; // degenerate (RandomPoint landed ~= +/-Parent.Direction) - just skip this one sub-seed, not worth retrying
			}
			Tangent = Tangent.GetSafeNormal();

			const float OffsetRadians = Stream.FRandRange(0.0f, SubSeedScatterFraction) * Parent.RadiusRadians;
			const FVector SubDirection = (Parent.Direction * FMath::Cos(OffsetRadians) + Tangent * FMath::Sin(OffsetRadians)).GetSafeNormal();
			const float SubRadiusRadians = Parent.RadiusRadians * Stream.FRandRange(MinSubSeedRadiusFraction, MaxSubSeedRadiusFraction);

			const FSolarOrbzContinentSeedData SubSeed = AddSeedAt(SubDirection, SubRadiusRadians);
			if (OverlapsEnabledPole(SubSeed))
			{
				CachedSeeds.Pop();
				--SeedIndex;
			}
		}
	};

	// Places a randomly-positioned continent/island seed, re-rolling its position+radius together
	// (same as a normal roll) up to MaxPlacementAttempts times if it overlaps an enabled N/S polar
	// continent - the actual "don't step on the pole continent's toes" restriction. Gives up and
	// keeps the last roll anyway (logging a warning) rather than looping forever on a degenerate
	// setup (e.g. a Polar Continent Radius large enough to leave nowhere left on that pole's side of
	// the sphere). A no-op when neither pole is enabled - OverlapsEnabledPole() is trivially false,
	// so this costs zero extra Stream draws and existing continent layouts (seeded without poles) are
	// unaffected by this restriction.
	constexpr int32 MaxPlacementAttempts = 32;
	auto AddRandomSeedClearOfPoles = [&](float MinRadiusDegrees, float MaxRadiusDegrees)
	{
		for (int32 Attempt = 0; Attempt < MaxPlacementAttempts; ++Attempt)
		{
			const FVector Direction = RandomPointOnUnitSphere(Stream);
			const FSolarOrbzContinentSeedData Placed = AddSeed(Direction, MinRadiusDegrees, MaxRadiusDegrees);
			if (!OverlapsEnabledPole(Placed))
			{
				AddSubSeeds(Placed);
				return;
			}
			if (Attempt + 1 == MaxPlacementAttempts)
			{
				UE_LOG(LogSolarOrbzContinent, Warning,
					TEXT("SolarOrbz Continent: couldn't find a seed position clear of the polar continent(s) after %d attempts - keeping this one overlapping anyway. Shrink Polar Continent Radius, or this layer's continent/island radius range, if that keeps happening."),
					MaxPlacementAttempts);
				AddSubSeeds(Placed);
				return;
			}
			CachedSeeds.Pop();
			--SeedIndex;
		}
	};

	for (int32 i = 0; i < EffectiveNumContinents; ++i)
	{
		AddRandomSeedClearOfPoles(MinContinentRadiusDegrees, MaxContinentRadiusDegrees);
	}
	for (int32 i = 0; i < EffectiveNumIslands; ++i)
	{
		AddRandomSeedClearOfPoles(MinIslandRadiusDegrees, MaxIslandRadiusDegrees);
	}
	if (bEffectiveNorthPolar)
	{
		AddSeed(FVector(0.0f, 0.0f, 1.0f), PolarContinentRadiusDegrees, PolarContinentRadiusDegrees);
	}
	if (bEffectiveSouthPolar)
	{
		AddSeed(FVector(0.0f, 0.0f, -1.0f), PolarContinentRadiusDegrees, PolarContinentRadiusDegrees);
	}
}

void USolarOrbzContinentTerrainLayer::BakeVoronoiGrowth(bool bEffectiveNorthPolar, bool bEffectiveSouthPolar, int32 EffectiveNumContinents, int32 EffectiveNumIslands)
{
	using namespace SolarOrbzContinent;

	const int32 GridW = FMath::Max(BakeGridWidth, 8);
	const int32 GridH = FMath::Max(BakeGridHeight, 4);
	BakedGridWidth = GridW;
	BakedGridHeight = GridH;
	const int32 NumCells = GridW * GridH;
	const FSolarOrbzLatLongGrid Grid(GridW, GridH);

	TArray<FVector> CellDirections;
	CellDirections.SetNumUninitialized(NumCells);
	Grid.ForEachCell([&](int32 Idx, const FVector& Dir, const FVector2D& UV) { CellDirections[Idx] = Dir; });

	FRandomStream Stream(Seed);

	TArray<int32> OwnerId;
	OwnerId.Init(-1, NumCells);
	TArray<float> OwnerEnergyRemaining; // per-cell: this cell's own remaining energy at the moment its growth claimed it
	OwnerEnergyRemaining.Init(0.0f, NumCells);
	TArray<float> OwnerStartEnergy; // per-owner (indexed by OwnerId), always 1.0 here - see StopThresholdForRadius below for why that's fine

	// Random-order flood fill from StartIdx: claims OwnerId for every cell it reaches, decaying
	// energy (starting at 1.0) by a randomized factor (Growth Decay Min/Max plus Growth Jitter) each
	// hop, until energy drops to/below StopThreshold or there's nowhere unclaimed left to grow into.
	// Picking a uniformly random cell from the CURRENT FRONTIER each step - not strict ring-by-ring
	// BFS - is what actually breaks radial symmetry into organic, non-convex blobs (see Azgaar's
	// Fantasy Map Generator, referenced on the class comment above); plain same-decay BFS on a
	// uniform grid would still grow in nearly perfect rings, right back to looking circular.
	auto GrowLandmass = [&](int32 StartIdx, float StopThreshold) -> bool
	{
		if (OwnerId[StartIdx] != -1)
		{
			return false;
		}

		const int32 OwnerTag = OwnerStartEnergy.Add(1.0f);
		OwnerId[StartIdx] = OwnerTag;
		OwnerEnergyRemaining[StartIdx] = 1.0f;

		struct FFrontierEntry { int32 Idx; float Energy; };
		TArray<FFrontierEntry> Frontier;
		Frontier.Add({ StartIdx, 1.0f });

		while (Frontier.Num() > 0)
		{
			const int32 PickIndex = Stream.RandRange(0, Frontier.Num() - 1);
			const FFrontierEntry Current = Frontier[PickIndex];
			Frontier.RemoveAtSwap(PickIndex, 1, EAllowShrinking::No);

			int32 Neighbors[4];
			GetGridNeighbors4(Current.Idx, GridW, GridH, Neighbors);
			for (int32 NeighborIdx : Neighbors)
			{
				if (OwnerId[NeighborIdx] != -1)
				{
					continue; // claimed by this growth or an earlier one (poles included) - never cross into it
				}

				const float Decay = FMath::Clamp(Stream.FRandRange(GrowthDecayMin, GrowthDecayMax) + Stream.FRandRange(-GrowthJitter, GrowthJitter), 0.0f, 1.0f);
				const float NextEnergy = Current.Energy * Decay;
				if (NextEnergy <= StopThreshold)
				{
					continue; // decayed to nothing - this branch of the growth stops here
				}

				OwnerId[NeighborIdx] = OwnerTag;
				OwnerEnergyRemaining[NeighborIdx] = NextEnergy;
				Frontier.Add({ NeighborIdx, NextEnergy });
			}
		}
		return true;
	};

	// Converts an authored "radius, degrees" into a stop-energy threshold so Radial Seeds and
	// Voronoi Growth stay authored in the same units despite growing completely differently -
	// starting energy is always 1.0 above, so only the AVERAGE decay rate and an approximate
	// degrees-per-hop (this grid's own longitude/latitude cell size) matter for how many hops the
	// growth survives before its energy crosses the threshold. Actual realized size still varies
	// around this average because of Growth Jitter and the random frontier order - intentionally;
	// that variation is a large part of what makes the result look organic rather than templated.
	const float DegreesPerHop = 0.5f * (360.0f / GridW + 180.0f / FMath::Max(GridH - 1, 1));
	const float AverageDecay = FMath::Clamp((GrowthDecayMin + GrowthDecayMax) * 0.5f, 0.01f, 0.999f);
	auto StopThresholdForRadius = [&](float RadiusDegrees)
	{
		const float NumHops = FMath::Max(RadiusDegrees / DegreesPerHop, 1.0f);
		return FMath::Pow(AverageDecay, NumHops);
	};

	constexpr int32 MaxPlacementAttempts = 16;
	auto GrowRandomLandmass = [&](float MinRadiusDegrees, float MaxRadiusDegrees)
	{
		const float StopThreshold = StopThresholdForRadius(Stream.FRandRange(MinRadiusDegrees, MaxRadiusDegrees));
		for (int32 Attempt = 0; Attempt < MaxPlacementAttempts; ++Attempt)
		{
			const int32 StartIdx = DirectionToNearestCellIdx(Grid, RandomPointOnUnitSphere(Stream));
			if (GrowLandmass(StartIdx, StopThreshold))
			{
				return;
			}
		}
		UE_LOG(LogSolarOrbzContinent, Warning,
			TEXT("SolarOrbz Continent (Voronoi Growth): couldn't find an unclaimed start cell after %d attempts - skipping one landmass. The grid is likely nearly full (too many continents/islands for Bake Grid Width/Height, or the polar continents cover most of it)."),
			MaxPlacementAttempts);
	};

	// Claims every cell within Polar Continent Radius of PoleDirection outright - a direct
	// geometric claim, not a randomized growth, since a polar cap conventionally reads fine as
	// (approximately) round, unlike a random landmass. Still writes OwnerEnergyRemaining/
	// OwnerStartEnergy through the exact same fields GrowLandmass uses, so the shared conversion
	// pass below treats every landmass identically regardless of how it was claimed.
	auto ClaimPolarCap = [&](const FVector& PoleDirection)
	{
		const float PolarRadiusRadians = FMath::DegreesToRadians(PolarContinentRadiusDegrees);
		const int32 OwnerTag = OwnerStartEnergy.Add(1.0f);
		for (int32 Idx = 0; Idx < NumCells; ++Idx)
		{
			if (OwnerId[Idx] != -1)
			{
				continue;
			}
			const float Angle = FMath::Acos(FMath::Clamp(FVector::DotProduct(CellDirections[Idx], PoleDirection), -1.0f, 1.0f));
			if (Angle <= PolarRadiusRadians)
			{
				OwnerId[Idx] = OwnerTag;
				OwnerEnergyRemaining[Idx] = FMath::Clamp(1.0f - Angle / FMath::Max(PolarRadiusRadians, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
			}
		}
	};

	// Poles claim their cap FIRST so continents/islands below can never grow into it - overlap with
	// a polar continent is prevented by construction here, unlike Radial Seeds' rejection-sampling.
	if (bEffectiveNorthPolar)
	{
		ClaimPolarCap(FVector(0.0f, 0.0f, 1.0f));
	}
	if (bEffectiveSouthPolar)
	{
		ClaimPolarCap(FVector(0.0f, 0.0f, -1.0f));
	}

	for (int32 i = 0; i < EffectiveNumContinents; ++i)
	{
		GrowRandomLandmass(MinContinentRadiusDegrees, MaxContinentRadiusDegrees);
	}
	for (int32 i = 0; i < EffectiveNumIslands; ++i)
	{
		GrowRandomLandmass(MinIslandRadiusDegrees, MaxIslandRadiusDegrees);
	}

	BakedHeightCm.SetNumUninitialized(NumCells);
	for (int32 Idx = 0; Idx < NumCells; ++Idx)
	{
		float BaseMeters = OceanFloorDepthMeters;
		if (OwnerId[Idx] != -1)
		{
			// 1 at a growth's origin (or a pole's exact center), ramping toward 0 at the edge of
			// however far it actually grew - same Lerp/Sharpness shape Radial Seeds' own falloff uses.
			const float T = FMath::Clamp(OwnerEnergyRemaining[Idx] / FMath::Max(OwnerStartEnergy[OwnerId[Idx]], KINDA_SMALL_NUMBER), 0.0f, 1.0f);
			const float Shaped = FMath::Pow(T, FMath::Max(CoastlineSharpness, 0.01f));
			BaseMeters = FMath::Lerp(OceanFloorDepthMeters, LandPlateauHeightMeters, Shaped);
		}

		// Small cosmetic wobble on top of the grid-driven coastline, reusing the shared Coastline
		// properties - 0.1x their own span is deliberately modest; the growth shape is already doing
		// the real work of not looking circular, this is just surface texture.
		const float NoiseMeters = CoastlineNoiseAmplitude * (LandPlateauHeightMeters - OceanFloorDepthMeters) * 0.1f *
			SolarOrbzNoiseBasis::SampleBasis(ESolarOrbzNoiseType::Perlin, CellDirections[Idx] * CoastlineNoiseFrequency, Seed);

		BakedHeightCm[Idx] = (BaseMeters + NoiseMeters) * 100.0f;
	}
}

void USolarOrbzContinentTerrainLayer::BakePlateTectonics(bool bEffectiveNorthPolar, bool bEffectiveSouthPolar)
{
	using namespace SolarOrbzContinent;

	const int32 GridW = FMath::Max(BakeGridWidth, 8);
	const int32 GridH = FMath::Max(BakeGridHeight, 4);
	BakedGridWidth = GridW;
	BakedGridHeight = GridH;
	const int32 NumCells = GridW * GridH;
	const FSolarOrbzLatLongGrid Grid(GridW, GridH);

	TArray<FVector> CellDirections;
	CellDirections.SetNumUninitialized(NumCells);
	Grid.ForEachCell([&](int32 Idx, const FVector& Dir, const FVector2D& UV) { CellDirections[Idx] = Dir; });

	FRandomStream Stream(Seed);

	// Each plate: a random seed direction (for the nearest-seed/Worley partition below), a random
	// "drift" direction (magnitude is never used - only two plates' drift directions RELATIVE to
	// each other classify their shared boundary, below), and oceanic-vs-continental. This is the
	// approximate (NOT a physics simulation) model "Procedural Tectonic Planets" (Cortial, Peytavie,
	// Galin & Guerin, CGF 38:2, 2019) describes - see the class comment above.
	const int32 PlateCount = FMath::Max(NumPlates, 2);
	TArray<FVector> PlateSeedDirection;
	TArray<FVector> PlateDrift;
	TArray<bool> PlateOceanic;
	PlateSeedDirection.SetNumUninitialized(PlateCount);
	PlateDrift.SetNumUninitialized(PlateCount);
	PlateOceanic.SetNumUninitialized(PlateCount);
	for (int32 P = 0; P < PlateCount; ++P)
	{
		PlateSeedDirection[P] = RandomPointOnUnitSphere(Stream);
		PlateDrift[P] = RandomPointOnUnitSphere(Stream); // an independent random unit vector reused purely as a direction, not a position
		PlateOceanic[P] = Stream.FRand() < OceanicPlateFraction;
	}

	// Nearest-seed (Worley) plate assignment, one brute-force scan over PlateCount per cell -
	// trivially cheap even at NumPlates' max (64) against a few hundred thousand cells, since this
	// runs once per regenerate, not per vertex.
	TArray<int32> PlateId;
	PlateId.SetNumUninitialized(NumCells);
	for (int32 Idx = 0; Idx < NumCells; ++Idx)
	{
		int32 BestPlate = 0;
		float BestDot = -2.0f;
		for (int32 P = 0; P < PlateCount; ++P)
		{
			const float Dot = FVector::DotProduct(CellDirections[Idx], PlateSeedDirection[P]);
			if (Dot > BestDot)
			{
				BestDot = Dot;
				BestPlate = P;
			}
		}
		PlateId[Idx] = BestPlate;
	}

	// Pole override: force the polar cap's plate assignment to a sentinel "always continental,
	// always stationary" id regardless of the Worley partition above - the explicit "pinned
	// landmass at the pole" feature works the same way under every algorithm. IsOceanic/IsContinental
	// below both understand this sentinel.
	constexpr int32 PolarContinentalSentinel = -2;
	const float PolarRadiusRadians = FMath::DegreesToRadians(PolarContinentRadiusDegrees);
	auto ApplyPolarOverride = [&](const FVector& PoleDirection)
	{
		for (int32 Idx = 0; Idx < NumCells; ++Idx)
		{
			const float Angle = FMath::Acos(FMath::Clamp(FVector::DotProduct(CellDirections[Idx], PoleDirection), -1.0f, 1.0f));
			if (Angle <= PolarRadiusRadians)
			{
				PlateId[Idx] = PolarContinentalSentinel;
			}
		}
	};
	if (bEffectiveNorthPolar)
	{
		ApplyPolarOverride(FVector(0.0f, 0.0f, 1.0f));
	}
	if (bEffectiveSouthPolar)
	{
		ApplyPolarOverride(FVector(0.0f, 0.0f, -1.0f));
	}

	auto IsOceanic = [&PlateOceanic](int32 Plate) { return Plate >= 0 && PlateOceanic[Plate]; };
	auto IsContinental = [&PlateOceanic](int32 Plate) { return Plate == PolarContinentalSentinel || (Plate >= 0 && !PlateOceanic[Plate]); };

	// Multi-source BFS from every plate-boundary cell (any cell with a 4-connected neighbor on a
	// different plate), carrying forward that boundary's classified height modifier - this is what
	// turns "two touching Worley cells" into an actual mountain range/trench/ridge/rift that extends
	// some distance either side of the boundary (Boundary Influence Degrees), not a single
	// one-cell-wide seam.
	TArray<int32> DistanceHops;
	TArray<float> BoundaryModifierMeters;
	DistanceHops.Init(-1, NumCells);
	BoundaryModifierMeters.Init(0.0f, NumCells);

	const float DegreesPerHop = 0.5f * (360.0f / GridW + 180.0f / FMath::Max(GridH - 1, 1));
	const int32 CutoffHops = FMath::Max(FMath::RoundToInt(BoundaryInfluenceDegrees / DegreesPerHop), 1);

	TArray<int32> Frontier;
	for (int32 Idx = 0; Idx < NumCells; ++Idx)
	{
		int32 Neighbors[4];
		GetGridNeighbors4(Idx, GridW, GridH, Neighbors);

		const int32 PlateA = PlateId[Idx];
		float BestModifier = 0.0f;
		bool bIsBoundary = false;
		for (int32 NeighborIdx : Neighbors)
		{
			const int32 PlateB = PlateId[NeighborIdx];
			if (PlateB == PlateA)
			{
				continue;
			}
			bIsBoundary = true;

			// Classify using the two plates' drift RELATIVE to each other, projected onto the line
			// between their seeds - a cheap proxy for "are they closing (convergent), opening
			// (divergent), or sliding past (transform) each other," in the same approximate spirit
			// the class comment's paper reference uses (no actual physics simulation).
			const FVector SeedDirA = (PlateA >= 0) ? PlateSeedDirection[PlateA] : CellDirections[Idx];
			const FVector SeedDirB = (PlateB >= 0) ? PlateSeedDirection[PlateB] : CellDirections[NeighborIdx];
			const FVector BoundaryAxis = (SeedDirB - SeedDirA).GetSafeNormal();
			const FVector DriftA = (PlateA >= 0) ? PlateDrift[PlateA] : FVector::ZeroVector; // the polar sentinel is treated as stationary
			const FVector DriftB = (PlateB >= 0) ? PlateDrift[PlateB] : FVector::ZeroVector;
			const float Closure = FVector::DotProduct(DriftA, BoundaryAxis) - FVector::DotProduct(DriftB, BoundaryAxis);

			constexpr float TransformThreshold = 0.25f;
			float Modifier = 0.0f;
			if (Closure > TransformThreshold)
			{
				// Converging.
				if (IsContinental(PlateA) && IsContinental(PlateB))
				{
					Modifier = MountainHeightMeters; // continent-continent collision, the Himalaya case
				}
				else if (IsOceanic(PlateA) && IsOceanic(PlateB))
				{
					Modifier = MountainHeightMeters * 0.3f; // smaller island-arc bump
				}
				else
				{
					// One oceanic, one continental - subduction: a trench on the oceanic side, a
					// smaller coastal-mountain bump on the continental side.
					Modifier = IsOceanic(PlateA) ? TrenchDepthMeters : MountainHeightMeters * 0.5f;
				}
			}
			else if (Closure < -TransformThreshold)
			{
				// Diverging.
				Modifier = (IsOceanic(PlateA) && IsOceanic(PlateB)) ? RidgeHeightMeters : RiftDepthMeters;
			}
			// else: transform - no height modifier, just the plain plate-boundary seam.

			if (FMath::Abs(Modifier) > FMath::Abs(BestModifier))
			{
				BestModifier = Modifier;
			}
		}

		if (bIsBoundary)
		{
			DistanceHops[Idx] = 0;
			BoundaryModifierMeters[Idx] = BestModifier;
			Frontier.Add(Idx);
		}
	}

	for (int32 Hop = 0; Hop < CutoffHops && Frontier.Num() > 0; ++Hop)
	{
		TArray<int32> NextFrontier;
		for (int32 Idx : Frontier)
		{
			int32 Neighbors[4];
			GetGridNeighbors4(Idx, GridW, GridH, Neighbors);
			for (int32 NeighborIdx : Neighbors)
			{
				if (DistanceHops[NeighborIdx] != -1)
				{
					continue;
				}
				DistanceHops[NeighborIdx] = Hop + 1;
				BoundaryModifierMeters[NeighborIdx] = BoundaryModifierMeters[Idx];
				NextFrontier.Add(NeighborIdx);
			}
		}
		Frontier = MoveTemp(NextFrontier);
	}

	BakedHeightCm.SetNumUninitialized(NumCells);
	for (int32 Idx = 0; Idx < NumCells; ++Idx)
	{
		const float BaseMeters = IsOceanic(PlateId[Idx]) ? OceanFloorDepthMeters : LandPlateauHeightMeters;

		float ModifierMeters = 0.0f;
		if (DistanceHops[Idx] != -1)
		{
			const float Falloff = 1.0f - FMath::SmoothStep(0.0f, (float)CutoffHops, (float)DistanceHops[Idx]);
			ModifierMeters = BoundaryModifierMeters[Idx] * Falloff;
		}

		// Small cosmetic wobble, same convention BakeVoronoiGrowth's grid uses.
		const float NoiseMeters = CoastlineNoiseAmplitude * (LandPlateauHeightMeters - OceanFloorDepthMeters) * 0.1f *
			SolarOrbzNoiseBasis::SampleBasis(ESolarOrbzNoiseType::Perlin, CellDirections[Idx] * CoastlineNoiseFrequency, Seed);

		BakedHeightCm[Idx] = (BaseMeters + ModifierMeters + NoiseMeters) * 100.0f;
	}
}

float USolarOrbzContinentTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	switch (Algorithm)
	{
	case ESolarOrbzContinentAlgorithm::VoronoiGrowth:
	case ESolarOrbzContinentAlgorithm::PlateTectonics:
		return GetRawHeightFromBakedGrid(UnitDirection);
	case ESolarOrbzContinentAlgorithm::RadialSeeds:
	default:
		return GetRawHeightRadialSeeds(UnitDirection);
	}
}

float USolarOrbzContinentTerrainLayer::GetRawHeightRadialSeeds(const FVector& UnitDirection) const
{
	using namespace SolarOrbzContinent;

	if (CachedSeeds.Num() == 0)
	{
		return OceanFloorDepthMeters * 100.0f + CachedSeaLevelCm; // no landmasses configured - the whole planet is ocean floor
	}

	// Domain-warp the SAMPLE POINT before any seed's circle test runs, rather than only perturbing
	// the output radius below - warping the point is what can actually bend the boundary into
	// peninsulas/bays (a radius perturbation alone is still one smooth distance field around one
	// center, however noisy). Same warp-vector construction USolarOrbzFractalNoiseTerrainLayerBase
	// already uses for its own WarpStrength/WarpFrequency, applied here to geometry instead of to a
	// noise function's internal sample position - see iquilezles.org/articles/warp.
	FVector WarpedDirection = UnitDirection;
	if (CoastlineWarpStrength > 0.0f)
	{
		const FVector WarpPos = UnitDirection * CoastlineWarpFrequency;
		const FVector Warp(
			FMath::PerlinNoise3D(WarpPos),
			FMath::PerlinNoise3D(WarpPos + FVector(31.7f, 0.0f, 0.0f)),
			FMath::PerlinNoise3D(WarpPos + FVector(0.0f, 57.3f, 0.0f)));
		WarpedDirection = (UnitDirection + Warp * CoastlineWarpStrength).GetSafeNormal();
	}

	float MaxInfluence = 0.0f;
	for (const FSolarOrbzContinentSeedData& S : CachedSeeds)
	{
		const float CosAngle = FVector::DotProduct(WarpedDirection, S.Direction);
		const float Angle = FMath::Acos(FMath::Clamp(CosAngle, -1.0f, 1.0f));

		const float CoastlineNoise = FractalNoise3D(WarpedDirection * CoastlineNoiseFrequency + S.NoiseOffset, CoastlineNoiseOctaves, CoastlineNoisePersistence, CoastlineNoiseLacunarity, Seed);
		const float PerturbedRadius = FMath::Max(S.RadiusRadians * (1.0f + CoastlineNoiseAmplitude * CoastlineNoise), 0.0f);

		// 1 at the seed's center, ramping smoothly down to 0 at (and beyond) its perturbed radius.
		const float Influence = 1.0f - FMath::SmoothStep(0.0f, FMath::Max(PerturbedRadius, KINDA_SMALL_NUMBER), Angle);
		MaxInfluence = FMath::Max(MaxInfluence, Influence); // overlapping seeds (sub-seeds included) merge into one landmass rather than fighting
	}

	MaxInfluence = FMath::Pow(FMath::Clamp(MaxInfluence, 0.0f, 1.0f), FMath::Max(CoastlineSharpness, 0.01f));

	const float HeightMetersAboveSeaLevel = FMath::Lerp(OceanFloorDepthMeters, LandPlateauHeightMeters, MaxInfluence);
	// Genuinely offset by Sea Level (cached via ApplyPlanetaryContext), not just the raw base
	// radius - so Ocean Floor Depth/Land Plateau Height mean "relative to wherever Sea Level is",
	// exactly matching the doc comments on those two properties.
	return HeightMetersAboveSeaLevel * 100.0f + CachedSeaLevelCm; // meters -> UE units (cm), then shift by Sea Level
}

float USolarOrbzContinentTerrainLayer::GetRawHeightFromBakedGrid(const FVector& UnitDirection) const
{
	if (BakedHeightCm.Num() == 0 || BakedGridWidth <= 0 || BakedGridHeight <= 0)
	{
		return OceanFloorDepthMeters * 100.0f + CachedSeaLevelCm; // Bake() hasn't run (or produced an empty grid) yet - fall back to plain ocean floor rather than reading past the array
	}

	// BakedHeightCm already stores plain meters-relative-to-sea-level (converted to cm) with NO Sea
	// Level offset baked in, exactly like CachedSeeds' RadialSeeds path - Sea Level is added once,
	// here, at sample time, so moving Sea Level later never requires re-baking the grid.
	return FSolarOrbzLatLongGrid(BakedGridWidth, BakedGridHeight).SampleBilinear(BakedHeightCm, UnitDirection) + CachedSeaLevelCm;
}
