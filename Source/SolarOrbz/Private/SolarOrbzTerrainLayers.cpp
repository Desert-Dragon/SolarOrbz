// SolarOrbz - Terrain Layers subsystem implementation.

#include "SolarOrbzTerrainLayers.h"
#include "Engine/Texture2D.h"
#include "Math/RandomStream.h"
#include "SolarOrbzProfiles.h"

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
void USolarOrbzTerrainLayerStack::ApplyPlanetaryContext(const USolarOrbzPlanetProfile* Profile, float SeaLevelCm) const
{
	for (const TObjectPtr<USolarOrbzTerrainLayer>& Layer : Layers)
	{
		if (Layer)
		{
			Layer->ApplyPlanetaryContext(Profile, SeaLevelCm);
		}
	}
}

void USolarOrbzTerrainLayerStack::PrepareLayers(float RadiusCm) const
{
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		USolarOrbzTerrainLayer* Layer = Layers[i];
		if (!Layer || !Layer->bEnabled || !Layer->RequiresWholeSurfaceBake())
		{
			continue;
		}

		// Capture by value (this, i) - safe to call from Bake() as many times as it likes, since it
		// only ever reaches back into layers strictly below index i, never itself or anything above it.
		auto PriorLayersHeight = [this, i](const FVector& UnitDirection, const FVector2D& UV) -> float
		{
			return EvaluateHeightUpTo(i, UnitDirection, UV);
		};

		Layer->Bake(PriorLayersHeight, RadiusCm);
	}
}

float USolarOrbzTerrainLayerStack::EvaluateHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	return EvaluateHeightUpTo(Layers.Num(), UnitDirection, UV);
}

float USolarOrbzTerrainLayerStack::EvaluateHeightUpTo(int32 EndIndexExclusive, const FVector& UnitDirection, const FVector2D& UV) const
{
	float Accum = 0.0f;

	const int32 Count = FMath::Min(EndIndexExclusive, Layers.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		const TObjectPtr<USolarOrbzTerrainLayer>& Layer = Layers[i];
		if (!Layer || !Layer->bEnabled)
		{
			continue;
		}

		const float LayerHeight = Layer->GetRawHeight(UnitDirection, UV) * Layer->Weight;

		switch (Layer->BlendMode)
		{
		case ESolarOrbzTerrainBlendMode::Add:      Accum += LayerHeight; break;
		case ESolarOrbzTerrainBlendMode::Subtract: Accum -= LayerHeight; break;
		case ESolarOrbzTerrainBlendMode::Multiply: Accum *= LayerHeight; break;
		case ESolarOrbzTerrainBlendMode::Max:      Accum = FMath::Max(Accum, LayerHeight); break;
		case ESolarOrbzTerrainBlendMode::Min:      Accum = FMath::Min(Accum, LayerHeight); break;
		case ESolarOrbzTerrainBlendMode::Replace:  Accum = LayerHeight; break;
		}
	}

	return Accum;
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

void USolarOrbzFractalNoiseTerrainLayerBase::Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, float RadiusCm)
{
	// PriorLayersHeight is deliberately unused - this Bake() exists purely as a "once per
	// regenerate" hook to calibrate amplitude compensation via Monte Carlo sampling of this
	// layer's OWN noise, not for whole-surface height data the way Erosion/Terrace use it.
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
	// Cheap deterministic hash so different seeds don't just look like the same
	// field shifted by a fixed, obvious amount.
	const FVector SeedOffset(
		FMath::Frac(FMath::Sin((float)Seed * 12.9898f) * 43758.5453f) * 1000.0f,
		FMath::Frac(FMath::Sin((float)Seed * 78.233f) * 43758.5453f) * 1000.0f,
		FMath::Frac(FMath::Sin((float)Seed * 37.719f) * 43758.5453f) * 1000.0f);

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
	// Same rationale as SolarOrbzIcoSphere.cpp / SolarOrbzClimateSimulation.cpp: an explicit
	// double constant rather than the engine's PI macro, since FVector components are double (LWC).
	static constexpr double PI_D = 3.14159265358979323846;

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

void USolarOrbzErosionTerrainLayer::Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, float RadiusCm)
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
	for (int32 Y = 0; Y < H; ++Y)
	{
		const double V = (double)Y / (double)FMath::Max(H - 1, 1); // 0 north pole .. 1 south pole
		const double Polar = V * PI_D;
		const double Z = FMath::Cos(Polar);
		const double SinPolar = FMath::Sin(Polar);

		for (int32 X = 0; X < W; ++X)
		{
			const double U = (double)X / (double)W; // wraps - no -1, W steps tile exactly around
			const double Azimuth = (U - 0.5) * 2.0 * PI_D;

			const FVector UnitDirection(
				(float)(SinPolar * FMath::Cos(Azimuth)),
				(float)(SinPolar * FMath::Sin(Azimuth)),
				(float)Z);
			const FVector2D UV((float)U, (float)V);

			const int32 Idx = Y * W + X;
			const float H0 = PriorLayersHeight(UnitDirection, UV);
			Height[Idx] = H0;
			OriginalHeight[Idx] = H0;
		}
	}

	// Approximate physical spacing between adjacent grid cells at the equator - a uniform stand-in
	// for talus/slope thresholds everywhere on the grid (see the pole-accuracy note at the top of the header).
	const float CellSpacingCm = (float)((2.0 * PI_D * (double)FMath::Max(RadiusCm, 1.0f)) / (double)W);

	// --- Thermal erosion: material above the talus angle slides toward its lowest neighbor. ---
	if (bEnableThermalErosion && ThermalIterations > 0)
	{
		const float TalusHeightPerCell = FMath::Tan(FMath::DegreesToRadians(TalusAngleDegrees)) * CellSpacingCm;

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

				const float Slope = FMath::Max((SelfHeight - Height[LowestIdx]) / CellSpacingCm, 0.0f);
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
	using namespace SolarOrbzErosion;

	if (BakedWidth <= 0 || BakedHeight <= 0 || BakedDeltaHeightCm.Num() != BakedWidth * BakedHeight)
	{
		return 0.0f; // Not baked yet (e.g. layer just added and RegenerateMesh hasn't run) - contribute nothing rather than garbage.
	}

	// Same convention as FSolarOrbzIcoSphereGenerator::ComputeUV / FSolarOrbzClimateGrid::Sample -
	// derived directly from UnitDirection rather than trusting the caller's UV, so this is robust to
	// any seam-fixing quirks the mesh's own UVs might have near the poles/seam.
	const double Azimuth = FMath::Atan2((double)UnitDirection.Y, (double)UnitDirection.X);
	const double U = 0.5 + Azimuth / (2.0 * PI_D);
	const double Polar = FMath::Acos(FMath::Clamp((double)UnitDirection.Z, -1.0, 1.0));
	const double V = Polar / PI_D;

	const double Fx = FMath::Frac(U) * (double)BakedWidth;
	const double Fy = FMath::Clamp(V, 0.0, 1.0) * (double)(BakedHeight - 1);

	const int32 X0 = FMath::FloorToInt(Fx) % BakedWidth;
	const int32 X1 = (X0 + 1) % BakedWidth;
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt(Fy), 0, BakedHeight - 1);
	const int32 Y1 = FMath::Clamp(Y0 + 1, 0, BakedHeight - 1);

	const float Tx = (float)(Fx - FMath::FloorToDouble(Fx));
	const float Ty = (float)(Fy - FMath::FloorToDouble(Fy));

	const float A = FMath::Lerp(BakedDeltaHeightCm[Y0 * BakedWidth + X0], BakedDeltaHeightCm[Y0 * BakedWidth + X1], Tx);
	const float B = FMath::Lerp(BakedDeltaHeightCm[Y1 * BakedWidth + X0], BakedDeltaHeightCm[Y1 * BakedWidth + X1], Tx);
	return FMath::Lerp(A, B, Ty);
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

void USolarOrbzTerraceTerrainLayer::Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, float RadiusCm)
{
	using namespace SolarOrbzTerrace;

	const int32 W = FMath::Max(GridWidth, 8);
	const int32 H = FMath::Max(GridHeight, 4);

	BakedWidth = W;
	BakedHeight = H;
	BakedDeltaHeightCm.SetNumUninitialized(W * H);

	const float StepHeightCm = StepHeightMeters * 100.0f; // meters -> UE units (cm)
	const FVector IrregularitySeedOffset(
		FMath::Frac(FMath::Sin((float)IrregularitySeed * 12.9898f) * 43758.5453f) * 1000.0f,
		FMath::Frac(FMath::Sin((float)IrregularitySeed * 78.233f) * 43758.5453f) * 1000.0f,
		FMath::Frac(FMath::Sin((float)IrregularitySeed * 37.719f) * 43758.5453f) * 1000.0f);

	float MinDelta = TNumericLimits<float>::Max(), MaxDelta = TNumericLimits<float>::Lowest();

	for (int32 Y = 0; Y < H; ++Y)
	{
		const double V = (double)Y / (double)FMath::Max(H - 1, 1); // 0 north pole .. 1 south pole
		const double Polar = V * SolarOrbzErosion::PI_D;
		const double Z = FMath::Cos(Polar);
		const double SinPolar = FMath::Sin(Polar);

		for (int32 X = 0; X < W; ++X)
		{
			const double U = (double)X / (double)W;
			const double Azimuth = (U - 0.5) * 2.0 * SolarOrbzErosion::PI_D;

			const FVector UnitDirection(
				(float)(SinPolar * FMath::Cos(Azimuth)),
				(float)(SinPolar * FMath::Sin(Azimuth)),
				(float)Z);
			const FVector2D UV((float)U, (float)V);

			const int32 Idx = Y * W + X;
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
		}
	}

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

	// Same convention as FSolarOrbzIcoSphereGenerator::ComputeUV / FSolarOrbzClimateGrid::Sample.
	const double Azimuth = FMath::Atan2((double)UnitDirection.Y, (double)UnitDirection.X);
	const double U = 0.5 + Azimuth / (2.0 * SolarOrbzErosion::PI_D);
	const double Polar = FMath::Acos(FMath::Clamp((double)UnitDirection.Z, -1.0, 1.0));
	const double V = Polar / SolarOrbzErosion::PI_D;

	const double Fx = FMath::Frac(U) * (double)BakedWidth;
	const double Fy = FMath::Clamp(V, 0.0, 1.0) * (double)(BakedHeight - 1);

	const int32 X0 = FMath::FloorToInt(Fx) % BakedWidth;
	const int32 X1 = (X0 + 1) % BakedWidth;
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt(Fy), 0, BakedHeight - 1);
	const int32 Y1 = FMath::Clamp(Y0 + 1, 0, BakedHeight - 1);

	const float Tx = (float)(Fx - FMath::FloorToDouble(Fx));
	const float Ty = (float)(Fy - FMath::FloorToDouble(Fy));

	const float A = FMath::Lerp(BakedDeltaHeightCm[Y0 * BakedWidth + X0], BakedDeltaHeightCm[Y0 * BakedWidth + X1], Tx);
	const float B = FMath::Lerp(BakedDeltaHeightCm[Y1 * BakedWidth + X0], BakedDeltaHeightCm[Y1 * BakedWidth + X1], Tx);
	return FMath::Lerp(A, B, Ty);
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

void USolarOrbzContinentTerrainLayer::Bake(const TFunctionRef<float(const FVector& UnitDirection, const FVector2D& UV)>& PriorLayersHeight, float RadiusCm)
{
	// PriorLayersHeight is deliberately unused - seed positions/radii don't depend on anything
	// below this layer in the stack. Bake() is only used here as a "once per regenerate" hook to
	// regenerate the seed list deterministically, not for whole-surface height sampling.
	using namespace SolarOrbzContinent;

	const USolarOrbzPlanetProfile* EffectiveProfile = ProfileOverride.Get();

	// Resolve once, here, rather than reading the authored properties directly below - this is the
	// one and only place Profile overriding actually happens; everything past this point behaves
	// identically whether these came from a Profile or from this layer's own fields.
	const int32 EffectiveNumContinents = EffectiveProfile ? EffectiveProfile->GetNumContinents() : NumContinents;
	const int32 EffectiveNumIslands = EffectiveProfile ? EffectiveProfile->GetNumIslands() : NumIslands;
	const bool bEffectiveNorthPolar = EffectiveProfile ? EffectiveProfile->HasNorthPolarContinent() : bHasNorthPolarContinent;
	const bool bEffectiveSouthPolar = EffectiveProfile ? EffectiveProfile->HasSouthPolarContinent() : bHasSouthPolarContinent;

	CachedSeeds.Reset();
	FRandomStream Stream(Seed);

	int32 SeedIndex = 0;
	auto AddSeed = [&](const FVector& Direction, float MinRadiusDegrees, float MaxRadiusDegrees)
	{
		FSolarOrbzContinentSeedData S;
		S.Direction = Direction;
		S.RadiusRadians = FMath::DegreesToRadians(Stream.FRandRange(MinRadiusDegrees, MaxRadiusDegrees));
		// Small deterministic per-seed offset so each landmass's coastline noise looks independent
		// rather than every coastline sharing the exact same wiggle pattern.
		S.NoiseOffset = FVector(SeedIndex * 17.3f, SeedIndex * 29.7f, SeedIndex * 53.1f);
		CachedSeeds.Add(S);
		++SeedIndex;
	};

	for (int32 i = 0; i < EffectiveNumContinents; ++i)
	{
		AddSeed(RandomPointOnUnitSphere(Stream), MinContinentRadiusDegrees, MaxContinentRadiusDegrees);
	}
	for (int32 i = 0; i < EffectiveNumIslands; ++i)
	{
		AddSeed(RandomPointOnUnitSphere(Stream), MinIslandRadiusDegrees, MaxIslandRadiusDegrees);
	}
	if (bEffectiveNorthPolar)
	{
		AddSeed(FVector(0.0f, 0.0f, 1.0f), PolarContinentRadiusDegrees, PolarContinentRadiusDegrees);
	}
	if (bEffectiveSouthPolar)
	{
		AddSeed(FVector(0.0f, 0.0f, -1.0f), PolarContinentRadiusDegrees, PolarContinentRadiusDegrees);
	}

	// Cheap diagnostic: estimate land coverage by sampling a small independent grid - not the same
	// grid resolution concept Erosion/Terrace use (this layer has no bake grid of its own), just a
	// coarse average purely for this log line.
	constexpr int32 CoverageSamplesW = 64, CoverageSamplesH = 32;
	int32 LandSamples = 0;
	for (int32 Y = 0; Y < CoverageSamplesH; ++Y)
	{
		const double V = (double)Y / (double)FMath::Max(CoverageSamplesH - 1, 1);
		const double Polar = V * SolarOrbzErosion::PI_D;
		const double Z = FMath::Cos(Polar);
		const double SinPolar = FMath::Sin(Polar);
		for (int32 X = 0; X < CoverageSamplesW; ++X)
		{
			const double U = (double)X / (double)CoverageSamplesW;
			const double Azimuth = (U - 0.5) * 2.0 * SolarOrbzErosion::PI_D;
			const FVector Dir((float)(SinPolar * FMath::Cos(Azimuth)), (float)(SinPolar * FMath::Sin(Azimuth)), (float)Z);
			if (GetRawHeight(Dir, FVector2D::ZeroVector) > 0.0f)
			{
				++LandSamples;
			}
		}
	}
	const float LandCoveragePercent = 100.0f * LandSamples / (float)(CoverageSamplesW * CoverageSamplesH);

	UE_LOG(LogSolarOrbzContinent, Log,
		TEXT("SolarOrbz Continent: placed %d continent(s), %d island(s), %s%s%s - approx %.1f%% land coverage (values from %s)"),
		EffectiveNumContinents, EffectiveNumIslands,
		bEffectiveNorthPolar ? TEXT("north polar continent") : TEXT("no north polar continent"),
		(bEffectiveNorthPolar && bEffectiveSouthPolar) ? TEXT(", ") : TEXT(""),
		bEffectiveSouthPolar ? TEXT("south polar continent") : TEXT(""),
		LandCoveragePercent,
		EffectiveProfile ? TEXT("Planet Profile") : TEXT("this layer's own authored properties"));

	if (CachedSeeds.Num() == 0)
	{
		UE_LOG(LogSolarOrbzContinent, Warning, TEXT("SolarOrbz Continent: no continents, islands, or polar continents configured - the whole planet will be Ocean Floor Depth."));
	}
}

float USolarOrbzContinentTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	if (CachedSeeds.Num() == 0)
	{
		return OceanFloorDepthMeters * 100.0f + CachedSeaLevelCm; // no landmasses configured - the whole planet is ocean floor
	}

	float MaxInfluence = 0.0f;
	for (const FSolarOrbzContinentSeedData& S : CachedSeeds)
	{
		const float CosAngle = FVector::DotProduct(UnitDirection, S.Direction);
		const float Angle = FMath::Acos(FMath::Clamp(CosAngle, -1.0f, 1.0f));

		const float CoastlineNoise = FMath::PerlinNoise3D(UnitDirection * CoastlineNoiseFrequency + S.NoiseOffset);
		const float PerturbedRadius = FMath::Max(S.RadiusRadians * (1.0f + CoastlineNoiseAmplitude * CoastlineNoise), 0.0f);

		// 1 at the seed's center, ramping smoothly down to 0 at (and beyond) its perturbed radius.
		const float Influence = 1.0f - FMath::SmoothStep(0.0f, FMath::Max(PerturbedRadius, KINDA_SMALL_NUMBER), Angle);
		MaxInfluence = FMath::Max(MaxInfluence, Influence); // overlapping seeds merge into one landmass rather than fighting
	}

	MaxInfluence = FMath::Pow(FMath::Clamp(MaxInfluence, 0.0f, 1.0f), FMath::Max(CoastlineSharpness, 0.01f));

	const float HeightMetersAboveSeaLevel = FMath::Lerp(OceanFloorDepthMeters, LandPlateauHeightMeters, MaxInfluence);
	// Genuinely offset by Sea Level (cached via ApplyPlanetaryContext), not just the raw base
	// radius - so Ocean Floor Depth/Land Plateau Height mean "relative to wherever Sea Level is",
	// exactly matching the doc comments on those two properties.
	return HeightMetersAboveSeaLevel * 100.0f + CachedSeaLevelCm; // meters -> UE units (cm), then shift by Sea Level
}
