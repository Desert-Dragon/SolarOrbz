// SolarOrbzChunkRestrictedQuadtree implementation. See the header for the full algorithm write-up,
// pentagon-vertex decision/reasoning, termination argument, and verification methodology/numbers -
// this file intentionally keeps its own comments short and points back there rather than
// duplicating it. Mirrors (function-by-function) the verified Python source's
// apply_neighbor_depth_restriction/find_covering_leaf_depth - see the header comment for where that
// Python source's own numbers came from.

#include "SolarOrbzChunkRestrictedQuadtree.h"

bool FSolarOrbzChunkRestrictedQuadtree::FindCoveringLeafDepth(
	const FSolarOrbzChunkAddress& Address,
	const TSet<FSolarOrbzChunkAddress>& LeafSet,
	int32& OutDepth)
{
	FSolarOrbzChunkAddress Current = Address;

	// Bounded by MaxDepth+1 steps: Address can be at Depth up to MaxDepth, and GetParent() strictly
	// decreases Depth by 1 each call until Depth 0 (where it returns itself - see GetParent's own
	// documented Depth-0 behavior), so at most MaxDepth+1 distinct (Current) values are ever visited
	// before this loop's own explicit Depth<=0 break below fires - this iteration cap is deliberately
	// redundant with that break, same "walker owns its own bound, not just the callee" discipline
	// SolarOrbzChunkResidencyWalk.cpp already documents for its own MaxDepth guard.
	for (int32 Step = 0; Step <= FSolarOrbzChunkAddress::MaxDepth; ++Step)
	{
		if (LeafSet.Contains(Current))
		{
			OutDepth = Current.Depth;
			return true;
		}

		if (Current.Depth <= 0)
		{
			break;
		}

		Current = Current.GetParent();
	}

	// Reached Depth 0 (or exhausted the iteration cap, which cannot actually happen before Depth 0
	// is reached - see the comment above) without a match: the leaf that actually covers Address is
	// a DESCENDANT of Address, i.e. finer/deeper than Address's own Depth - not this function's
	// concern (see the header's algorithm comment on how callers treat a false return).
	return false;
}

void FSolarOrbzChunkRestrictedQuadtree::ApplyNeighborDepthRestriction(TArray<FSolarOrbzChunkAddress>& InOutLeaves)
{
	TSet<FSolarOrbzChunkAddress> LeafSet(InOutLeaves);

	// Plain FIFO worklist, deliberately allowed to contain duplicate/stale entries (an address
	// queued once per edge that triggered a split, or an address popped after it was itself already
	// removed from LeafSet by an earlier pop) - matches the verified Python source exactly (see the
	// header comment: the termination argument does not depend on de-duplicating this queue, only
	// on every split permanently reducing the bounded total "depth debt" somewhere in the tree), and
	// keeping it this simple avoids needing a second TSet just to track worklist membership.
	//
	// Implemented as an append-only array plus a read cursor (WorklistHead), NOT TArray::RemoveAt(0)
	// each pop - removing from the front of a TArray is O(n) per call (it shifts every remaining
	// element down), which would make this whole pass O(n^2) in the worklist's total length for no
	// reason; a cursor makes every pop O(1) amortized, same complexity the Python deque.popleft()
	// version already enjoys. The array only ever grows during this pass and is discarded at the
	// end, so paying its memory for already-popped entries is a non-issue.
	TArray<FSolarOrbzChunkAddress> Worklist(InOutLeaves);
	int32 WorklistHead = 0;

	while (WorklistHead < Worklist.Num())
	{
		const FSolarOrbzChunkAddress L = Worklist[WorklistHead++];

		if (!LeafSet.Contains(L))
		{
			// L was itself split away (removed from LeafSet) by an earlier worklist entry's split
			// since being queued - nothing to check for an address that is no longer a leaf.
			continue;
		}

		// Deliberately only the 3 ordinary edges, never GetPentagonVertexNeighbors - see the
		// header's dedicated "pentagon-vertex" section for the reasoning and the measured bound
		// (worst observed same-point depth spread: 2) that decision rests on.
		static const ESolarOrbzChunkEdge Edges[3] = { ESolarOrbzChunkEdge::AB, ESolarOrbzChunkEdge::BC, ESolarOrbzChunkEdge::CA };

		for (const ESolarOrbzChunkEdge Edge : Edges)
		{
			FSolarOrbzChunkAddress NeighborAddr;
			L.GetEdgeNeighbor(Edge, NeighborAddr);

			int32 CoveringDepth = INDEX_NONE;
			if (!FindCoveringLeafDepth(NeighborAddr, LeafSet, CoveringDepth))
			{
				// Neighbor region is already finer/deeper than L - nothing to fix for this edge
				// (L's depth can only ever be "too shallow" relative to a neighbor, never "too
				// deep" - see the header's algorithm comment).
				continue;
			}

			if (CoveringDepth >= L.Depth - 1)
			{
				// Within the 1-level restriction already.
				continue;
			}

			// CoveringDepth < L.Depth - 1: the covering ancestor is too shallow relative to L.
			// Re-walk from NeighborAddr to find that actual ancestor's ADDRESS (FindCoveringLeafDepth
			// only returned its Depth) - cheap (at most MaxDepth GetParent() calls) and avoids
			// FindCoveringLeafDepth needing an extra out-parameter solely for this one caller.
			FSolarOrbzChunkAddress CoveringAncestor = NeighborAddr;
			while (CoveringAncestor.Depth != CoveringDepth)
			{
				CoveringAncestor = CoveringAncestor.GetParent();
			}
			checkf(LeafSet.Contains(CoveringAncestor),
				TEXT("SolarOrbz ChunkRestrictedQuadtree: re-walked covering ancestor not found in LeafSet - LeafSet must have been mutated between FindCoveringLeafDepth and this re-walk, which should be impossible (both happen synchronously, no other code runs in between)."));

			LeafSet.Remove(CoveringAncestor);

			FSolarOrbzChunkAddress Children[4];
			CoveringAncestor.GetChildren(Children);
			for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
			{
				LeafSet.Add(Children[ChildIndex]);
				Worklist.Add(Children[ChildIndex]);
			}

			// Re-queue L itself - one split might not be enough (if CoveringDepth+1 is still
			// < L.Depth - 1, the new children will discover that on their own next pop, but L's
			// OWN relationship to its other edges, and to this same edge again, needs re-checking
			// too), see the header's algorithm comment for the full reasoning. L may get queued
			// again more than once across its 3 edges in this same pass if more than one edge
			// triggers a split - harmless, just means L gets re-checked slightly more than the
			// strict minimum (same as the verified Python source - no de-dup, see above).
			Worklist.Add(L);
		}
	}

	InOutLeaves = LeafSet.Array();
}
