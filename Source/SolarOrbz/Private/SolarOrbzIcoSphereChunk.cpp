// SolarOrbzIcoSphereChunk implementation. See the header and Docs/ChunkedPlanetTerrain.md for the
// overall design and what's deliberately not built yet.

#include "SolarOrbzIcoSphereChunk.h"
#include "SolarOrbzTerrainLayers.h"
#include "SolarOrbzLatLongGrid.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzChunk, Log, All);

FSolarOrbzChunkAddress FSolarOrbzChunkAddress::GetParent() const
{
	if (Depth <= 0)
	{
		// Documented behavior (see header) - Depth 0 is the whole base face, nothing above it.
		return *this;
	}

	// Mask off the top 2 bits (the child choice made at the deepest level) - everything below that
	// level's bits is unaffected, since PathBits packs level 0 at the least-significant pair.
	const uint64 ParentMask = (Depth > 1) ? ((uint64(1) << (2 * (Depth - 1))) - 1) : 0;
	return FSolarOrbzChunkAddress(BaseFaceIndex, Depth - 1, PathBits & ParentMask);
}

void FSolarOrbzChunkAddress::GetChildren(FSolarOrbzChunkAddress OutChildren[4]) const
{
	if (Depth >= MaxDepth)
	{
		UE_LOG(LogSolarOrbzChunk, Warning, TEXT("SolarOrbz Chunk: GetChildren called at MaxDepth (%d) - returning this chunk's own address for all 4 children instead of going deeper."), MaxDepth);
		for (int32 i = 0; i < 4; ++i)
		{
			OutChildren[i] = *this;
		}
		return;
	}

	const int32 ChildDepth = Depth + 1;
	for (int32 Child = 0; Child < 4; ++Child)
	{
		const uint64 ChildPathBits = PathBits | (((uint64)Child) << (2 * Depth));
		OutChildren[Child] = FSolarOrbzChunkAddress(BaseFaceIndex, ChildDepth, ChildPathBits);
	}
}

void FSolarOrbzChunkAddress::GetCornerUnitDirections(FVector& OutA, FVector& OutB, FVector& OutC) const
{
	TArray<FVector> BaseVertices;
	TArray<FIntVector> BaseFaces;
	FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron(BaseVertices, BaseFaces);

	const FIntVector& Face = BaseFaces[BaseFaceIndex];
	FVector A = BaseVertices[Face.X];
	FVector B = BaseVertices[Face.Y];
	FVector C = BaseVertices[Face.Z];

	for (int32 Level = 0; Level < Depth; ++Level)
	{
		const int32 Child = (int32)((PathBits >> (2 * Level)) & 0x3);

		// Same midpoint math as FSolarOrbzIcoSphereGenerator::GetOrCreateMidpoint (the *0.5 doesn't
		// change a normalized result, kept for visual parity with that function).
		const FVector MidAB = ((A + B) * 0.5).GetSafeNormal();
		const FVector MidBC = ((B + C) * 0.5).GetSafeNormal();
		const FVector MidCA = ((C + A) * 0.5).GetSafeNormal();

		FVector NewA, NewB, NewC;
		switch (Child)
		{
		case 0: NewA = A; NewB = MidAB; NewC = MidCA; break;      // corner child at A: (A, AB, CA)
		case 1: NewA = B; NewB = MidBC; NewC = MidAB; break;      // corner child at B: (B, BC, AB)
		case 2: NewA = C; NewB = MidCA; NewC = MidBC; break;      // corner child at C: (C, CA, BC)
		default: NewA = MidAB; NewB = MidBC; NewC = MidCA; break; // center child: (AB, BC, CA)
		}

		A = NewA; B = NewB; C = NewC;
	}

	OutA = A; OutB = B; OutC = C;
}

void FSolarOrbzIcoSphereChunkGenerator::GenerateChunk(
	const FSolarOrbzChunkAddress& Address,
	int32 Resolution,
	double Radius,
	const USolarOrbzTerrainLayerStack* TerrainStack,
	const FSolarOrbzClimateGrid* ClimateGridForMasking,
	FSolarOrbzIcoSphereMeshData& OutMeshData)
{
	OutMeshData.Reset();

	Resolution = FMath::Max(Resolution, 1);
	Radius = FMath::Max(Radius, (double)KINDA_SMALL_NUMBER);

	FVector CornerA, CornerB, CornerC;
	Address.GetCornerUnitDirections(CornerA, CornerB, CornerC);

	// Triangular grid: row i (0..Resolution) has (i+1) points, point j (0..i) within that row -
	// barycentric weights (WeightA, WeightB, WeightC) always sum to 1, and degenerate cleanly to
	// A at (i=0, j=0) and to the B/C edge at i=Resolution without any division-by-zero special case
	// (unlike a naive nested-lerp-by-row-length formulation would need at row 0).
	const int32 NumVerts = (Resolution + 1) * (Resolution + 2) / 2;
	OutMeshData.Vertices.SetNum(NumVerts);
	OutMeshData.Normals.SetNum(NumVerts);
	OutMeshData.Tangents.SetNum(NumVerts);
	OutMeshData.UVs.SetNum(NumVerts);

	auto PointIndex = [](int32 I, int32 J) { return I * (I + 1) / 2 + J; };

	// Same equirectangular UV convention FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize uses -
	// USolarOrbzHeightmapTerrainLayer/USolarOrbzStampTerrainLayer sample via this UV, so a chunk has to
	// agree with the whole-sphere mesh's convention for a shared TerrainStack to look the same on both.
	// No antimeridian handling here yet - see the design doc's "known out of scope" list.
	auto ComputeEquirectangularUV = [](const FVector& UnitDir) -> FVector2D
	{
		const double Azimuth = FMath::Atan2((double)UnitDir.Y, (double)UnitDir.X);
		const double U = 0.5 + Azimuth / (2.0 * FSolarOrbzLatLongGrid::PI_D);
		const double Polar = FMath::Acos(FMath::Clamp((double)UnitDir.Z, -1.0, 1.0));
		const double V = Polar / FSolarOrbzLatLongGrid::PI_D;
		return FVector2D((float)U, (float)V);
	};

	for (int32 I = 0; I <= Resolution; ++I)
	{
		for (int32 J = 0; J <= I; ++J)
		{
			const double WeightA = (double)(Resolution - I) / (double)Resolution;
			const double WeightB = (double)(I - J) / (double)Resolution;
			const double WeightC = (double)J / (double)Resolution;

			const FVector UnitDirection = (CornerA * WeightA + CornerB * WeightB + CornerC * WeightC).GetSafeNormal();
			const FVector2D UV = ComputeEquirectangularUV(UnitDirection);

			float Height = 0.0f;
			if (TerrainStack)
			{
				Height = TerrainStack->EvaluateHeight(UnitDirection, UV, ClimateGridForMasking);
			}

			const int32 VertIdx = PointIndex(I, J);
			OutMeshData.Vertices[VertIdx] = UnitDirection * Radius + UnitDirection * Height;
			OutMeshData.Normals[VertIdx] = UnitDirection; // pristine sphere normal - corrected below once triangles exist
			OutMeshData.UVs[VertIdx] = UV;

			// Same "longitude tangent, degenerate at poles falls back to a fixed axis" approach as
			// FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize's ComputeTangent.
			FVector Tangent = FVector::CrossProduct(FVector::UpVector, UnitDirection);
			if (!Tangent.Normalize())
			{
				Tangent = FVector::ForwardVector;
			}
			OutMeshData.Tangents[VertIdx] = Tangent;
		}
	}

	// Standard triangular-lattice triangulation: between row I and row I+1, there are (I+1) "upward"
	// triangles (apex in row I) and I "downward" triangles (apex in row I+1) - 2I+1 total, summing to
	// Resolution^2 across every row pair, matching a triangle subdivided Resolution times per edge.
	OutMeshData.Triangles.Reserve(Resolution * Resolution * 3);
	for (int32 I = 0; I < Resolution; ++I)
	{
		for (int32 J = 0; J <= I; ++J)
		{
			const int32 Up0 = PointIndex(I, J);
			const int32 Up1 = PointIndex(I + 1, J);
			const int32 Up2 = PointIndex(I + 1, J + 1);

			// Winding UNVERIFIED without a renderer - see this function's header comment.
			OutMeshData.Triangles.Add(Up0);
			OutMeshData.Triangles.Add(Up2);
			OutMeshData.Triangles.Add(Up1);

			if (J < I)
			{
				const int32 Down0 = PointIndex(I, J);
				const int32 Down1 = PointIndex(I + 1, J + 1);
				const int32 Down2 = PointIndex(I, J + 1);

				OutMeshData.Triangles.Add(Down0);
				OutMeshData.Triangles.Add(Down2);
				OutMeshData.Triangles.Add(Down1);
			}
		}
	}

	// Recomputes real (area-weighted) normals from the now-displaced vertex positions, same as
	// RunTerrainPassA does for the whole-sphere mesh.
	FSolarOrbzIcoSphereGenerator::RecomputeSmoothNormals(OutMeshData);

	UE_LOG(LogSolarOrbzChunk, Log,
		TEXT("SolarOrbz Chunk: generated BaseFace=%d Depth=%d PathBits=%llu, %d verts, %d tris"),
		Address.BaseFaceIndex, Address.Depth, Address.PathBits, NumVerts, OutMeshData.Triangles.Num() / 3);
}
