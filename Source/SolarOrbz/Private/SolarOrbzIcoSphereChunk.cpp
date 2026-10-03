// SolarOrbzIcoSphereChunk implementation. See the header and Docs/ChunkedPlanetTerrain.md for the
// overall design and what's deliberately not built yet.

#include "SolarOrbzIcoSphereChunk.h"
#include "SolarOrbzTerrainLayers.h"
#include "SolarOrbzLatLongGrid.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzChunk, Log, All);

// ================================================================================================
// GetEdgeNeighbor's lookup tables - see that function's own header comment for how these were
// derived and verified (exhaustively, against real geodesic positions, outside the engine). Kept in
// this anonymous namespace since nothing outside GetEdgeNeighbor itself should need them directly.
// ================================================================================================
namespace
{
	// For child index 0-3 and one of its edges: true if that edge is INTERNAL (shared with a
	// specific sibling within the same 4-way split, not the parent) - OutSiblingChild/OutSiblingEdge
	// name that sibling and which of ITS OWN edges is the same physical edge (always the OPPOSITE
	// 2-point order - i.e. "direction-reversed" relative to this child's naming of it, same as every
	// other internal transition verified; GetEdgeNeighbor's caller flips its accumulated "which end"
	// flags on every internal match for exactly this reason, not just on a cross-base-face jump).
	bool TryGetInternalNeighbor(int32 ChildIndex, ESolarOrbzChunkEdge Edge, int32& OutSiblingChild, ESolarOrbzChunkEdge& OutSiblingEdge)
	{
		switch (ChildIndex)
		{
		case 0:
			if (Edge == ESolarOrbzChunkEdge::BC) { OutSiblingChild = 3; OutSiblingEdge = ESolarOrbzChunkEdge::CA; return true; }
			break;
		case 1:
			if (Edge == ESolarOrbzChunkEdge::BC) { OutSiblingChild = 3; OutSiblingEdge = ESolarOrbzChunkEdge::AB; return true; }
			break;
		case 2:
			if (Edge == ESolarOrbzChunkEdge::BC) { OutSiblingChild = 3; OutSiblingEdge = ESolarOrbzChunkEdge::BC; return true; }
			break;
		case 3:
			if (Edge == ESolarOrbzChunkEdge::AB) { OutSiblingChild = 1; OutSiblingEdge = ESolarOrbzChunkEdge::BC; return true; }
			if (Edge == ESolarOrbzChunkEdge::BC) { OutSiblingChild = 2; OutSiblingEdge = ESolarOrbzChunkEdge::BC; return true; }
			if (Edge == ESolarOrbzChunkEdge::CA) { OutSiblingChild = 0; OutSiblingEdge = ESolarOrbzChunkEdge::BC; return true; }
			break;
		default:
			break;
		}
		return false;
	}

	// For a corner child (0,1,2 - a center child's edges are always internal, never boundary) and
	// one of its two boundary edges: which of the PARENT's 3 edges it's half of, and whether it's the
	// half nearer the parent edge's FIRST or SECOND named endpoint (AB=(A,B), BC=(B,C), CA=(C,A), in
	// that order - "first"/"second" refers to position in that pair).
	bool TryGetBoundaryParentEdge(int32 ChildIndex, ESolarOrbzChunkEdge Edge, ESolarOrbzChunkEdge& OutParentEdge, bool& bOutNearFirst)
	{
		switch (ChildIndex)
		{
		case 0:
			if (Edge == ESolarOrbzChunkEdge::AB) { OutParentEdge = ESolarOrbzChunkEdge::AB; bOutNearFirst = true; return true; }
			if (Edge == ESolarOrbzChunkEdge::CA) { OutParentEdge = ESolarOrbzChunkEdge::CA; bOutNearFirst = false; return true; }
			break;
		case 1:
			if (Edge == ESolarOrbzChunkEdge::AB) { OutParentEdge = ESolarOrbzChunkEdge::BC; bOutNearFirst = true; return true; }
			if (Edge == ESolarOrbzChunkEdge::CA) { OutParentEdge = ESolarOrbzChunkEdge::AB; bOutNearFirst = false; return true; }
			break;
		case 2:
			if (Edge == ESolarOrbzChunkEdge::AB) { OutParentEdge = ESolarOrbzChunkEdge::CA; bOutNearFirst = true; return true; }
			if (Edge == ESolarOrbzChunkEdge::CA) { OutParentEdge = ESolarOrbzChunkEdge::BC; bOutNearFirst = false; return true; }
			break;
		default:
			break;
		}
		return false;
	}

	// Inverse of TryGetBoundaryParentEdge: given a parent edge + which end, which child touches it and
	// which of THAT CHILD's own edges is the continuation (needed if descent continues another level).
	void GetDescendChild(ESolarOrbzChunkEdge ParentEdge, bool bNearFirst, int32& OutChild, ESolarOrbzChunkEdge& OutChildEdge)
	{
		switch (ParentEdge)
		{
		case ESolarOrbzChunkEdge::AB:
			if (bNearFirst) { OutChild = 0; OutChildEdge = ESolarOrbzChunkEdge::AB; }
			else { OutChild = 1; OutChildEdge = ESolarOrbzChunkEdge::CA; }
			break;
		case ESolarOrbzChunkEdge::BC:
			if (bNearFirst) { OutChild = 1; OutChildEdge = ESolarOrbzChunkEdge::AB; }
			else { OutChild = 2; OutChildEdge = ESolarOrbzChunkEdge::CA; }
			break;
		default: // CA
			if (bNearFirst) { OutChild = 2; OutChildEdge = ESolarOrbzChunkEdge::AB; }
			else { OutChild = 0; OutChildEdge = ESolarOrbzChunkEdge::CA; }
			break;
		}
	}

	struct FSolarOrbzFaceEdgeCrossing
	{
		int32 OtherFace = INDEX_NONE;
		int32 OtherEdgeIndex = INDEX_NONE;
	};

	// Builds, once, the 20x3 table of "which other face/edge is across this face's edge E" from
	// GetBaseIcosahedron's own data - not a hardcoded magic table, so it can't drift from the vertex/
	// face data it's derived from. Every one of the 30 shared edges is traversed in OPPOSITE vertex
	// order by its two faces (checked here, not assumed) - an expected property of any consistently
	// outward-wound closed triangle mesh, which is exactly what BuildBaseIcosahedron's RawFaces is.
	const TArray<FSolarOrbzFaceEdgeCrossing>& GetFaceEdgeCrossingTable()
	{
		static const TArray<FSolarOrbzFaceEdgeCrossing> Table = []()
		{
			TArray<FVector> BaseVertices;
			TArray<FIntVector> BaseFaces;
			FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron(BaseVertices, BaseFaces);

			auto PairKey = [](int32 A, int32 B) -> uint64
			{
				const int32 Lo = FMath::Min(A, B), Hi = FMath::Max(A, B);
				return (((uint64)Lo) << 32) | (uint32)Hi;
			};

			// key -> list of (face, edge index, directed first vertex, directed second vertex)
			TMap<uint64, TArray<TTuple<int32, int32, int32, int32>>> EdgeOwners;
			for (int32 Face = 0; Face < BaseFaces.Num(); ++Face)
			{
				const FIntVector& F = BaseFaces[Face];
				const int32 Verts[3] = { F.X, F.Y, F.Z };
				for (int32 E = 0; E < 3; ++E)
				{
					const int32 Va = Verts[E];
					const int32 Vb = Verts[(E + 1) % 3];
					EdgeOwners.FindOrAdd(PairKey(Va, Vb)).Add(MakeTuple(Face, E, Va, Vb));
				}
			}

			TArray<FSolarOrbzFaceEdgeCrossing> Result;
			Result.SetNum(BaseFaces.Num() * 3);
			for (int32 Face = 0; Face < BaseFaces.Num(); ++Face)
			{
				const FIntVector& F = BaseFaces[Face];
				const int32 Verts[3] = { F.X, F.Y, F.Z };
				for (int32 E = 0; E < 3; ++E)
				{
					const int32 Va = Verts[E];
					const int32 Vb = Verts[(E + 1) % 3];
					const TArray<TTuple<int32, int32, int32, int32>>& Owners = EdgeOwners[PairKey(Va, Vb)];
					checkf(Owners.Num() == 2, TEXT("SolarOrbz Chunk: base icosahedron edge (%d,%d) touched by %d faces, expected exactly 2."), Va, Vb, Owners.Num());

					const TTuple<int32, int32, int32, int32>& OtherEntry = (Owners[0].Get<0>() == Face) ? Owners[1] : Owners[0];
					const int32 OtherVa = OtherEntry.Get<2>();
					const int32 OtherVb = OtherEntry.Get<3>();
					checkf(OtherVa == Vb && OtherVb == Va,
						TEXT("SolarOrbz Chunk: base icosahedron edge (%d,%d) shared by faces %d/%d in the SAME direction - expected always reversed for a consistently-wound closed mesh."),
						Va, Vb, Face, OtherEntry.Get<0>());

					Result[Face * 3 + E] = { OtherEntry.Get<0>(), OtherEntry.Get<1>() };
				}
			}

			return Result;
		}();

		return Table;
	}
}

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

bool FSolarOrbzChunkAddress::IsAnchoredAtOriginalVertex(int32& OutOriginalVertexIndex) const
{
	if (Depth <= 0)
	{
		// Documented behavior (see header) - the whole base face touches all 3 of its own original
		// corners at once, not one specific pinned point.
		return false;
	}

	const int32 Level0Child = (int32)(PathBits & 0x3);
	if (Level0Child == 3)
	{
		// Level 0 picked the center child - none of a center triangle's 3 corners are original
		// vertices (all three are midpoints), so nothing below this can ever be anchored either.
		return false;
	}

	// Every level after 0 must be exactly child 0 (the only choice that leaves "A" - wherever it's
	// currently pinned - unchanged, per GetCornerUnitDirections' switch) to keep this chunk anchored
	// at the same point level 0 locked onto, rather than sliding onto a midpoint introduced later.
	if ((PathBits >> 2) != 0)
	{
		return false;
	}

	TArray<FVector> BaseVertices;
	TArray<FIntVector> BaseFaces;
	FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron(BaseVertices, BaseFaces);

	const FIntVector& Face = BaseFaces[BaseFaceIndex];
	switch (Level0Child)
	{
	case 0: OutOriginalVertexIndex = Face.X; break;
	case 1: OutOriginalVertexIndex = Face.Y; break;
	default: OutOriginalVertexIndex = Face.Z; break; // Level0Child == 2
	}

	return true;
}

bool FSolarOrbzChunkAddress::GetPentagonVertexNeighbors(FSolarOrbzPentagonVertexNeighbors& OutNeighbors) const
{
	int32 OriginalVertexIndex = INDEX_NONE;
	if (!IsAnchoredAtOriginalVertex(OriginalVertexIndex))
	{
		return false;
	}

	TArray<FVector> BaseVertices;
	TArray<FIntVector> BaseFaces;
	FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron(BaseVertices, BaseFaces);

	OutNeighbors.OriginalVertexIndex = OriginalVertexIndex;
	OutNeighbors.OtherChunks.Reset();

	for (int32 OtherFaceIndex = 0; OtherFaceIndex < BaseFaces.Num(); ++OtherFaceIndex)
	{
		if (OtherFaceIndex == BaseFaceIndex)
		{
			continue; // this chunk's own face - not "other"
		}

		const FIntVector& OtherFace = BaseFaces[OtherFaceIndex];
		int32 CornerSlot = INDEX_NONE;
		if (OtherFace.X == OriginalVertexIndex) CornerSlot = 0;
		else if (OtherFace.Y == OriginalVertexIndex) CornerSlot = 1;
		else if (OtherFace.Z == OriginalVertexIndex) CornerSlot = 2;

		if (CornerSlot != INDEX_NONE)
		{
			// Same construction IsAnchoredAtOriginalVertex requires: level 0 = this face's own corner
			// slot for the shared vertex, every level after 0 implicitly 0 (PathBits has no higher
			// bits set at all here) - i.e. "anchor to this vertex, at the same Depth".
			OutNeighbors.OtherChunks.Add(FSolarOrbzChunkAddress(OtherFaceIndex, Depth, (uint64)CornerSlot));
		}
	}

	// Verified directly (see Docs/ChunkedPlanetTerrain.md's revision work) that every one of the 12
	// base vertices is touched by exactly 5 faces - if this ever fires, GetBaseIcosahedron's table
	// changed without this assumption being re-checked.
	if (OutNeighbors.OtherChunks.Num() != 4)
	{
		UE_LOG(LogSolarOrbzChunk, Warning,
			TEXT("SolarOrbz Chunk: GetPentagonVertexNeighbors found %d other chunks for original vertex %d, expected exactly 4 - GetBaseIcosahedron's face table may have changed."),
			OutNeighbors.OtherChunks.Num(), OriginalVertexIndex);
	}

	return true;
}

void FSolarOrbzChunkAddress::GetEdgeNeighbor(ESolarOrbzChunkEdge Edge, FSolarOrbzChunkAddress& OutNeighbor) const
{
	int32 WorkDepth = Depth;
	uint64 WorkPathBits = PathBits;
	ESolarOrbzChunkEdge CurEdge = Edge;
	int32 CurFace = BaseFaceIndex;

	// "Near first"/"near second" flags collected while ascending, outermost-last (i.e. in the order
	// we ascended) - applied in reverse (outermost-first) while descending back down. Flipped
	// wholesale on every internal-sibling match AND every cross-face jump, both of which were
	// verified to always reverse the edge's traversal direction (see this function's header comment).
	TArray<bool> AscendFlagsNearFirst;

	int32 RefDepth = INDEX_NONE;
	uint64 RefPathBits = 0;

	for (;;)
	{
		if (WorkDepth <= 0)
		{
			const FSolarOrbzFaceEdgeCrossing& Crossing = GetFaceEdgeCrossingTable()[CurFace * 3 + (int32)CurEdge];
			CurFace = Crossing.OtherFace;
			CurEdge = (ESolarOrbzChunkEdge)Crossing.OtherEdgeIndex;
			for (int32 i = 0; i < AscendFlagsNearFirst.Num(); ++i)
			{
				AscendFlagsNearFirst[i] = !AscendFlagsNearFirst[i];
			}
			RefDepth = 0;
			RefPathBits = 0;
			break;
		}

		const int32 LastChild = (int32)((WorkPathBits >> (2 * (WorkDepth - 1))) & 0x3);

		int32 SiblingChild;
		ESolarOrbzChunkEdge SiblingEdge;
		if (TryGetInternalNeighbor(LastChild, CurEdge, SiblingChild, SiblingEdge))
		{
			const uint64 ParentBits = (WorkDepth > 1) ? (WorkPathBits & ((uint64(1) << (2 * (WorkDepth - 1))) - 1)) : 0;
			RefDepth = WorkDepth;
			RefPathBits = ParentBits | (((uint64)SiblingChild) << (2 * (WorkDepth - 1)));
			CurEdge = SiblingEdge;
			for (int32 i = 0; i < AscendFlagsNearFirst.Num(); ++i)
			{
				AscendFlagsNearFirst[i] = !AscendFlagsNearFirst[i];
			}
			break;
		}

		ESolarOrbzChunkEdge ParentEdge;
		bool bNearFirst;
		if (!TryGetBoundaryParentEdge(LastChild, CurEdge, ParentEdge, bNearFirst))
		{
			// Unreachable: every (child, edge) combination is either internal or boundary.
			checkf(false, TEXT("SolarOrbz Chunk: GetEdgeNeighbor - child %d edge %d matched neither internal nor boundary."), LastChild, (int32)CurEdge);
			OutNeighbor = *this;
			return;
		}

		AscendFlagsNearFirst.Add(bNearFirst);
		WorkDepth -= 1;
		WorkPathBits = (WorkDepth > 0) ? (WorkPathBits & ((uint64(1) << (2 * WorkDepth)) - 1)) : 0;
		CurEdge = ParentEdge;
	}

	int32 ResultDepth = RefDepth;
	uint64 ResultPathBits = RefPathBits;
	for (int32 i = AscendFlagsNearFirst.Num() - 1; i >= 0; --i)
	{
		int32 ChildToDescend;
		ESolarOrbzChunkEdge ChildEdge;
		GetDescendChild(CurEdge, AscendFlagsNearFirst[i], ChildToDescend, ChildEdge);
		ResultPathBits |= ((uint64)ChildToDescend) << (2 * ResultDepth);
		ResultDepth += 1;
		CurEdge = ChildEdge;
	}

	checkf(ResultDepth == Depth, TEXT("SolarOrbz Chunk: GetEdgeNeighbor produced depth %d, expected %d."), ResultDepth, Depth);

	OutNeighbor = FSolarOrbzChunkAddress(CurFace, ResultDepth, ResultPathBits);
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
