#include "SolarOrbzTerrainGraph.h"
#include "SolarOrbzTerrainLayers.h"

DEFINE_LOG_CATEGORY(LogSolarOrbzTerrainGraph);

const FName USolarOrbzTerrainGraphNode::HeightPinCategory(TEXT("SolarOrbzHeight"));
const FName USolarOrbzTerrainGraphNode::HeightInPinName(TEXT("HeightIn"));
const FName USolarOrbzTerrainGraphNode::HeightOutPinName(TEXT("HeightOut"));

// ================================================================================================
// USolarOrbzTerrainGraphNode
// ================================================================================================

void USolarOrbzTerrainGraphNode::AllocateDefaultPins()
{
	FEdGraphPinType HeightPinType;
	HeightPinType.PinCategory = HeightPinCategory;

	if (!bIsStartNode)
	{
		CreatePin(EGPD_Input, HeightPinType, HeightInPinName, INDEX_NONE);
	}
	if (!bIsOutputNode)
	{
		CreatePin(EGPD_Output, HeightPinType, HeightOutPinName, INDEX_NONE);
	}
}

FText USolarOrbzTerrainGraphNode::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (bIsStartNode)
	{
		return NSLOCTEXT("SolarOrbzTerrainGraph", "StartNodeTitle", "Start");
	}
	if (bIsOutputNode)
	{
		return NSLOCTEXT("SolarOrbzTerrainGraph", "OutputNodeTitle", "Output");
	}
	if (Layer)
	{
		return Layer->GetClass()->GetDisplayNameText();
	}
	return NSLOCTEXT("SolarOrbzTerrainGraph", "UnknownLayerNodeTitle", "Layer");
}

FText USolarOrbzTerrainGraphNode::GetTooltipText() const
{
	if (bIsStartNode)
	{
		return NSLOCTEXT("SolarOrbzTerrainGraph", "StartNodeTooltip", "The base sphere, before any terrain layer is applied.");
	}
	if (bIsOutputNode)
	{
		return NSLOCTEXT("SolarOrbzTerrainGraph", "OutputNodeTooltip", "The final combined height the planet actually samples.");
	}
	if (Layer)
	{
		return Layer->GetClass()->GetDisplayNameText();
	}
	return FText::GetEmpty();
}

UEdGraphPin* USolarOrbzTerrainGraphNode::GetHeightInPin() const
{
	return FindPin(HeightInPinName, EGPD_Input);
}

UEdGraphPin* USolarOrbzTerrainGraphNode::GetHeightOutPin() const
{
	return FindPin(HeightOutPinName, EGPD_Output);
}

// ================================================================================================
// USolarOrbzTerrainGraphSchema
// ================================================================================================

const FPinConnectionResponse USolarOrbzTerrainGraphSchema::CanCreateConnection(const UEdGraphPin* PinA, const UEdGraphPin* PinB) const
{
	if (!PinA || !PinB)
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Warning, TEXT("CanCreateConnection called with a null pin"));
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("Invalid pin"));
	}
	if (PinA->GetOwningNode() == PinB->GetOwningNode())
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Verbose, TEXT("CanCreateConnection: disallowed, %s would connect to itself"), *PinA->GetOwningNode()->GetName());
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("Can't connect a node to itself"));
	}
	if (PinA->Direction == PinB->Direction)
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Verbose, TEXT("CanCreateConnection: disallowed, %s and %s are both %s pins"), *PinA->GetOwningNode()->GetName(), *PinB->GetOwningNode()->GetName(), (PinA->Direction == EGPD_Input) ? TEXT("Height-In") : TEXT("Height-Out"));
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("A Height-In pin must connect to a Height-Out pin, not another pin of the same direction"));
	}

	const bool bAIsInput = (PinA->Direction == EGPD_Input);
	const UEdGraphPin* InputPin = bAIsInput ? PinA : PinB;
	const UEdGraphPin* OutputPin = bAIsInput ? PinB : PinA;

	// Strict chain: each Height-In/Height-Out pin connects to exactly one neighbor (matches
	// USolarOrbzTerrainLayerStack::Layers being a plain ordered array, not a tree) - making a new
	// connection into a pin that already has one replaces it rather than allowing a second.
	const bool bInputHasLink = InputPin->LinkedTo.Num() > 0;
	const bool bOutputHasLink = OutputPin->LinkedTo.Num() > 0;

	if (bInputHasLink && bOutputHasLink)
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Verbose, TEXT("CanCreateConnection: %s <-> %s replaces both pins' existing connections"), *PinA->PinName.ToString(), *PinB->PinName.ToString());
		return FPinConnectionResponse(CONNECT_RESPONSE_BREAK_OTHERS_AB, TEXT("Replaces both pins' existing connections"));
	}
	if (bInputHasLink)
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Verbose, TEXT("CanCreateConnection: %s <-> %s replaces the input pin's existing connection"), *PinA->PinName.ToString(), *PinB->PinName.ToString());
		return FPinConnectionResponse(bAIsInput ? CONNECT_RESPONSE_BREAK_OTHERS_A : CONNECT_RESPONSE_BREAK_OTHERS_B, TEXT("Replaces this layer's existing input connection"));
	}
	if (bOutputHasLink)
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Verbose, TEXT("CanCreateConnection: %s <-> %s replaces the output pin's existing connection"), *PinA->PinName.ToString(), *PinB->PinName.ToString());
		return FPinConnectionResponse(bAIsInput ? CONNECT_RESPONSE_BREAK_OTHERS_B : CONNECT_RESPONSE_BREAK_OTHERS_A, TEXT("Replaces this layer's existing output connection"));
	}

	return FPinConnectionResponse(CONNECT_RESPONSE_MAKE, TEXT("Connect"));
}

// ================================================================================================
// USolarOrbzTerrainGraph
// ================================================================================================

void USolarOrbzTerrainGraph::RebuildFromLayers(USolarOrbzTerrainLayerStack* OwningStack)
{
	if (!OwningStack)
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Warning, TEXT("RebuildFromLayers called with a null OwningStack - ignoring"));
		return;
	}

	UE_LOG(LogSolarOrbzTerrainGraph, Log, TEXT("RebuildFromLayers: rebuilding %s from %d layer(s)"), *OwningStack->GetName(), OwningStack->Layers.Num());
	bIsRebuilding = true;

	Schema = USolarOrbzTerrainGraphSchema::StaticClass();

	// The graph is a disposable view - discard whatever nodes exist today, Layers is the only thing
	// that actually needs to survive this.
	TArray<TObjectPtr<UEdGraphNode>> OldNodes = Nodes;
	for (UEdGraphNode* OldNode : OldNodes)
	{
		RemoveNode(OldNode, /*bBreakAllLinks=*/ true);
	}
	StartNode = nullptr;
	OutputNode = nullptr;

	constexpr int32 ColumnSpacing = 250;

	StartNode = NewObject<USolarOrbzTerrainGraphNode>(this, NAME_None, RF_Transactional);
	StartNode->bIsStartNode = true;
	StartNode->CreateNewGuid();
	StartNode->AllocateDefaultPins();
	StartNode->NodePosX = 0;
	StartNode->NodePosY = 0;
	AddNode(StartNode, /*bIsUserAction=*/ false, /*bSelectNewNode=*/ false);

	OutputNode = NewObject<USolarOrbzTerrainGraphNode>(this, NAME_None, RF_Transactional);
	OutputNode->bIsOutputNode = true;
	OutputNode->CreateNewGuid();
	OutputNode->AllocateDefaultPins();
	AddNode(OutputNode, false, false);

	USolarOrbzTerrainGraphNode* PreviousNode = StartNode;
	int32 ColumnIndex = 1;

	for (int32 LayerArrayIndex = 0; LayerArrayIndex < OwningStack->Layers.Num(); ++LayerArrayIndex)
	{
		const TObjectPtr<USolarOrbzTerrainLayer>& Layer = OwningStack->Layers[LayerArrayIndex];
		if (!Layer)
		{
			UE_LOG(LogSolarOrbzTerrainGraph, Warning, TEXT("RebuildFromLayers: %s has a null entry in Layers at index %d - skipping it (it will be dropped from Layers next time this graph compiles)"), *OwningStack->GetName(), LayerArrayIndex);
			continue;
		}
		Layer->EnsureEditorNodeId();

		USolarOrbzTerrainGraphNode* LayerNode = NewObject<USolarOrbzTerrainGraphNode>(this, NAME_None, RF_Transactional);
		LayerNode->Layer = Layer;
		LayerNode->CreateNewGuid();
		LayerNode->AllocateDefaultPins();
		AddNode(LayerNode, false, false);

		if (const FVector2D* SavedPosition = OwningStack->EditorNodePositions.Find(Layer->EditorNodeId))
		{
			LayerNode->NodePosX = (int32)SavedPosition->X;
			LayerNode->NodePosY = (int32)SavedPosition->Y;
		}
		else
		{
			LayerNode->NodePosX = ColumnIndex * ColumnSpacing;
			LayerNode->NodePosY = 0;
		}

		PreviousNode->GetHeightOutPin()->MakeLinkTo(LayerNode->GetHeightInPin());
		PreviousNode = LayerNode;
		++ColumnIndex;
	}

	OutputNode->NodePosX = ColumnIndex * ColumnSpacing;
	OutputNode->NodePosY = 0;
	PreviousNode->GetHeightOutPin()->MakeLinkTo(OutputNode->GetHeightInPin());

	bIsRebuilding = false;
	UE_LOG(LogSolarOrbzTerrainGraph, Log, TEXT("RebuildFromLayers: finished, %d node(s) in graph (including Start/Output)"), Nodes.Num());
}

void USolarOrbzTerrainGraph::CompileToLayers(USolarOrbzTerrainLayerStack* OwningStack) const
{
	if (!OwningStack || !StartNode || !OutputNode)
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Warning, TEXT("CompileToLayers called before the graph has Start/Output nodes (OwningStack=%s) - ignoring"), OwningStack ? *OwningStack->GetName() : TEXT("null"));
		return;
	}

	TArray<TObjectPtr<USolarOrbzTerrainLayer>> NewLayers;
	TMap<FGuid, FVector2D> NewPositions;

	const USolarOrbzTerrainGraphNode* CurrentNode = StartNode;
	const int32 MaxSteps = Nodes.Num() + 1;
	bool bReachedOutput = false;

	for (int32 StepCount = 0; CurrentNode && StepCount <= MaxSteps; ++StepCount)
	{
		if (CurrentNode == OutputNode)
		{
			bReachedOutput = true;
			break;
		}

		const UEdGraphPin* OutPin = CurrentNode->GetHeightOutPin();
		const USolarOrbzTerrainGraphNode* NextNode = nullptr;
		if (OutPin && OutPin->LinkedTo.Num() > 0)
		{
			NextNode = Cast<USolarOrbzTerrainGraphNode>(OutPin->LinkedTo[0]->GetOwningNode());
		}

		if (NextNode && NextNode->Layer)
		{
			NextNode->Layer->EnsureEditorNodeId();
			NewLayers.Add(NextNode->Layer);
			NewPositions.Add(NextNode->Layer->EditorNodeId, FVector2D(NextNode->NodePosX, NextNode->NodePosY));
		}

		CurrentNode = NextNode;
	}

	if (!bReachedOutput)
	{
		UE_LOG(LogSolarOrbzTerrainGraph, Warning, TEXT("CompileToLayers: chain from Start never reached Output (broken mid-edit?) - %s's Layers will be truncated to the %d layer(s) actually reached"), *OwningStack->GetName(), NewLayers.Num());
	}

	UE_LOG(LogSolarOrbzTerrainGraph, Log, TEXT("CompileToLayers: writing %d layer(s) back into %s"), NewLayers.Num(), *OwningStack->GetName());

	OwningStack->Modify();
	OwningStack->Layers = MoveTemp(NewLayers);
	OwningStack->EditorNodePositions = MoveTemp(NewPositions);
}
