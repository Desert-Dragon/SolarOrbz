// SolarOrbzChunkResidencyWalk implementation. See the header for the full algorithm write-up,
// MaxDepth-guard rationale, and verification methodology - this file intentionally keeps its own
// comments short and points back there rather than duplicating it.

#include "SolarOrbzChunkResidencyWalk.h"
#include "SolarOrbzIcoSphereChunk.h"
#include "SolarOrbzChunkLODPolicy.h"
#include "SolarOrbzIcoSphere.h"

void FSolarOrbzChunkResidencyWalker::GatherDesiredLeaves(
	double Radius,
	const FVector& ViewerWorldPosition,
	const FSolarOrbzChunkLODSettings& Settings,
	TArray<FSolarOrbzChunkAddress>& OutLeaves)
{
	OutLeaves.Reset();

	TArray<FVector> BaseVertices;
	TArray<FIntVector> BaseFaces;
	FSolarOrbzIcoSphereGenerator::GetBaseIcosahedron(BaseVertices, BaseFaces);

	for (int32 BaseFaceIndex = 0; BaseFaceIndex < BaseFaces.Num(); ++BaseFaceIndex)
	{
		VisitNode(FSolarOrbzChunkAddress(BaseFaceIndex, 0, 0), Radius, ViewerWorldPosition, Settings, OutLeaves);
	}
}

void FSolarOrbzChunkResidencyWalker::VisitNode(
	const FSolarOrbzChunkAddress& Node,
	double Radius,
	const FVector& ViewerWorldPosition,
	const FSolarOrbzChunkLODSettings& Settings,
	TArray<FSolarOrbzChunkAddress>& OutLeaves)
{
	FVector UnitA, UnitB, UnitC;
	Node.GetCornerUnitDirections(UnitA, UnitB, UnitC);

	// FSolarOrbzChunkLODPolicy takes already-world-scaled corner positions, not unit directions -
	// see that file's own header comment on "input shape". This is the one place that scaling step
	// happens, so every caller of this class gets it done consistently rather than each needing to
	// remember Radius itself.
	const FVector WorldA = UnitA * Radius;
	const FVector WorldB = UnitB * Radius;
	const FVector WorldC = UnitC * Radius;

	// Explicit, redundant-with-ShouldSplit's-own-check depth guard - the WALKER owns this bound, not
	// just the policy function. See this file's header comment for why both checks exist.
	const bool bWantsSplit = (Node.Depth < FSolarOrbzChunkAddress::MaxDepth)
		&& FSolarOrbzChunkLODPolicy::ShouldSplit(WorldA, WorldB, WorldC, ViewerWorldPosition, Settings, Node.Depth);

	if (!bWantsSplit)
	{
		OutLeaves.Add(Node);
		return;
	}

	FSolarOrbzChunkAddress Children[4];
	Node.GetChildren(Children);
	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		VisitNode(Children[ChildIndex], Radius, ViewerWorldPosition, Settings, OutLeaves);
	}
}
