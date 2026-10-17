// SolarOrbzChunkLODPolicy implementation. See the header for the full rationale (metric choice,
// threshold derivation, units, hysteresis) - kept out of this file so it isn't duplicated/drifted.

#include "SolarOrbzChunkLODPolicy.h"
#include "SolarOrbzIcoSphereChunk.h" // FSolarOrbzChunkAddress::MaxDepth only - see ShouldSplit's CurrentDepth parameter.
#include "SolarOrbzIcoSphere.h" // FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron - see ComputeMaxUsefulDepth.

namespace
{
	// A chunk's own corners sit (by construction - see the header comment on input shape) essentially
	// exactly on the sphere, so Length() of any one of them is already a good radius estimate; this
	// averages all 3 anyway rather than trusting just one, purely as cheap insurance against whatever
	// floating-point drift a terrain-displaced corner (a future caller - see the header's "input
	// shape" note) might carry.
	double EstimateChunkRadius(const FVector& CornerA, const FVector& CornerB, const FVector& CornerC)
	{
		return (CornerA.Length() + CornerB.Length() + CornerC.Length()) / 3.0;
	}

	// The angle (radians) between two ADJACENT base-icosahedron vertices, read off
	// FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron()'s own vertex data rather than hand-typed as a
	// trig constant - this plugin's established convention (see e.g. GetEdgeNeighbor/
	// GetPentagonVertexNeighbors' own verification notes in SolarOrbzIcoSphereChunk.h). Vertices[0] and
	// Vertices[1] are confirmed adjacent via the base face list's second entry, {0, 5, 1} (both appear
	// in a shared face with Vertices[0]) - see SolarOrbzIcoSphere.cpp's GetBaseIcosahedron. Every base
	// edge has the same angular length by construction (a regular icosahedron), so any one adjacent
	// pair gives the same answer as any other.
	double ComputeBaseEdgeAngleRadians()
	{
		TArray<FVector> BaseVertices;
		TArray<FIntVector> BaseFaces;
		FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron(BaseVertices, BaseFaces);
		const FVector UnitA = BaseVertices[0].GetSafeNormal();
		const FVector UnitB = BaseVertices[1].GetSafeNormal();
		return FMath::Acos(FMath::Clamp(FVector::DotProduct(UnitA, UnitB), -1.0, 1.0));
	}
}

double FSolarOrbzChunkLODPolicy::ComputeLongestEdgeLength(const FVector& CornerA, const FVector& CornerB, const FVector& CornerC)
{
	const double AB = FVector::Dist(CornerA, CornerB);
	const double BC = FVector::Dist(CornerB, CornerC);
	const double CA = FVector::Dist(CornerC, CornerA);
	return FMath::Max3(AB, BC, CA);
}

double FSolarOrbzChunkLODPolicy::ComputeNearestDistance(const FVector& CornerA, const FVector& CornerB, const FVector& CornerC, const FVector& ViewerWorldPosition)
{
	// Centroid of the 3 corners, re-projected back onto the sphere those corners sit on - NOT the
	// true geometric center of the spherical triangle (that would need actual spherical-geometry
	// math), just a cheap stand-in "somewhere in the middle" point. See the header comment for why
	// this, combined with the 3 corners, is good enough for a first-pass distance estimate.
	const FVector Centroid = (CornerA + CornerB + CornerC) / 3.0;
	const double CentroidLength = Centroid.Length();
	FVector CentroidOnSphere = Centroid;
	if (CentroidLength > (double)KINDA_SMALL_NUMBER)
	{
		CentroidOnSphere = Centroid * (EstimateChunkRadius(CornerA, CornerB, CornerC) / CentroidLength);
	}

	double NearestDistance = FVector::Dist(ViewerWorldPosition, CornerA);
	NearestDistance = FMath::Min(NearestDistance, FVector::Dist(ViewerWorldPosition, CornerB));
	NearestDistance = FMath::Min(NearestDistance, FVector::Dist(ViewerWorldPosition, CornerC));
	NearestDistance = FMath::Min(NearestDistance, FVector::Dist(ViewerWorldPosition, CentroidOnSphere));
	return NearestDistance;
}

double FSolarOrbzChunkLODPolicy::ComputeScreenSizeRatio(const FVector& CornerA, const FVector& CornerB, const FVector& CornerC, const FVector& ViewerWorldPosition)
{
	const double EdgeLength = ComputeLongestEdgeLength(CornerA, CornerB, CornerC);
	const double RawDistance = ComputeNearestDistance(CornerA, CornerB, CornerC, ViewerWorldPosition);

	// Guard against a division blowing up to +inf (not just "very large") if the viewer sits exactly
	// on one of the 4 sampled points - a legitimate case (e.g. a viewer standing exactly at a chunk
	// corner), not an error, and the function should still answer "yes, split, this is as close as it
	// gets" rather than hand back a non-finite ratio a caller then has to separately guard against.
	const double SafeDistance = FMath::Max(RawDistance, (double)KINDA_SMALL_NUMBER);
	return EdgeLength / SafeDistance;
}

int32 FSolarOrbzChunkLODPolicy::ComputeMaxUsefulDepth(double Radius, int32 ChunkResolution, double TargetQuadEdgeMetersCm)
{
	const int32 SafeChunkResolution = FMath::Max(ChunkResolution, 1);
	const double SafeTargetQuadEdge = FMath::Max(TargetQuadEdgeMetersCm, (double)KINDA_SMALL_NUMBER);
	const double BaseEdgeLength = Radius * ComputeBaseEdgeAngleRadians();

	// BaseEdgeLength / (2^D * SafeChunkResolution) <= SafeTargetQuadEdge, solved for the smallest D.
	const double RawDepth = FMath::Log2(BaseEdgeLength / (SafeChunkResolution * SafeTargetQuadEdge));
	const int32 Depth = FMath::CeilToInt(RawDepth);
	return FMath::Clamp(Depth, 0, FSolarOrbzChunkAddress::MaxDepth);
}

bool FSolarOrbzChunkLODPolicy::ShouldSplit(
	const FVector& CornerA,
	const FVector& CornerB,
	const FVector& CornerC,
	const FVector& ViewerWorldPosition,
	const FSolarOrbzChunkLODSettings& Settings,
	int32 CurrentDepth,
	int32 EffectiveMaxDepth)
{
	if (CurrentDepth != INDEX_NONE)
	{
		const int32 ActualMaxDepth = FMath::Min(EffectiveMaxDepth, FSolarOrbzChunkAddress::MaxDepth);
		if (CurrentDepth >= ActualMaxDepth)
		{
			return false;
		}
	}

	const double Ratio = ComputeScreenSizeRatio(CornerA, CornerB, CornerC, ViewerWorldPosition);
	return Ratio > Settings.SplitScreenSizeRatio;
}

bool FSolarOrbzChunkLODPolicy::ShouldMerge(
	const FVector& CornerA,
	const FVector& CornerB,
	const FVector& CornerC,
	const FVector& ViewerWorldPosition,
	const FSolarOrbzChunkLODSettings& Settings)
{
	const double Ratio = ComputeScreenSizeRatio(CornerA, CornerB, CornerC, ViewerWorldPosition);
	return Ratio < Settings.MergeScreenSizeRatio;
}
