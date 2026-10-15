// SolarOrbz - Biome System subsystem implementation.

#include "SolarOrbzBiomeSystem.h"
#include "SolarOrbzTerrainLayers.h"
#include "Materials/MaterialInterface.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzBiome, Log, All);

// ================================================================================================
// USolarOrbzBiomeMask - GetWeight has an inline default; nothing else to implement here.
// ================================================================================================

// ================================================================================================
// FSolarOrbzMaskRange / USolarOrbzClimateBiomeMask
// ================================================================================================
float FSolarOrbzMaskRange::Evaluate(float Value) const
{
	if (!bEnabled)
	{
		return 1.0f;
	}

	// Authors can leave Min > Max (e.g. while dragging both fields around) - swap rather than
	// silently returning 0 everywhere, so a momentarily-inverted range still behaves sanely.
	const float EffectiveMin = FMath::Min(Min, Max);
	const float EffectiveMax = FMath::Max(Min, Max);

	if (Falloff <= KINDA_SMALL_NUMBER)
	{
		return (Value >= EffectiveMin && Value <= EffectiveMax) ? 1.0f : 0.0f;
	}

	const float LowEdge = FMath::SmoothStep(EffectiveMin - Falloff, EffectiveMin + Falloff, Value);
	const float HighEdge = 1.0f - FMath::SmoothStep(EffectiveMax - Falloff, EffectiveMax + Falloff, Value);
	return FMath::Clamp(FMath::Min(LowEdge, HighEdge), 0.0f, 1.0f);
}

float USolarOrbzClimateBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	float Weight = 1.0f;

	Weight *= Elevation.Evaluate((Context.Elevation / 100.0f) - Context.SeaLevel); // cm -> meters, then offset so the range tracks Sea Level rather than the raw base radius
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	Weight *= Latitude.Evaluate(FMath::Abs(Context.UnitDirection.Z));
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	Weight *= Slope.Evaluate(Context.Slope);
	if (Weight <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	if (Temperature.bEnabled)
	{
		// Simulated (Kelvin) when available; otherwise a generic Earth-like Lerp by latitude, in the
		// same Kelvin units, so the Min/Max above stay meaningful whether or not a simulation exists.
		const float TemperatureValue = Context.bHasClimateData
			? Context.Temperature
			: FMath::Lerp(288.0f, 255.0f, FMath::Abs(Context.UnitDirection.Z));
		Weight *= Temperature.Evaluate(TemperatureValue);
		if (Weight <= KINDA_SMALL_NUMBER)
		{
			return 0.0f;
		}
	}

	if (Moisture.bEnabled)
	{
		float MoistureValue;
		if (Context.bHasClimateData)
		{
			MoistureValue = Context.Moisture;
		}
		else
		{
			const FVector SeedOffset(
				FMath::Frac(FMath::Sin((float)MoistureSeed * 91.345f) * 47453.7f) * 1000.0f,
				FMath::Frac(FMath::Sin((float)MoistureSeed * 13.71f) * 47453.7f) * 1000.0f,
				FMath::Frac(FMath::Sin((float)MoistureSeed * 58.92f) * 47453.7f) * 1000.0f);

			MoistureValue = FMath::PerlinNoise3D(Context.UnitDirection * MoistureFrequency + SeedOffset) * 0.5f + 0.5f; // 0..1
		}
		Weight *= Moisture.Evaluate(MoistureValue);
		if (Weight <= KINDA_SMALL_NUMBER)
		{
			return 0.0f;
		}
	}

	if (SunExposure.bEnabled)
	{
		float ExposureValue = 0.5f; // flat ground (or no gradient computed at all) - neutral, no facing direction
		if (!Context.LocalGradient.IsNearlyZero())
		{
			const float SunAzimuthRadians = FMath::DegreesToRadians(SunAzimuthDegrees);
			const FVector2D SunDirection(FMath::Cos(SunAzimuthRadians), FMath::Sin(SunAzimuthRadians));
			// The gradient points uphill; a slope "faces" the direction you'd walk downhill from its
			// peak, so negate it before comparing against the sun's bearing.
			const float CosAngle = FVector2D::DotProduct(-Context.LocalGradient.GetSafeNormal(), SunDirection);
			ExposureValue = CosAngle * 0.5f + 0.5f; // -1..1 -> 0..1
		}
		Weight *= SunExposure.Evaluate(ExposureValue);
	}

	return FMath::Clamp(Weight, 0.0f, 1.0f);
}

// ================================================================================================
// USolarOrbzCompositeBiomeMask
// ================================================================================================
float USolarOrbzCompositeBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	if (Biomes.Num() == 0)
	{
		return 1.0f; // no children = no restriction, matching an "empty AND is true" convention
	}

	switch (CombineMode)
	{
	case ESolarOrbzMaskCombineMode::Multiply:
	{
		float Result = 1.0f;
		for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
		{
			if (Biome)
			{
				Result *= Biome->GetWeight(Context);
				if (Result <= KINDA_SMALL_NUMBER)
				{
					return 0.0f;
				}
			}
		}
		return Result;
	}
	case ESolarOrbzMaskCombineMode::Min:
	{
		float Result = 1.0f;
		for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
		{
			if (Biome)
			{
				Result = FMath::Min(Result, Biome->GetWeight(Context));
			}
		}
		return Result;
	}
	case ESolarOrbzMaskCombineMode::Max:
	{
		float Result = 0.0f;
		for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
		{
			if (Biome)
			{
				Result = FMath::Max(Result, Biome->GetWeight(Context));
			}
		}
		return Result;
	}
	case ESolarOrbzMaskCombineMode::Average:
	{
		float Sum = 0.0f;
		int32 Count = 0;
		for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
		{
			if (Biome)
			{
				Sum += Biome->GetWeight(Context);
				++Count;
			}
		}
		return Count > 0 ? (Sum / Count) : 1.0f;
	}
	}

	return 1.0f;
}

bool USolarOrbzCompositeBiomeMask::NeedsClimateData() const
{
	// Same reference-cycle guard as USolarOrbzBiome::GetWeight, scoped to this traversal - a cycle
	// here already gets a loud warning from GetWeight itself once this mask is actually evaluated.
	static thread_local int32 RecursionDepth = 0;
	constexpr int32 MaxRecursionDepth = 16;
	if (RecursionDepth >= MaxRecursionDepth)
	{
		return false;
	}

	++RecursionDepth;
	bool bNeeds = false;
	for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
	{
		if (Biome && Biome->Mask && Biome->Mask->NeedsClimateData())
		{
			bNeeds = true;
			break;
		}
	}
	--RecursionDepth;
	return bNeeds;
}

bool USolarOrbzCompositeBiomeMask::NeedsSlope() const
{
	static thread_local int32 RecursionDepth = 0;
	constexpr int32 MaxRecursionDepth = 16;
	if (RecursionDepth >= MaxRecursionDepth)
	{
		return false;
	}

	++RecursionDepth;
	bool bNeeds = false;
	for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
	{
		if (Biome && Biome->Mask && Biome->Mask->NeedsSlope())
		{
			bNeeds = true;
			break;
		}
	}
	--RecursionDepth;
	return bNeeds;
}

bool USolarOrbzCompositeBiomeMask::NeedsLocalGradient() const
{
	static thread_local int32 RecursionDepth = 0;
	constexpr int32 MaxRecursionDepth = 16;
	if (RecursionDepth >= MaxRecursionDepth)
	{
		return false;
	}

	++RecursionDepth;
	bool bNeeds = false;
	for (const TObjectPtr<USolarOrbzBiome>& Biome : Biomes)
	{
		if (Biome && Biome->Mask && Biome->Mask->NeedsLocalGradient())
		{
			bNeeds = true;
			break;
		}
	}
	--RecursionDepth;
	return bNeeds;
}

// ================================================================================================
// USolarOrbzNoiseBiomeMask
// ================================================================================================
float USolarOrbzNoiseBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	const FVector SeedOffset = SolarOrbzNoiseBasis::ComputeSeedOffset(Seed);
	FVector SamplePos = Context.UnitDirection * Frequency + SeedOffset;

	// Fixed Persistence (0.5) / Lacunarity (2.0) - see this class's own header comment for why a
	// mask weight doesn't need those as separate authored fields the way a height-shaping Noise
	// Terrain Layer does.
	float Sum = 0.0f;
	float MaxPossible = 0.0f;
	float OctaveAmplitude = 1.0f;
	for (int32 Octave = 0; Octave < FMath::Max(Octaves, 1); ++Octave)
	{
		Sum += SolarOrbzNoiseBasis::SampleBasis(NoiseType, SamplePos, Seed) * OctaveAmplitude;
		MaxPossible += OctaveAmplitude;
		OctaveAmplitude *= 0.5f;
		SamplePos *= 2.0f;
	}
	const float RawNoise = MaxPossible > KINDA_SMALL_NUMBER ? FMath::Clamp(Sum / MaxPossible, -1.0f, 1.0f) : 0.0f;
	const float Normalized = RawNoise * 0.5f + 0.5f; // -1..1 -> 0..1, matching GetWeight's own documented 0..1 contract

	return Threshold.bEnabled ? Threshold.Evaluate(Normalized) : Normalized;
}

// ================================================================================================
// SolarOrbzSphericalGeometry - shared math for Area/Polygon/Path Mask below. See the big comment
// block above those classes' own declarations (SolarOrbzBiomeSystem.h) for why Polygon/Path use one
// local-tangent-plane projection instead of exact spherical trig.
// ================================================================================================
namespace SolarOrbzSphericalGeometry
{
	/** Same Latitude/Longitude -> direction formula as USolarOrbzStampTerrainLayer::GetStampDirection (SolarOrbzTerrainLayers.cpp) - kept as its own small copy here rather than exposed from that unrelated class, since it's 4 lines and this is the second, not the first, place this exact formula is needed in this plugin. */
	FVector DirectionFromLatLong(float LatitudeDegrees, float LongitudeDegrees)
	{
		const float LatRad = FMath::DegreesToRadians(LatitudeDegrees);
		const float LongRad = FMath::DegreesToRadians(LongitudeDegrees);
		const float CosLat = FMath::Cos(LatRad);
		return FVector(CosLat * FMath::Cos(LongRad), CosLat * FMath::Sin(LongRad), FMath::Sin(LatRad));
	}

	/**
	 * Azimuthal-equidistant projection of Direction onto the tangent plane at CenterDirection - the
	 * 2D distance between two projected points equals their true great-circle angular distance (in
	 * radians) for anything not spanning more than roughly a hemisphere from CenterDirection. TangentA
	 * is the same local-north construction used throughout this plugin (EstimateSlopeUpTo/
	 * EstimateLocalGradientUpTo in SolarOrbzTerrainLayers.cpp); degenerates at the poles the same way
	 * those already do (arbitrary but deterministic ForwardVector fallback).
	 */
	FVector2D ProjectToTangentPlane(const FVector& CenterDirection, const FVector& Direction)
	{
		const double CosAngle = FMath::Clamp((double)FVector::DotProduct(Direction, CenterDirection), -1.0, 1.0);
		const double Angle = FMath::Acos(CosAngle);

		FVector TangentA = FVector::CrossProduct(FVector::UpVector, CenterDirection);
		if (!TangentA.Normalize())
		{
			TangentA = FVector::ForwardVector; // degenerate at the poles - arbitrary but deterministic
		}
		const FVector TangentB = FVector::CrossProduct(CenterDirection, TangentA);

		const FVector Perp = (Direction - CenterDirection * CosAngle).GetSafeNormal() * Angle;
		return FVector2D(FVector::DotProduct(Perp, TangentA), FVector::DotProduct(Perp, TangentB));
	}

	/** Standard point-in-polygon test (PNPOLY, W. Randolph Franklin's ray-casting/crossing-number algorithm) - flat 2D, meaningful here because the points are already locally-projected. */
	bool PointInPolygon2D(const TArray<FVector2D>& Polygon, const FVector2D& Point)
	{
		bool bInside = false;
		const int32 NumPoints = Polygon.Num();
		for (int32 i = 0, j = NumPoints - 1; i < NumPoints; j = i++)
		{
			const FVector2D& Pi = Polygon[i];
			const FVector2D& Pj = Polygon[j];
			const bool bCrossesY = (Pi.Y > Point.Y) != (Pj.Y > Point.Y);
			if (bCrossesY && (Point.X < (Pj.X - Pi.X) * (Point.Y - Pi.Y) / (Pj.Y - Pi.Y) + Pi.X))
			{
				bInside = !bInside;
			}
		}
		return bInside;
	}

	/** Flat 2D point-to-segment distance - standard clamped projection onto the segment. */
	float PointToSegmentDistance2D(const FVector2D& Point, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D Segment = B - A;
		const float SegmentLengthSquared = Segment.SizeSquared();
		if (SegmentLengthSquared <= KINDA_SMALL_NUMBER)
		{
			return (Point - A).Size(); // degenerate (A == B)
		}
		const float T = FMath::Clamp(FVector2D::DotProduct(Point - A, Segment) / SegmentLengthSquared, 0.0f, 1.0f);
		return (Point - (A + Segment * T)).Size();
	}

	/** Shared falloff shape every mask below uses - 1.0 at SignedDistance <= -FalloffRadians (deep inside), 0.0 at SignedDistance >= +FalloffRadians (well outside), smooth between, matching FSolarOrbzMaskRange::Evaluate's own falloff feel. FalloffRadians <= 0 is a hard cutoff at SignedDistance == 0. */
	float EvaluateSignedDistanceFalloff(float SignedDistance, float FalloffRadians)
	{
		if (FalloffRadians <= KINDA_SMALL_NUMBER)
		{
			return SignedDistance <= 0.0f ? 1.0f : 0.0f;
		}
		return 1.0f - FMath::SmoothStep(-FalloffRadians, FalloffRadians, SignedDistance);
	}
}

// ================================================================================================
// USolarOrbzAreaBiomeMask
// ================================================================================================
float USolarOrbzAreaBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	const FVector Center = SolarOrbzSphericalGeometry::DirectionFromLatLong(LatitudeDegrees, LongitudeDegrees);
	const double AngleRadians = FMath::Acos(FMath::Clamp((double)FVector::DotProduct(Context.UnitDirection, Center), -1.0, 1.0));
	const float RadiusRadians = FMath::DegreesToRadians(FMath::Max(RadiusDegrees, 0.01f));
	const float FalloffRadians = FMath::DegreesToRadians(FMath::Max(FalloffDegrees, 0.0f));

	const float SignedDistance = (float)AngleRadians - RadiusRadians; // negative = inside
	float Weight = SolarOrbzSphericalGeometry::EvaluateSignedDistanceFalloff(SignedDistance, FalloffRadians);
	return bInvert ? 1.0f - Weight : Weight;
}

// ================================================================================================
// USolarOrbzPolygonBiomeMask
// ================================================================================================
float USolarOrbzPolygonBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	if (VertexLatLongDegrees.Num() < 3)
	{
		return 0.0f; // not enough points to form a region
	}

	TArray<FVector> VertexDirections;
	VertexDirections.Reserve(VertexLatLongDegrees.Num());
	FVector CentroidSum = FVector::ZeroVector;
	for (const FVector2D& LatLong : VertexLatLongDegrees)
	{
		const FVector Dir = SolarOrbzSphericalGeometry::DirectionFromLatLong(LatLong.X, LatLong.Y);
		VertexDirections.Add(Dir);
		CentroidSum += Dir;
	}
	const FVector Centroid = CentroidSum.GetSafeNormal();
	if (Centroid.IsNearlyZero())
	{
		return 0.0f; // vertices cancel out (e.g. perfectly antipodal) - no meaningful single region
	}

	TArray<FVector2D> LocalVertices;
	LocalVertices.Reserve(VertexDirections.Num());
	for (const FVector& Dir : VertexDirections)
	{
		LocalVertices.Add(SolarOrbzSphericalGeometry::ProjectToTangentPlane(Centroid, Dir));
	}
	const FVector2D LocalQuery = SolarOrbzSphericalGeometry::ProjectToTangentPlane(Centroid, Context.UnitDirection);

	const bool bInside = SolarOrbzSphericalGeometry::PointInPolygon2D(LocalVertices, LocalQuery);

	float MinEdgeDistance = TNumericLimits<float>::Max();
	const int32 NumVertices = LocalVertices.Num();
	for (int32 i = 0, j = NumVertices - 1; i < NumVertices; j = i++)
	{
		MinEdgeDistance = FMath::Min(MinEdgeDistance, SolarOrbzSphericalGeometry::PointToSegmentDistance2D(LocalQuery, LocalVertices[j], LocalVertices[i]));
	}

	const float SignedDistance = bInside ? -MinEdgeDistance : MinEdgeDistance; // radians, since the projection preserves true angular distance
	const float FalloffRadians = FMath::DegreesToRadians(FMath::Max(FalloffDegrees, 0.0f));
	const float Weight = SolarOrbzSphericalGeometry::EvaluateSignedDistanceFalloff(SignedDistance, FalloffRadians);
	return bInvert ? 1.0f - Weight : Weight;
}

// ================================================================================================
// USolarOrbzPathBiomeMask
// ================================================================================================
float USolarOrbzPathBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	if (PointsLatLongDegrees.Num() < 2)
	{
		return 0.0f; // not enough points to form a line
	}

	TArray<FVector> PointDirections;
	PointDirections.Reserve(PointsLatLongDegrees.Num());
	FVector CentroidSum = FVector::ZeroVector;
	for (const FVector2D& LatLong : PointsLatLongDegrees)
	{
		const FVector Dir = SolarOrbzSphericalGeometry::DirectionFromLatLong(LatLong.X, LatLong.Y);
		PointDirections.Add(Dir);
		CentroidSum += Dir;
	}
	const FVector Centroid = CentroidSum.GetSafeNormal();
	if (Centroid.IsNearlyZero())
	{
		return 0.0f;
	}

	TArray<FVector2D> LocalPoints;
	LocalPoints.Reserve(PointDirections.Num());
	for (const FVector& Dir : PointDirections)
	{
		LocalPoints.Add(SolarOrbzSphericalGeometry::ProjectToTangentPlane(Centroid, Dir));
	}
	const FVector2D LocalQuery = SolarOrbzSphericalGeometry::ProjectToTangentPlane(Centroid, Context.UnitDirection);

	float MinDistance = TNumericLimits<float>::Max();
	for (int32 i = 0; i + 1 < LocalPoints.Num(); ++i)
	{
		MinDistance = FMath::Min(MinDistance, SolarOrbzSphericalGeometry::PointToSegmentDistance2D(LocalQuery, LocalPoints[i], LocalPoints[i + 1]));
	}

	const float WidthRadians = FMath::DegreesToRadians(FMath::Max(WidthDegrees, 0.0f));
	const float FalloffRadians = FMath::DegreesToRadians(FMath::Max(FalloffDegrees, 0.0f));
	const float SignedDistance = MinDistance - WidthRadians;
	return SolarOrbzSphericalGeometry::EvaluateSignedDistanceFalloff(SignedDistance, FalloffRadians);
}

// ================================================================================================
// USolarOrbzBorderBlendBiomeMask
// ================================================================================================
float USolarOrbzBorderBlendBiomeMask::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	if (!ReferenceMask)
	{
		return 0.0f;
	}
	const float ReferenceWeight = ReferenceMask->GetWeight(Context);
	const float DistanceFromBoundary = FMath::Abs(ReferenceWeight - 0.5f);
	return FMath::Clamp(1.0f - DistanceFromBoundary / FMath::Max(BandHalfWidth, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
}

// ================================================================================================
// USolarOrbzBiome
// ================================================================================================
float USolarOrbzBiome::GetWeight(const FSolarOrbzBiomeSampleContext& Context) const
{
	if (!Mask)
	{
		return 1.0f;
	}

	// A Composite Mask can reference other Biomes, whose own Mask can itself be a Composite Mask
	// referencing back - an accidental cycle here would recurse forever and hang/crash the editor.
	// This depth cap turns that into a loud, recoverable warning instead. thread_local so this is
	// safe even if biome evaluation is ever parallelized across vertices later.
	static thread_local int32 RecursionDepth = 0;
	constexpr int32 MaxRecursionDepth = 16;

	if (RecursionDepth >= MaxRecursionDepth)
	{
		UE_LOG(LogTemp, Warning, TEXT("SolarOrbz Biome: '%s' hit max mask recursion depth (%d) - you likely have a Composite Mask reference cycle (Biome A's mask includes Biome B, whose mask includes Biome A, directly or through more steps). Returning 0 for this branch."), *GetName(), MaxRecursionDepth);
		return 0.0f;
	}

	++RecursionDepth;
	const float Weight = Mask->GetWeight(Context);
	--RecursionDepth;

	return Weight;
}

int32 USolarOrbzBiome::SelectMaterialIndex(const FSolarOrbzBiomeSampleContext& Context) const
{
	if (Materials.Num() == 0)
	{
		return INDEX_NONE;
	}
	if (Materials.Num() == 1)
	{
		return 0;
	}

	switch (MaterialSelector)
	{
	case ESolarOrbzBiomeMaterialSelector::Slope:
	default:
	{
		// Evenly split [FlatSlope, SteepSlope] into (Materials.Num() - 1) bands and round to the
		// nearest index - hard cutoff at each band boundary, no blend (see Materials' own comment
		// for why). Note the first and last bands are each half the width of an interior band
		// (round-to-nearest-index maps [0, 0.5/LastIndex) to index 0 and (1-0.5/LastIndex, 1] to
		// LastIndex, while an interior index gets the full 1/LastIndex span either side) - accepted
		// as a minor, deliberate asymmetry rather than a special-cased remap, since with only 2
		// materials (the common case) there's exactly one boundary and no asymmetry to speak of.
		const float Range = FMath::Max(SteepSlope - FlatSlope, KINDA_SMALL_NUMBER);
		const float T = FMath::Clamp((Context.Slope - FlatSlope) / Range, 0.0f, 1.0f);
		const int32 LastIndex = Materials.Num() - 1;
		return FMath::Clamp(FMath::RoundToInt(T * LastIndex), 0, LastIndex);
	}
	}
}

// ================================================================================================
// USolarOrbzBiomeStack
// ================================================================================================
void USolarOrbzBiomeStack::EvaluateLayerWeights(const FSolarOrbzBiomeSampleContext& Context, TArray<float>& OutWeights) const
{
	OutWeights.Reset(Layers.Num());
	for (const FSolarOrbzBiomeLayerEntry& Entry : Layers)
	{
		const float MaskWeight = Entry.Biome ? Entry.Biome->GetWeight(Context) : 0.0f;
		OutWeights.Add(FMath::Clamp(MaskWeight * Entry.Opacity, 0.0f, 1.0f));
	}
}

USolarOrbzBiome* USolarOrbzBiomeStack::GetDominantBiome(const FSolarOrbzBiomeSampleContext& Context) const
{
	TArray<float> LayerWeights;
	EvaluateLayerWeights(Context, LayerWeights);
	return GetDominantBiome(Context, LayerWeights);
}

USolarOrbzBiome* USolarOrbzBiomeStack::GetDominantBiome(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights) const
{
	USolarOrbzBiome* Best = nullptr;
	float BestWeight = 0.0f;

	// Iterate forward and use >= so later (topmost) entries win ties, matching the Photoshop-style ordering.
	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		const FSolarOrbzBiomeLayerEntry& Entry = Layers[i];
		if (!Entry.Biome)
		{
			continue;
		}

		const float Weight = LayerWeights.IsValidIndex(i) ? LayerWeights[i] : 0.0f;
		if (Weight >= BestWeight)
		{
			BestWeight = Weight;
			Best = Entry.Biome;
		}
	}

	return BestWeight > KINDA_SMALL_NUMBER ? Best : nullptr;
}

UMaterialInterface* USolarOrbzBiomeStack::SelectMaterial(const FSolarOrbzBiomeSampleContext& Context) const
{
	TArray<float> LayerWeights;
	EvaluateLayerWeights(Context, LayerWeights);
	return SelectMaterial(Context, LayerWeights);
}

UMaterialInterface* USolarOrbzBiomeStack::SelectMaterial(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights) const
{
	const USolarOrbzBiome* Dominant = GetDominantBiome(Context, LayerWeights);
	if (!Dominant)
	{
		return nullptr;
	}

	const int32 MaterialIndex = Dominant->SelectMaterialIndex(Context);
	return Dominant->Materials.IsValidIndex(MaterialIndex) ? Dominant->Materials[MaterialIndex] : nullptr;
}

float USolarOrbzBiomeStack::EvaluateBiomeTerrainContribution(const FSolarOrbzBiomeSampleContext& Context) const
{
	TArray<float> LayerWeights;
	EvaluateLayerWeights(Context, LayerWeights);
	return EvaluateBiomeTerrainContribution(Context, LayerWeights);
}

float USolarOrbzBiomeStack::EvaluateBiomeTerrainContribution(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights) const
{
	float Accum = 0.0f;

	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		const FSolarOrbzBiomeLayerEntry& Entry = Layers[i];
		if (!Entry.Biome || !Entry.Biome->TerrainDetail)
		{
			continue;
		}

		const float Weight = LayerWeights.IsValidIndex(i) ? LayerWeights[i] : 0.0f;
		if (Weight <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		const float DetailHeight = Entry.Biome->TerrainDetail->EvaluateHeight(Context.UnitDirection, Context.UV);
		Accum += DetailHeight * Weight;
	}

	return Accum;
}

void USolarOrbzBiomeStack::GetUniqueBiomes(TArray<USolarOrbzBiome*>& OutBiomes) const
{
	OutBiomes.Reset();
	for (const FSolarOrbzBiomeLayerEntry& Entry : Layers)
	{
		if (Entry.Biome && !OutBiomes.Contains(Entry.Biome))
		{
			OutBiomes.Add(Entry.Biome);
		}
	}
}

void USolarOrbzBiomeStack::EvaluateTopWeightedBiomes(const FSolarOrbzBiomeSampleContext& Context, int32 MaxBiomes, TArray<int32>& OutBiomeIndices, TArray<float>& OutWeights) const
{
	TArray<float> LayerWeights;
	EvaluateLayerWeights(Context, LayerWeights);

	TArray<USolarOrbzBiome*> UniqueBiomes;
	GetUniqueBiomes(UniqueBiomes);

	EvaluateTopWeightedBiomes(Context, LayerWeights, UniqueBiomes, MaxBiomes, OutBiomeIndices, OutWeights);
}

void USolarOrbzBiomeStack::EvaluateTopWeightedBiomes(const FSolarOrbzBiomeSampleContext& Context, const TArray<float>& LayerWeights, const TArray<USolarOrbzBiome*>& UniqueBiomes, int32 MaxBiomes, TArray<int32>& OutBiomeIndices, TArray<float>& OutWeights) const
{
	OutBiomeIndices.Reset();
	OutWeights.Reset();

	if (UniqueBiomes.Num() == 0)
	{
		return;
	}

	// Weight per UNIQUE biome - if the same biome appears in more than one layer entry, its
	// strongest entry wins rather than double-counting.
	TArray<float> WeightPerUniqueBiome;
	WeightPerUniqueBiome.Init(0.0f, UniqueBiomes.Num());

	for (int32 i = 0; i < Layers.Num(); ++i)
	{
		const FSolarOrbzBiomeLayerEntry& Entry = Layers[i];
		if (!Entry.Biome)
		{
			continue;
		}
		const int32 Index = UniqueBiomes.IndexOfByKey(Entry.Biome);
		if (Index == INDEX_NONE)
		{
			continue;
		}
		const float Weight = LayerWeights.IsValidIndex(i) ? LayerWeights[i] : 0.0f;
		WeightPerUniqueBiome[Index] = FMath::Max(WeightPerUniqueBiome[Index], Weight);
	}

	// Sort unique-biome indices by weight, strongest first.
	TArray<int32> SortedIndices;
	SortedIndices.Reserve(UniqueBiomes.Num());
	for (int32 i = 0; i < UniqueBiomes.Num(); ++i)
	{
		SortedIndices.Add(i);
	}
	SortedIndices.Sort([&WeightPerUniqueBiome](int32 A, int32 B) { return WeightPerUniqueBiome[A] > WeightPerUniqueBiome[B]; });

	float TotalWeight = 0.0f;
	for (int32 i = 0; i < SortedIndices.Num() && OutBiomeIndices.Num() < MaxBiomes; ++i)
	{
		const int32 Index = SortedIndices[i];
		const float Weight = WeightPerUniqueBiome[Index];
		if (Weight <= KINDA_SMALL_NUMBER)
		{
			break; // sorted descending - nothing further is nonzero either
		}
		OutBiomeIndices.Add(Index);
		OutWeights.Add(Weight);
		TotalWeight += Weight;
	}

	// Renormalize so the returned weights sum to 1 - ready for a caller that wants a genuine
	// multi-biome weighted blend (e.g. a future shader-side use); SelectMaterial below doesn't use
	// this at all, since it always wants a single hard winner (GetDominantBiome), not a blend.
	if (TotalWeight > KINDA_SMALL_NUMBER)
	{
		for (float& Weight : OutWeights)
		{
			Weight /= TotalWeight;
		}
	}
}

// ================================================================================================
// USolarOrbzBiomeStack::ExportScatterEntriesToPCGExMeshCollectionJson - see the doc comment on the
// declaration in SolarOrbzBiomeSystem.h for the verified schema and what's deliberately left out
// (Density/MinSlope/MaxSlope).
// ================================================================================================
void USolarOrbzBiomeStack::ExportScatterEntriesToPCGExMeshCollectionJson()
{
	TArray<USolarOrbzBiome*> UniqueBiomes;
	GetUniqueBiomes(UniqueBiomes);

	TArray<TSharedPtr<FJsonValue>> EntriesArray;
	int32 SkippedCount = 0;

	for (const USolarOrbzBiome* Biome : UniqueBiomes)
	{
		if (!Biome)
		{
			continue;
		}

		for (const FSolarOrbzBiomeScatterEntry& ScatterEntry : Biome->ScatterEntries)
		{
			if (ScatterEntry.ScatterMesh.IsNull())
			{
				UE_LOG(LogSolarOrbzBiome, Warning,
					TEXT("SolarOrbz Biome: scatter entry '%s' on biome '%s' has no ScatterMesh assigned - skipped from the PCGEx Mesh Collection export."),
					*ScatterEntry.Tag.ToString(), *Biome->GetName());
				++SkippedCount;
				continue;
			}

			TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
			EntryObj->SetStringField(TEXT("type"), TEXT("Mesh"));
			EntryObj->SetStringField(TEXT("StaticMesh"), ScatterEntry.ScatterMesh.ToSoftObjectPath().ToString());
			EntryObj->SetStringField(TEXT("Category"), ScatterEntry.Tag.ToString());
			EntryObj->SetNumberField(TEXT("Weight"), 1);
			EntriesArray.Add(MakeShared<FJsonValueObject>(EntryObj));

			UE_LOG(LogSolarOrbzBiome, Log,
				TEXT("SolarOrbz Biome: exported scatter entry '%s' (biome '%s', mesh '%s') - density %.4f/m^2, slope [%.2f, %.2f] aren't part of this JSON (PCGEx models those on its own Distribute-family node); enter them there by hand."),
				*ScatterEntry.Tag.ToString(), *Biome->GetName(), *ScatterEntry.ScatterMesh.ToSoftObjectPath().ToString(),
				ScatterEntry.DensityPerSquareMeter, ScatterEntry.MinSlope, ScatterEntry.MaxSlope);
		}
	}

	if (EntriesArray.Num() == 0)
	{
		UE_LOG(LogSolarOrbzBiome, Warning,
			TEXT("SolarOrbz Biome: no ScatterEntry in this stack has a ScatterMesh assigned (%d skipped) - nothing to export."),
			SkippedCount);
		return;
	}

	TSharedPtr<FJsonObject> MembersObj = MakeShared<FJsonObject>();
	MembersObj->SetArrayField(TEXT("Entries"), EntriesArray);

	TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
	DataObj->SetStringField(TEXT("class"), TEXT("/Script/PCGExCollections.PCGExMeshCollection"));
	DataObj->SetObjectField(TEXT("members"), MembersObj);

	TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();
	RootObj->SetStringField(TEXT("format"), TEXT("pcgex.object"));
	RootObj->SetNumberField(TEXT("version"), 1);
	RootObj->SetObjectField(TEXT("data"), DataObj);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer);

	const FString FilePath = FPaths::ProjectSavedDir() / TEXT("SolarOrbzPCGExExports") / (GetName() + TEXT("_ScatterCollection.json"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree=*/true);

	if (!FFileHelper::SaveStringToFile(OutputString, *FilePath))
	{
		UE_LOG(LogSolarOrbzBiome, Warning, TEXT("SolarOrbz Biome: failed to write PCGEx Mesh Collection export to %s."), *FilePath);
		return;
	}

	UE_LOG(LogSolarOrbzBiome, Log,
		TEXT("SolarOrbz Biome: wrote %d scatter entries (%d skipped - no ScatterMesh) to %s. Feed this to PCGEx's ImportJson/CreateAsset tool action (via the PCGEx Assistant plugin) to get a real Mesh Collection asset."),
		EntriesArray.Num(), SkippedCount, *FilePath);
}

