// SolarOrbz - Terrain Layers subsystem implementation.

#include "SolarOrbzTerrainLayers.h"
#include "Engine/Texture2D.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzErosion, Log, All);

// ================================================================================================
// USolarOrbzTerrainLayer - GetRawHeight has an inline default; nothing else to implement here.
// ================================================================================================

// ================================================================================================
// USolarOrbzTerrainLayerStack
// ================================================================================================
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
float USolarOrbzFractalNoiseTerrainLayerBase::ComputeNormalizedNoise(const FVector& UnitDirection) const
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
		Sum += FMath::PerlinNoise3D(OctavePos) * OctaveAmplitude;
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
	return ComputeNormalizedNoise(UnitDirection) * AmplitudeMeters * 100.0f; // meters -> UE units (cm)
}

// ================================================================================================
// USolarOrbzPlanetaryNoiseTerrainLayer
// ================================================================================================
float USolarOrbzPlanetaryNoiseTerrainLayer::GetRawHeight(const FVector& UnitDirection, const FVector2D& UV) const
{
	const float Normalized = ComputeNormalizedNoise(UnitDirection); // -1..1, relative to sea level (0 = base radius)

	// Above sea level scales toward MaxElevationMeters; below scales toward MaxDepthMeters (entered
	// as a positive depth, so this stays negative here since Normalized is negative below sea level).
	const float HeightMeters = Normalized >= 0.0f
		? Normalized * MaxElevationMeters
		: Normalized * MaxDepthMeters;

	return HeightMeters * 100.0f; // meters -> UE units (cm)
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
