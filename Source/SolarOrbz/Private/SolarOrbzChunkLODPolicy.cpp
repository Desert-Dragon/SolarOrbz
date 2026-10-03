// SolarOrbzChunkLODPolicy implementation. See the header for the full rationale (metric choice,
// threshold derivation, units, hysteresis) - kept out of this file so it isn't duplicated/drifted.

#include "SolarOrbzChunkLODPolicy.h"
#include "SolarOrbzIcoSphereChunk.h" // FSolarOrbzChunkAddress::MaxDepth only - see ShouldSplit's CurrentDepth parameter.

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

bool FSolarOrbzChunkLODPolicy::ShouldSplit(
	const FVector& CornerA,
	const FVector& CornerB,
	const FVector& CornerC,
	const FVector& ViewerWorldPosition,
	const FSolarOrbzChunkLODSettings& Settings,
	int32 CurrentDepth)
{
	if (CurrentDepth != INDEX_NONE && CurrentDepth >= FSolarOrbzChunkAddress::MaxDepth)
	{
		return false;
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
