// SolarOrbz - Stamp terrain layer implementation.

#include "SolarOrbzStampTerrainLayer.h"
#include "Engine/Texture2D.h"

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
		return Height01 * Amplitude * EdgeWeight;
	}

	// No heightmap - procedural dome (or crater) using a smooth cosine profile so the peak is C1-continuous.
	const float Shape = 0.5f * (1.0f + FMath::Cos(PI * FMath::Clamp(NormalizedDist, 0.0f, 1.0f))); // 1 at center, 0 at edge

	float HeightValue;
	if (!bCrater)
	{
		HeightValue = Shape * Amplitude;
	}
	else
	{
		const float CraterDepth = -Shape * Amplitude;

		float RimShape = 0.0f;
		if (CraterRimHeight > 0.0f)
		{
			constexpr float RimCenter = 0.8f;
			constexpr float RimWidth = 0.15f;
			const float RimT = FMath::Clamp(1.0f - FMath::Abs(NormalizedDist - RimCenter) / RimWidth, 0.0f, 1.0f);
			RimShape = RimT * RimT * (3.0f - 2.0f * RimT); // smoothstep-shaped bump
		}

		HeightValue = CraterDepth + RimShape * CraterRimHeight * Amplitude;
	}

	return HeightValue * EdgeWeight;
}
