// See SolarOrbzChunkResidentSetDiff.h for the algorithm, API contract, and verification methodology.

#include "SolarOrbzChunkResidentSetDiff.h"

void FSolarOrbzChunkResidentSetDiff::ComputeDiff(
	const TArray<FSolarOrbzChunkAddress>& Previous,
	const TArray<FSolarOrbzChunkAddress>& Desired,
	TArray<FSolarOrbzChunkAddress>& OutToSpawn,
	TArray<FSolarOrbzChunkAddress>& OutToDespawn)
{
	OutToSpawn.Reset();
	OutToDespawn.Reset();

	// FSolarOrbzChunkAddress already has operator==/GetTypeHash (SolarOrbzIcoSphereChunk.h), so it
	// works directly as a TSet key with no further machinery needed here.
	const TSet<FSolarOrbzChunkAddress> PreviousSet(Previous);
	const TSet<FSolarOrbzChunkAddress> DesiredSet(Desired);

	// ToSpawn = Desired - Previous.
	for (const FSolarOrbzChunkAddress& Address : DesiredSet)
	{
		if (!PreviousSet.Contains(Address))
		{
			OutToSpawn.Add(Address);
		}
	}

	// ToDespawn = Previous - Desired.
	for (const FSolarOrbzChunkAddress& Address : PreviousSet)
	{
		if (!DesiredSet.Contains(Address))
		{
			OutToDespawn.Add(Address);
		}
	}

	// Addresses in both PreviousSet and DesiredSet are deliberately not written to either output
	// array - per this file's own header comment, those are "left alone" and need no action from a
	// caller beyond this diff itself.
}
