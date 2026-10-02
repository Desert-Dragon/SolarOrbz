// SolarOrbzCubeSphereChunk implementation. See the header and Docs/ChunkedPlanetTerrain.md for the
// overall design and what's deliberately not built yet.

#include "SolarOrbzCubeSphereChunk.h"
#include "SolarOrbzTerrainLayers.h"
#include "SolarOrbzLatLongGrid.h"

DEFINE_LOG_CATEGORY_STATIC(LogSolarOrbzChunk, Log, All);

void FSolarOrbzChunkAddress::GetFaceLocalBounds(double& OutMinS, double& OutMaxS, double& OutMinT, double& OutMaxT) const
{
	const int32 N = 1 << FMath::Max(Depth, 0); // chunks per axis at this depth
	const double Width = 2.0 / (double)N;      // face-local (S,T) spans [-1, 1], total width 2

	OutMinS = -1.0 + (double)X * Width;
	OutMaxS = OutMinS + Width;
	OutMinT = -1.0 + (double)Y * Width;
	OutMaxT = OutMinT + Width;
}

FSolarOrbzChunkAddress FSolarOrbzChunkAddress::GetParent() const
{
	if (Depth <= 0)
	{
		// Documented behavior (see header) - Depth 0 is the whole face, nothing above it.
		return *this;
	}

	return FSolarOrbzChunkAddress(Face, Depth - 1, X / 2, Y / 2);
}

void FSolarOrbzChunkAddress::GetChildren(FSolarOrbzChunkAddress OutChildren[4]) const
{
	const int32 ChildDepth = Depth + 1;
	const int32 BaseX = X * 2;
	const int32 BaseY = Y * 2;

	OutChildren[0] = FSolarOrbzChunkAddress(Face, ChildDepth, BaseX,     BaseY);
	OutChildren[1] = FSolarOrbzChunkAddress(Face, ChildDepth, BaseX + 1, BaseY);
	OutChildren[2] = FSolarOrbzChunkAddress(Face, ChildDepth, BaseX,     BaseY + 1);
	OutChildren[3] = FSolarOrbzChunkAddress(Face, ChildDepth, BaseX + 1, BaseY + 1);
}

void FSolarOrbzCubeSphereChunkGenerator::GetFaceBasis(ESolarOrbzCubeFace Face, FVector& OutRight, FVector& OutUp, FVector& OutForward)
{
	// Each face's Forward is simply that face's outward cube normal. Right/Up chosen per face so
	// Right x Up == Forward exactly (verified by hand for all six - see the header's own comment on
	// what this does and doesn't prove: each face is individually a consistent right-handed basis,
	// but full edge-to-edge continuity between adjacent faces still needs checking against a real
	// rendered chunk, same as every other orientation-sensitive bit of math in this plugin).
	switch (Face)
	{
	case ESolarOrbzCubeFace::PlusX:
		OutRight = FVector(0, 1, 0); OutUp = FVector(0, 0, 1); OutForward = FVector(1, 0, 0);
		break;
	case ESolarOrbzCubeFace::MinusX:
		OutRight = FVector(0, -1, 0); OutUp = FVector(0, 0, 1); OutForward = FVector(-1, 0, 0);
		break;
	case ESolarOrbzCubeFace::PlusY:
		OutRight = FVector(-1, 0, 0); OutUp = FVector(0, 0, 1); OutForward = FVector(0, 1, 0);
		break;
	case ESolarOrbzCubeFace::MinusY:
		OutRight = FVector(1, 0, 0); OutUp = FVector(0, 0, 1); OutForward = FVector(0, -1, 0);
		break;
	case ESolarOrbzCubeFace::PlusZ:
		OutRight = FVector(1, 0, 0); OutUp = FVector(0, 1, 0); OutForward = FVector(0, 0, 1);
		break;
	case ESolarOrbzCubeFace::MinusZ:
		OutRight = FVector(1, 0, 0); OutUp = FVector(0, -1, 0); OutForward = FVector(0, 0, -1);
		break;
	default:
		OutRight = FVector(1, 0, 0); OutUp = FVector(0, 1, 0); OutForward = FVector(0, 0, 1);
		break;
	}
}

FVector FSolarOrbzCubeSphereChunkGenerator::FaceLocalToUnitSphereDirection(ESolarOrbzCubeFace Face, double S, double T)
{
	FVector Right, Up, Forward;
	GetFaceBasis(Face, Right, Up, Forward);

	// Plain normalize of the cube point - NOT area-preserving, see the header's own comment on the
	// COBE quad-sphere warp this could use instead later.
	const FVector CubePoint = Right * (float)S + Up * (float)T + Forward;
	return CubePoint.GetSafeNormal();
}

void FSolarOrbzCubeSphereChunkGenerator::GenerateChunk(
	const FSolarOrbzChunkAddress& Address,
	int32 Resolution,
	double Radius,
	const USolarOrbzTerrainLayerStack* TerrainStack,
	const FSolarOrbzClimateGrid* ClimateGridForMasking,
	FSolarOrbzIcoSphereMeshData& OutMeshData)
{
	OutMeshData.Reset();

	Resolution = FMath::Max(Resolution, 1);
	const int32 VertsPerEdge = Resolution + 1;
	const int32 NumVerts = VertsPerEdge * VertsPerEdge;
	Radius = FMath::Max(Radius, (double)KINDA_SMALL_NUMBER);

	double MinS, MaxS, MinT, MaxT;
	Address.GetFaceLocalBounds(MinS, MaxS, MinT, MaxT);

	OutMeshData.Vertices.SetNum(NumVerts);
	OutMeshData.Normals.SetNum(NumVerts);
	OutMeshData.Tangents.SetNum(NumVerts);
	OutMeshData.UVs.SetNum(NumVerts);

	auto VertexIndex = [VertsPerEdge](int32 Ix, int32 Iy) { return Iy * VertsPerEdge + Ix; };

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

	for (int32 Iy = 0; Iy < VertsPerEdge; ++Iy)
	{
		const double T = FMath::Lerp(MinT, MaxT, (double)Iy / (double)Resolution);
		for (int32 Ix = 0; Ix < VertsPerEdge; ++Ix)
		{
			const double S = FMath::Lerp(MinS, MaxS, (double)Ix / (double)Resolution);
			const int32 VertIdx = VertexIndex(Ix, Iy);

			const FVector UnitDirection = FaceLocalToUnitSphereDirection(Address.Face, S, T);
			const FVector2D UV = ComputeEquirectangularUV(UnitDirection);

			float Height = 0.0f;
			if (TerrainStack)
			{
				Height = TerrainStack->EvaluateHeight(UnitDirection, UV, ClimateGridForMasking);
			}

			OutMeshData.Vertices[VertIdx] = UnitDirection * Radius + UnitDirection * Height;
			OutMeshData.Normals[VertIdx] = UnitDirection; // pristine sphere normal - corrected below once triangles exist
			OutMeshData.UVs[VertIdx] = UV;

			// Same "longitude tangent, degenerate at poles falls back to a fixed axis" approach as
			// FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize's ComputeTangent - a chunk's own
			// grid axes aren't used for tangent space here, kept consistent with the whole-sphere
			// mesh instead so a shared material behaves the same on both.
			FVector Tangent = FVector::CrossProduct(FVector::UpVector, UnitDirection);
			if (!Tangent.Normalize())
			{
				Tangent = FVector::ForwardVector;
			}
			OutMeshData.Tangents[VertIdx] = Tangent;
		}
	}

	// Grid triangulation, 2 triangles per cell. Winding chosen to match
	// FSolarOrbzIcoSphereGenerator::FixUVSeamsAndFinalize's own empirically-determined convention
	// (see its comment on swapping the last two corners) - UNVERIFIED here without a renderer; check
	// a generated chunk's front/back facing before relying on this.
	OutMeshData.Triangles.Reserve(Resolution * Resolution * 6);
	for (int32 Iy = 0; Iy < Resolution; ++Iy)
	{
		for (int32 Ix = 0; Ix < Resolution; ++Ix)
		{
			const int32 V00 = VertexIndex(Ix, Iy);
			const int32 V10 = VertexIndex(Ix + 1, Iy);
			const int32 V01 = VertexIndex(Ix, Iy + 1);
			const int32 V11 = VertexIndex(Ix + 1, Iy + 1);

			OutMeshData.Triangles.Add(V00); OutMeshData.Triangles.Add(V01); OutMeshData.Triangles.Add(V11);
			OutMeshData.Triangles.Add(V00); OutMeshData.Triangles.Add(V11); OutMeshData.Triangles.Add(V10);
		}
	}

	// Recomputes real (area-weighted) normals from the now-displaced vertex positions, same as
	// RunTerrainPassA does for the whole-sphere mesh - the pristine sphere normals written above are
	// only a placeholder until triangles exist to derive real ones from.
	FSolarOrbzIcoSphereGenerator::RecomputeSmoothNormals(OutMeshData);

	UE_LOG(LogSolarOrbzChunk, Log,
		TEXT("SolarOrbz Chunk: generated Face=%d Depth=%d (%d,%d), %d verts, %d tris"),
		(int32)Address.Face, Address.Depth, Address.X, Address.Y, NumVerts, OutMeshData.Triangles.Num() / 3);
}
