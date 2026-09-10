// SolarOrbz - Parametric IcoSphere Generator (implementation)

#include "SolarOrbzIcoSphereGenerator.h"

namespace SolarOrbzIcoSphere
{
	// Using an explicit double constant rather than the engine's PI macro to
	// avoid precision loss now that FVector components are double (LWC) by default.
	static constexpr double PI_D = 3.14159265358979323846;

	// Edge length of a regular icosahedron with circumradius 1.0.
	static constexpr double UnitCircumradiusEdgeLength = 1.0514622242382672;
}

int64 FSolarOrbzIcoSphereGenerator::EstimateVertexCount(int32 SubdivisionLevel)
{
	// Base icosahedron: 12 verts, 20 tris, 30 edges.
	// Each subdivision: V' = V + E, E' = 2E + 3F, F' = 4F  (Euler-consistent for a closed sphere mesh)
	int64 V = 12, E = 30, F = 20;
	for (int32 i = 0; i < SubdivisionLevel; ++i)
	{
		const int64 NewV = V + E;
		const int64 NewE = 2 * E + 3 * F;
		const int64 NewF = 4 * F;
		V = NewV; E = NewE; F = NewF;
	}
	// Actual output vertex count will be a bit higher due to UV seam/pole duplication.
	return V;
}

void FSolarOrbzIcoSphereGenerator::RecomputeSmoothNormals(FSolarOrbzIcoSphereMeshData& MeshData)
{
	TArray<FVector> AccumNormals;
	AccumNormals.SetNumZeroed(MeshData.Vertices.Num());

	for (int32 TriStart = 0; TriStart < MeshData.Triangles.Num(); TriStart += 3)
	{
		const int32 IA = MeshData.Triangles[TriStart];
		const int32 IB = MeshData.Triangles[TriStart + 1];
		const int32 IC = MeshData.Triangles[TriStart + 2];

		const FVector& A = MeshData.Vertices[IA];
		const FVector& B = MeshData.Vertices[IB];
		const FVector& C = MeshData.Vertices[IC];

		// Un-normalized cross product's magnitude is proportional to triangle area, which
		// naturally area-weights the contribution of each face to its corners' normals.
		const FVector FaceNormal = FVector::CrossProduct(B - A, C - A);

		AccumNormals[IA] += FaceNormal;
		AccumNormals[IB] += FaceNormal;
		AccumNormals[IC] += FaceNormal;
	}

	for (int32 i = 0; i < MeshData.Normals.Num(); ++i)
	{
		const FVector Normalized = AccumNormals[i].GetSafeNormal();
		if (!Normalized.IsNearlyZero())
		{
			MeshData.Normals[i] = Normalized;
		}
	}
}

int32 FSolarOrbzIcoSphereGenerator::ComputeSubdivisionLevelForEdgeLength(float Radius, float TargetEdgeLength, int32 MaxSubdivisions)
{
	using namespace SolarOrbzIcoSphere;

	const double BaseEdgeLength = FMath::Max(Radius, KINDA_SMALL_NUMBER) * UnitCircumradiusEdgeLength;

	if (TargetEdgeLength <= KINDA_SMALL_NUMBER)
	{
		return MaxSubdivisions;
	}

	const double Ratio = BaseEdgeLength / (double)TargetEdgeLength;
	const int32 Level = Ratio > 1.0 ? (int32)FMath::CeilToDouble(FMath::Log2(Ratio)) : 0;

	return FMath::Clamp(Level, 0, MaxSubdivisions);
}

int32 FSolarOrbzIcoSphereGenerator::Generate(float Radius, float VerticesPerMeter, FSolarOrbzIcoSphereMeshData& OutMeshData, int32 MaxSubdivisions)
{
	// UE units are centimeters, so "per meter" density -> divide 100 by it to get target edge length in cm.
	const float TargetEdgeLength = VerticesPerMeter > KINDA_SMALL_NUMBER ? (100.0f / VerticesPerMeter) : Radius;

	const int32 Level = ComputeSubdivisionLevelForEdgeLength(Radius, TargetEdgeLength, MaxSubdivisions);
	GenerateAtSubdivisionLevel(Radius, Level, OutMeshData);
	return Level;
}

void FSolarOrbzIcoSphereGenerator::GenerateAtSubdivisionLevel(float Radius, int32 SubdivisionLevel, FSolarOrbzIcoSphereMeshData& OutMeshData)
{
	FBuildContext Context;
	BuildBaseIcosahedron(Context);

	SubdivisionLevel = FMath::Max(SubdivisionLevel, 0);
	for (int32 i = 0; i < SubdivisionLevel; ++i)
	{
		SubdivideOnce(Context);
	}

	FixUVSeamsAndFinalize(Context, FMath::Max(Radius, KINDA_SMALL_NUMBER), OutMeshData);
}

void FSolarOrbzIcoSphereGenerator::BuildBaseIcosahedron(FBuildContext& Context)
{
	Context.Positions.Reset(12);
	Context.Indices.Reset(60);
	Context.MidpointCache.Reset();

	const double T = (1.0 + FMath::Sqrt(5.0)) / 2.0;

	// 12 vertices of a regular icosahedron, normalized onto the unit sphere.
	const FVector RawVerts[12] =
	{
		FVector(-1,  T,  0), FVector( 1,  T,  0), FVector(-1, -T,  0), FVector( 1, -T,  0),
		FVector( 0, -1,  T), FVector( 0,  1,  T), FVector( 0, -1, -T), FVector( 0,  1, -T),
		FVector( T,  0, -1), FVector( T,  0,  1), FVector(-T,  0, -1), FVector(-T,  0,  1),
	};

	for (const FVector& V : RawVerts)
	{
		Context.Positions.Add(V.GetSafeNormal());
	}

	// 20 triangular faces, wound so normals point outward (CCW when viewed from outside).
	const int32 RawFaces[20][3] =
	{
		{0, 11, 5}, {0, 5, 1},  {0, 1, 7},  {0, 7, 10}, {0, 10, 11},
		{1, 5, 9},  {5, 11, 4}, {11, 10, 2},{10, 7, 6}, {7, 1, 8},
		{3, 9, 4},  {3, 4, 2},  {3, 2, 6},  {3, 6, 8},  {3, 8, 9},
		{4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},  {9, 8, 1},
	};

	for (const int32 (&Face)[3] : RawFaces)
	{
		Context.Indices.Add(Face[0]);
		Context.Indices.Add(Face[1]);
		Context.Indices.Add(Face[2]);
	}
}

int32 FSolarOrbzIcoSphereGenerator::GetOrCreateMidpoint(FBuildContext& Context, int32 IndexA, int32 IndexB)
{
	const uint64 Key = ((uint64)FMath::Min(IndexA, IndexB) << 32) | (uint32)FMath::Max(IndexA, IndexB);

	if (const int32* Existing = Context.MidpointCache.Find(Key))
	{
		return *Existing;
	}

	const FVector Mid = ((Context.Positions[IndexA] + Context.Positions[IndexB]) * 0.5).GetSafeNormal();
	const int32 NewIndex = Context.Positions.Add(Mid);
	Context.MidpointCache.Add(Key, NewIndex);
	return NewIndex;
}

void FSolarOrbzIcoSphereGenerator::SubdivideOnce(FBuildContext& Context)
{
	TArray<uint32> NewIndices;
	NewIndices.Reserve(Context.Indices.Num() * 4);

	// Fresh per level: midpoints are only reused *within* a single subdivision pass.
	Context.MidpointCache.Reset();

	for (int32 i = 0; i < Context.Indices.Num(); i += 3)
	{
		const int32 A = Context.Indices[i];
		const int32 B = Context.Indices[i + 1];
		const int32 C = Context.Indices[i + 2];

		const int32 AB = GetOrCreateMidpoint(Context, A, B);
		const int32 BC = GetOrCreateMidpoint(Context, B, C);
		const int32 CA = GetOrCreateMidpoint(Context, C, A);

		NewIndices.Append({ (uint32)A, (uint32)AB, (uint32)CA });
		NewIndices.Append({ (uint32)B, (uint32)BC, (uint32)AB });
		NewIndices.Append({ (uint32)C, (uint32)CA, (uint32)BC });
		NewIndices.Append({ (uint32)AB, (uint32)BC, (uint32)CA });
	}

	Context.Indices = MoveTemp(NewIndices);
}

void FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize(const FBuildContext& Context, float Radius, FSolarOrbzIcoSphereMeshData& OutMeshData)
{
	using namespace SolarOrbzIcoSphere;

	OutMeshData.Reset();

	const int32 SourceVertCount = Context.Positions.Num();
	OutMeshData.Vertices.Reserve(SourceVertCount);
	OutMeshData.Normals.Reserve(SourceVertCount);
	OutMeshData.Tangents.Reserve(SourceVertCount);
	OutMeshData.UVs.Reserve(SourceVertCount);
	OutMeshData.Triangles.Reserve(Context.Indices.Num());

	// --- Pass 1: emit one vertex per unique geometric position, with a baseline UV. ---
	// Longitude wraps around Z (up in UE); latitude runs from +Z (north pole, V=0) to -Z (south pole, V=1).
	auto ComputeUV = [](const FVector& UnitPos) -> FVector2D
	{
		const double Azimuth = FMath::Atan2((double)UnitPos.Y, (double)UnitPos.X); // -PI .. PI
		const double U = 0.5 + Azimuth / (2.0 * PI_D);
		const double Polar = FMath::Acos(FMath::Clamp((double)UnitPos.Z, -1.0, 1.0)); // 0 .. PI
		const double V = Polar / PI_D;
		return FVector2D((float)U, (float)V);
	};

	auto ComputeTangent = [](const FVector& UnitPos, const FVector& Normal) -> FVector
	{
		// Tangent follows the line of latitude (increasing longitude). Degenerates at the poles,
		// where we just fall back to a fixed axis - poles are single points, tangent is irrelevant there.
		FVector Tangent = FVector::CrossProduct(FVector::UpVector, Normal);
		if (!Tangent.Normalize())
		{
			Tangent = FVector::ForwardVector;
		}
		return Tangent;
	};

	OutMeshData.Vertices.SetNum(SourceVertCount);
	OutMeshData.Normals.SetNum(SourceVertCount);
	OutMeshData.Tangents.SetNum(SourceVertCount);
	OutMeshData.UVs.SetNum(SourceVertCount);

	for (int32 i = 0; i < SourceVertCount; ++i)
	{
		const FVector& UnitPos = Context.Positions[i];
		OutMeshData.Vertices[i] = UnitPos * Radius;
		OutMeshData.Normals[i] = UnitPos;
		OutMeshData.Tangents[i] = ComputeTangent(UnitPos, UnitPos);
		OutMeshData.UVs[i] = ComputeUV(UnitPos);
	}

	OutMeshData.Triangles.SetNum(Context.Indices.Num());
	for (int32 TriStart = 0; TriStart < Context.Indices.Num(); TriStart += 3)
	{
		// Swap the last two corners of every triangle: our index generation uses a
		// mathematically "natural" CCW winding, but it's the opposite handedness from
		// what UE treats as front-facing, so left as-is the sphere renders inside-out.
		OutMeshData.Triangles[TriStart + 0] = (int32)Context.Indices[TriStart + 0];
		OutMeshData.Triangles[TriStart + 1] = (int32)Context.Indices[TriStart + 2];
		OutMeshData.Triangles[TriStart + 2] = (int32)Context.Indices[TriStart + 1];
	}

	// --- Pass 2: fix the U seam (where longitude wraps 0 -> 1) and the two poles. ---
	// A triangle straddles the seam when its U values span more than half the UV range;
	// duplicate the "low" side vertices per-triangle so the interpolation goes the short way around.
	// A triangle touches a pole when one vertex sits exactly at Z = +-1; duplicate that pole vertex
	// per-triangle with U averaged from its two neighbors, since a pole has no single "correct" U.
	TMap<int32, int32> SeamDuplicateCache; // original index -> duplicate index with U += 1.0
	auto GetSeamDuplicate = [&](int32 OriginalIndex) -> int32
	{
		if (const int32* Existing = SeamDuplicateCache.Find(OriginalIndex))
		{
			return *Existing;
		}

		// Copy out by value first - Add() can reallocate the array, which would
		// invalidate a reference taken from that same array before the copy happens.
		const FVector PosCopy = OutMeshData.Vertices[OriginalIndex];
		const FVector NormalCopy = OutMeshData.Normals[OriginalIndex];
		const FVector TangentCopy = OutMeshData.Tangents[OriginalIndex];
		FVector2D UVCopy = OutMeshData.UVs[OriginalIndex];
		UVCopy.X += 1.0f;

		const int32 NewIndex = OutMeshData.Vertices.Add(PosCopy);
		OutMeshData.Normals.Add(NormalCopy);
		OutMeshData.Tangents.Add(TangentCopy);
		OutMeshData.UVs.Add(UVCopy);
		SeamDuplicateCache.Add(OriginalIndex, NewIndex);
		return NewIndex;
	};

	const double PoleDotThreshold = 0.9999; // |Z| this close to 1 counts as "at the pole"

	for (int32 TriStart = 0; TriStart < OutMeshData.Triangles.Num(); TriStart += 3)
	{
		int32 TriIdx[3] = { OutMeshData.Triangles[TriStart], OutMeshData.Triangles[TriStart + 1], OutMeshData.Triangles[TriStart + 2] };
		float U[3] = { OutMeshData.UVs[TriIdx[0]].X, OutMeshData.UVs[TriIdx[1]].X, OutMeshData.UVs[TriIdx[2]].X };

		bool bIsPole[3];
		for (int32 k = 0; k < 3; ++k)
		{
			bIsPole[k] = FMath::Abs(OutMeshData.Normals[TriIdx[k]].Z) > PoleDotThreshold;
		}

		// Pole handling first: a pole vertex's U is meaningless on its own, so derive it from
		// the other two (non-pole) corners and give this triangle its own private copy.
		bool bHasPole = bIsPole[0] || bIsPole[1] || bIsPole[2];
		if (bHasPole)
		{
			for (int32 k = 0; k < 3; ++k)
			{
				if (bIsPole[k])
				{
					const int32 OtherA = TriIdx[(k + 1) % 3];
					const int32 OtherB = TriIdx[(k + 2) % 3];
					float AvgU = (OutMeshData.UVs[OtherA].X + OutMeshData.UVs[OtherB].X) * 0.5f;

					// If the two non-pole corners themselves straddle the seam, fix that first
					// so we're averaging the correct (unwrapped) values.
					if (FMath::Abs(OutMeshData.UVs[OtherA].X - OutMeshData.UVs[OtherB].X) > 0.5f)
					{
						AvgU += 0.5f;
					}

					// Copy out by value first, same reason as GetSeamDuplicate above.
					const FVector PosCopy = OutMeshData.Vertices[TriIdx[k]];
					const FVector NormalCopy = OutMeshData.Normals[TriIdx[k]];
					const FVector TangentCopy = OutMeshData.Tangents[TriIdx[k]];
					const float OriginalV = OutMeshData.UVs[TriIdx[k]].Y;

					const int32 DupIndex = OutMeshData.Vertices.Add(PosCopy);
					OutMeshData.Normals.Add(NormalCopy);
					OutMeshData.Tangents.Add(TangentCopy);
					OutMeshData.UVs.Add(FVector2D(AvgU, OriginalV));

					OutMeshData.Triangles[TriStart + k] = DupIndex;
					TriIdx[k] = DupIndex;
					U[k] = AvgU;
				}
			}
		}

		// Seam handling: if this triangle's U values span more than half the UV space,
		// it's wrapping around the back - push the "low" corners into the [1,2] range instead.
		const float MinU = FMath::Min3(U[0], U[1], U[2]);
		const float MaxU = FMath::Max3(U[0], U[1], U[2]);
		if (MaxU - MinU > 0.5f)
		{
			for (int32 k = 0; k < 3; ++k)
			{
				if (!bIsPole[k] && U[k] < 0.5f)
				{
					OutMeshData.Triangles[TriStart + k] = GetSeamDuplicate(TriIdx[k]);
				}
			}
		}
	}
}
