// SolarOrbzChunkPointLocation implementation. See the header for the full algorithm write-up,
// sign-convention derivation, boundary tie-break rule, and verification methodology - this file
// intentionally keeps its own comments short and points back there rather than duplicating it.

#include "SolarOrbzChunkPointLocation.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzChunkPointLocation, Log, All);

namespace
{
	// Point-in-spherical-triangle test, fixed-sign (>= 0 on all three edges, no opposite-corner
	// comparison) - correct for every triangle this project produces because
	// GetBaseIcosahedron/GetCornerUnitDirections never vary their CCW-viewed-from-outside winding.
	// See the header's "why no opposite-corner comparison is needed" section. Inclusive (>= 0, not
	// > 0) so a point exactly on a shared edge is still "inside" both adjacent triangles rather than
	// neither - see the header's tie-break section for how the caller resolves that.
	bool PointInSphericalTriangle(const FVector& A, const FVector& B, const FVector& C, const FVector& P)
	{
		const double SideAB = FVector::DotProduct(FVector::CrossProduct(A, B), P);
		const double SideBC = FVector::DotProduct(FVector::CrossProduct(B, C), P);
		const double SideCA = FVector::DotProduct(FVector::CrossProduct(C, A), P);
		return SideAB >= 0.0 && SideBC >= 0.0 && SideCA >= 0.0;
	}

	// Classifies P into one of the 20 base icosahedron faces - first match wins in ascending face-
	// index order (the tie-break rule, see header). Returns INDEX_NONE only in the practically-
	// unreachable case that none of the 20 faces matched (see header's verification notes - never
	// observed; the 20 faces are a full partition of the sphere).
	int32 ClassifyBaseFace(const FVector& P)
	{
		TArray<FVector> BaseVertices;
		TArray<FIntVector> BaseFaces;
		FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron(BaseVertices, BaseFaces);

		for (int32 FaceIndex = 0; FaceIndex < BaseFaces.Num(); ++FaceIndex)
		{
			const FIntVector& Face = BaseFaces[FaceIndex];
			if (PointInSphericalTriangle(BaseVertices[Face.X], BaseVertices[Face.Y], BaseVertices[Face.Z], P))
			{
				return FaceIndex;
			}
		}

		return INDEX_NONE;
	}
}

FSolarOrbzChunkAddress FSolarOrbzChunkPointLocator::FindChunkContainingDirection(const FVector& UnitDirection, int32 TargetDepth)
{
	FVector P = UnitDirection;
	if (!P.Normalize())
	{
		// Degenerate (zero-length, or otherwise unnormalizable) input - see header comment on the
		// FVector::ZeroVector special case. Substitute a real, arbitrary-but-deterministic direction
		// rather than letting a zero vector silently resolve to "base face 0, every level" without
		// any record of why.
		UE_LOG(LogSolarOrbzChunkPointLocation, Warning,
			TEXT("SolarOrbz ChunkPointLocation: FindChunkContainingDirection received a degenerate (near-zero-length) direction - falling back to FVector::ForwardVector."));
		P = FVector::ForwardVector;
	}

	TargetDepth = FMath::Clamp(TargetDepth, 0, FSolarOrbzChunkAddress::MaxDepth);

	const int32 BaseFaceIndex = ClassifyBaseFace(P);
	if (BaseFaceIndex == INDEX_NONE)
	{
		// Practically unreachable - see header's verification notes (259,000+ cases, zero gaps).
		// Fall back to base face 0 / Depth 0 rather than returning an uninitialized address.
		UE_LOG(LogSolarOrbzChunkPointLocation, Error,
			TEXT("SolarOrbz ChunkPointLocation: ClassifyBaseFace found no matching base face for direction (%s) - this should be unreachable (the 20 base faces are a full partition of the sphere). Falling back to BaseFaceIndex 0, Depth 0."),
			*P.ToString());
		return FSolarOrbzChunkAddress(0, 0, 0);
	}

	FSolarOrbzChunkAddress Current(BaseFaceIndex, 0, 0);

	for (int32 Level = 0; Level < TargetDepth; ++Level)
	{
		FSolarOrbzChunkAddress Children[4];
		Current.GetChildren(Children);

		int32 ChosenChild = INDEX_NONE;
		for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
		{
			FVector A, B, C;
			Children[ChildIndex].GetCornerUnitDirections(A, B, C);
			if (PointInSphericalTriangle(A, B, C, P))
			{
				ChosenChild = ChildIndex;
				break; // first match wins ties - see header's tie-break section
			}
		}

		if (ChosenChild == INDEX_NONE)
		{
			// Practically unreachable - see header's verification notes. Stop descending here
			// rather than guessing wrong; the caller gets a shallower-than-requested but still
			// genuinely-containing chunk instead of a silently incorrect deeper one.
			UE_LOG(LogSolarOrbzChunkPointLocation, Error,
				TEXT("SolarOrbz ChunkPointLocation: FindChunkContainingDirection - none of a chunk's 4 children matched direction (%s) at level %d (BaseFace=%d) - this should be unreachable (a chunk's 4 children are a full partition of their parent). Stopping descent at Depth %d instead of the requested %d."),
				*P.ToString(), Level, BaseFaceIndex, Current.Depth, TargetDepth);
			break;
		}

		Current = Children[ChosenChild];
	}

	return Current;
}
