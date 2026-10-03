// See SolarOrbzChunkSkirtBuilder.h for the algorithm, the exact vertex-indexing scheme this
// re-derives from GenerateChunk, winding caveat, and verification methodology.

#include "SolarOrbzChunkSkirtBuilder.h"
#include "SolarOrbzIcoSphere.h"

namespace
{
	// Same PointIndex formula GenerateChunk uses (SolarOrbzIcoSphereChunk.cpp) - transcribed here,
	// not re-exported from there, since this file is meant to stay a pure post-process that doesn't
	// require any change to GenerateChunk itself. See this file's header for why re-deriving this
	// (rather than assuming a different scheme) matters.
	FORCEINLINE int32 PointIndex(int32 I, int32 J)
	{
		return I * (I + 1) / 2 + J;
	}

	// The 3 ordered boundary vertex lists - see the header comment for the WeightA/B/C derivation.
	// AB: A (I=0) -> B (I=Resolution). BC: B (J=0) -> C (J=Resolution). CA: C (I=Resolution) -> A (I=0).
	void BuildBoundaryLists(int32 Resolution, TArray<int32>& OutAB, TArray<int32>& OutBC, TArray<int32>& OutCA)
	{
		OutAB.Reset();
		OutBC.Reset();
		OutCA.Reset();

		OutAB.Reserve(Resolution + 1);
		for (int32 I = 0; I <= Resolution; ++I)
		{
			OutAB.Add(PointIndex(I, 0));
		}

		OutBC.Reserve(Resolution + 1);
		for (int32 J = 0; J <= Resolution; ++J)
		{
			OutBC.Add(PointIndex(Resolution, J));
		}

		OutCA.Reserve(Resolution + 1);
		for (int32 I = Resolution; I >= 0; --I)
		{
			OutCA.Add(PointIndex(I, I));
		}
	}
}

void FSolarOrbzChunkSkirtBuilder::AppendSkirts(int32 Resolution, double SkirtDepth, FSolarOrbzIcoSphereMeshData& InOutMeshData)
{
	Resolution = FMath::Max(Resolution, 1);

	TArray<int32> AB, BC, CA;
	BuildBoundaryLists(Resolution, AB, BC, CA);

	// Lazy-create-or-reuse cache: each ORIGINAL vertex index maps to exactly one skirt counterpart,
	// even though the 3 corners each appear in two of the three boundary lists above - see the
	// header's own comment on why this matters (a shared corner must reuse one skirt vertex, not get
	// two, or the skirt itself would gap exactly at the corner).
	TMap<int32, int32> OriginalToSkirt;

	auto GetOrCreateSkirtVertex = [&InOutMeshData, &OriginalToSkirt, SkirtDepth](int32 OriginalIndex) -> int32
	{
		if (const int32* Found = OriginalToSkirt.Find(OriginalIndex))
		{
			return *Found;
		}

		const FVector& OriginalPosition = InOutMeshData.Vertices[OriginalIndex];
		const FVector& OriginalNormal = InOutMeshData.Normals[OriginalIndex];

		// Pulled inward along the vertex's own FINAL normal - see the header comment for why the
		// normal (not the radial/from-planet-center direction) is the right reference once terrain
		// displacement is involved, and why this is safe to call after GenerateChunk's own
		// RecomputeSmoothNormals has already run.
		const FVector SkirtPosition = OriginalPosition - OriginalNormal * SkirtDepth;

		const int32 NewIndex = InOutMeshData.Vertices.Num();
		InOutMeshData.Vertices.Add(SkirtPosition);
		InOutMeshData.Normals.Add(OriginalNormal);
		InOutMeshData.UVs.Add(InOutMeshData.UVs[OriginalIndex]);
		InOutMeshData.Tangents.Add(InOutMeshData.Tangents[OriginalIndex]);

		OriginalToSkirt.Add(OriginalIndex, NewIndex);
		return NewIndex;
	};

	// For each ordered boundary list, for every consecutive pair of original boundary vertices,
	// append a quad (2 triangles) connecting that segment to its skirt counterpart directly "below"
	// it. Winding picked for internal consistency only - UNVERIFIED without a renderer, same standing
	// caveat GenerateChunk's own triangles already carry (see the header's own comment on this).
	const TArray<int32>* BoundaryLists[3] = { &AB, &BC, &CA };
	for (const TArray<int32>* EdgeList : BoundaryLists)
	{
		for (int32 K = 0; K < EdgeList->Num() - 1; ++K)
		{
			const int32 Original0 = (*EdgeList)[K];
			const int32 Original1 = (*EdgeList)[K + 1];
			const int32 Skirt0 = GetOrCreateSkirtVertex(Original0);
			const int32 Skirt1 = GetOrCreateSkirtVertex(Original1);

			InOutMeshData.Triangles.Add(Original0);
			InOutMeshData.Triangles.Add(Original1);
			InOutMeshData.Triangles.Add(Skirt1);

			InOutMeshData.Triangles.Add(Original0);
			InOutMeshData.Triangles.Add(Skirt1);
			InOutMeshData.Triangles.Add(Skirt0);
		}
	}
}
