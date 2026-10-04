#include "SolarOrbzPlanetSpawnerGraph.h"
#include "SolarOrbzTerrainGraph.h"
#include "SolarOrbzTerrainLayers.h"

DEFINE_LOG_CATEGORY(LogSolarOrbzPlanetSpawner);

const FName USolarOrbzPlanetSpawnerGraphNode::ModulePinCategory(TEXT("SolarOrbzPlanetModule"));
const FName USolarOrbzPlanetSpawnerGraphNode::ModuleOutPinName(TEXT("Out"));

namespace
{
	const FName PlanetPin_BaseSphere(TEXT("Base Sphere"));
	const FName PlanetPin_Biome(TEXT("Biome"));
	const FName PlanetPin_Climate(TEXT("Climate"));
	const FName PlanetPin_Profile(TEXT("Profile"));
}

// ================================================================================================
// USolarOrbzPlanetSpawnerGraphNode
// ================================================================================================

void USolarOrbzPlanetSpawnerGraphNode::AllocateDefaultPins()
{
	FEdGraphPinType PinType;
	PinType.PinCategory = ModulePinCategory;

	if (bIsPlanetNode)
	{
		CreatePin(EGPD_Input, PinType, PlanetPin_BaseSphere, INDEX_NONE);
		CreatePin(EGPD_Input, PinType, PlanetPin_Biome, INDEX_NONE);
		CreatePin(EGPD_Input, PinType, PlanetPin_Climate, INDEX_NONE);
		CreatePin(EGPD_Input, PinType, PlanetPin_Profile, INDEX_NONE);
	}
	else
	{
		CreatePin(EGPD_Output, PinType, ModuleOutPinName, INDEX_NONE);
	}
}

FText USolarOrbzPlanetSpawnerGraphNode::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return DisplayTitle;
}

UEdGraphPin* USolarOrbzPlanetSpawnerGraphNode::GetModuleOutPin() const
{
	return FindPin(ModuleOutPinName, EGPD_Output);
}

UEdGraphPin* USolarOrbzPlanetSpawnerGraphNode::GetPlanetInputPin(FName InputPinName) const
{
	return FindPin(InputPinName, EGPD_Input);
}

// ================================================================================================
// USolarOrbzPlanetSpawnerGraphSchema
// ================================================================================================

const FPinConnectionResponse USolarOrbzPlanetSpawnerGraphSchema::CanCreateConnection(const UEdGraphPin* PinA, const UEdGraphPin* PinB) const
{
	if (!PinA || !PinB)
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Warning, TEXT("CanCreateConnection called with a null pin"));
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("Invalid pin"));
	}

	// Fixed module/Planet topology - never user-editable, regardless of which side is which.
	if (PinA->PinType.PinCategory == USolarOrbzPlanetSpawnerGraphNode::ModulePinCategory
		|| PinB->PinType.PinCategory == USolarOrbzPlanetSpawnerGraphNode::ModulePinCategory)
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Verbose, TEXT("CanCreateConnection: disallowed - the fixed module/Planet layout is not user-editable"));
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("This part of the graph's layout is fixed - module assignments are edited via the Details panel, not by rewiring connections."));
	}

	// Everything else is the embedded terrain chain's Height-In/Height-Out pins - same strict-chain
	// rule as USolarOrbzTerrainGraphSchema, duplicated here since this schema also has to handle the
	// module pin category check above, which that standalone schema has no reason to know about.
	if (PinA->GetOwningNode() == PinB->GetOwningNode())
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Verbose, TEXT("CanCreateConnection: disallowed, %s would connect to itself"), *PinA->GetOwningNode()->GetName());
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("Can't connect a node to itself"));
	}
	if (PinA->Direction == PinB->Direction)
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Verbose, TEXT("CanCreateConnection: disallowed, %s and %s are both %s pins"), *PinA->GetOwningNode()->GetName(), *PinB->GetOwningNode()->GetName(), (PinA->Direction == EGPD_Input) ? TEXT("Height-In") : TEXT("Height-Out"));
		return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("A Height-In pin must connect to a Height-Out pin, not another pin of the same direction"));
	}

	const bool bAIsInput = (PinA->Direction == EGPD_Input);
	const UEdGraphPin* InputPin = bAIsInput ? PinA : PinB;
	const UEdGraphPin* OutputPin = bAIsInput ? PinB : PinA;

	const bool bInputHasLink = InputPin->LinkedTo.Num() > 0;
	const bool bOutputHasLink = OutputPin->LinkedTo.Num() > 0;

	if (bInputHasLink && bOutputHasLink)
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Verbose, TEXT("CanCreateConnection: %s <-> %s replaces both pins' existing connections"), *PinA->PinName.ToString(), *PinB->PinName.ToString());
		return FPinConnectionResponse(CONNECT_RESPONSE_BREAK_OTHERS_AB, TEXT("Replaces both pins' existing connections"));
	}
	if (bInputHasLink)
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Verbose, TEXT("CanCreateConnection: %s <-> %s replaces the input pin's existing connection"), *PinA->PinName.ToString(), *PinB->PinName.ToString());
		return FPinConnectionResponse(bAIsInput ? CONNECT_RESPONSE_BREAK_OTHERS_A : CONNECT_RESPONSE_BREAK_OTHERS_B, TEXT("Replaces this layer's existing input connection"));
	}
	if (bOutputHasLink)
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Verbose, TEXT("CanCreateConnection: %s <-> %s replaces the output pin's existing connection"), *PinA->PinName.ToString(), *PinB->PinName.ToString());
		return FPinConnectionResponse(bAIsInput ? CONNECT_RESPONSE_BREAK_OTHERS_B : CONNECT_RESPONSE_BREAK_OTHERS_A, TEXT("Replaces this layer's existing output connection"));
	}

	return FPinConnectionResponse(CONNECT_RESPONSE_MAKE, TEXT("Connect"));
}

// ================================================================================================
// USolarOrbzPlanetSpawnerGraph
// ================================================================================================

void USolarOrbzPlanetSpawnerGraph::BuildFixedLayout()
{
	UE_LOG(LogSolarOrbzPlanetSpawner, Log, TEXT("BuildFixedLayout: building the fixed 4-module + Planet layout, plus the embedded terrain chain"));

	Schema = USolarOrbzPlanetSpawnerGraphSchema::StaticClass();

	BaseSphereConfig = NewObject<USolarOrbzPlanetBaseSphereConfig>(this, NAME_None, RF_Transactional);
	BiomeModule = NewObject<USolarOrbzPlanetBiomeModule>(this, NAME_None, RF_Transactional);
	ClimateModule = NewObject<USolarOrbzPlanetClimateModule>(this, NAME_None, RF_Transactional);
	ProfileModule = NewObject<USolarOrbzPlanetProfileModule>(this, NAME_None, RF_Transactional);
	EmbeddedTerrainStack = NewObject<USolarOrbzTerrainLayerStack>(this, NAME_None, RF_Transactional);

	constexpr int32 ColumnSpacing = 260;
	constexpr int32 RowSpacing = 90;
	constexpr int32 FixedModuleRowY = -180;

	auto MakeModuleNode = [this, FixedModuleRowY](UObject* Config, const FText& Title, int32 Column) -> USolarOrbzPlanetSpawnerGraphNode*
	{
		USolarOrbzPlanetSpawnerGraphNode* Node = NewObject<USolarOrbzPlanetSpawnerGraphNode>(this, NAME_None, RF_Transactional);
		Node->ModuleConfig = Config;
		Node->DisplayTitle = Title;
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		Node->NodePosX = Column * ColumnSpacing;
		Node->NodePosY = FixedModuleRowY;
		AddNode(Node, /*bIsUserAction=*/ false, /*bSelectNewNode=*/ false);
		return Node;
	};

	USolarOrbzPlanetSpawnerGraphNode* BaseSphereNode = MakeModuleNode(BaseSphereConfig, NSLOCTEXT("SolarOrbzPlanetSpawner", "BaseSphereNodeTitle", "Base Sphere"), 0);
	USolarOrbzPlanetSpawnerGraphNode* BiomeNode = MakeModuleNode(BiomeModule, NSLOCTEXT("SolarOrbzPlanetSpawner", "BiomeNodeTitle", "Biome Stack"), 1);
	USolarOrbzPlanetSpawnerGraphNode* ClimateNode = MakeModuleNode(ClimateModule, NSLOCTEXT("SolarOrbzPlanetSpawner", "ClimateNodeTitle", "Climate Simulation"), 2);
	USolarOrbzPlanetSpawnerGraphNode* ProfileNode = MakeModuleNode(ProfileModule, NSLOCTEXT("SolarOrbzPlanetSpawner", "ProfileNodeTitle", "Profile"), 3);

	PlanetNode = NewObject<USolarOrbzPlanetSpawnerGraphNode>(this, NAME_None, RF_Transactional);
	PlanetNode->bIsPlanetNode = true;
	PlanetNode->DisplayTitle = NSLOCTEXT("SolarOrbzPlanetSpawner", "PlanetNodeTitle", "Planet");
	PlanetNode->CreateNewGuid();
	PlanetNode->AllocateDefaultPins();
	PlanetNode->NodePosX = ColumnSpacing;
	PlanetNode->NodePosY = FixedModuleRowY + RowSpacing;
	AddNode(PlanetNode, false, false);

	BaseSphereNode->GetModuleOutPin()->MakeLinkTo(PlanetNode->GetPlanetInputPin(PlanetPin_BaseSphere));
	BiomeNode->GetModuleOutPin()->MakeLinkTo(PlanetNode->GetPlanetInputPin(PlanetPin_Biome));
	ClimateNode->GetModuleOutPin()->MakeLinkTo(PlanetNode->GetPlanetInputPin(PlanetPin_Climate));
	ProfileNode->GetModuleOutPin()->MakeLinkTo(PlanetNode->GetPlanetInputPin(PlanetPin_Profile));

	RebuildEmbeddedTerrainChain();

	UE_LOG(LogSolarOrbzPlanetSpawner, Log, TEXT("BuildFixedLayout: done, %d node(s) in graph"), Nodes.Num());
}

void USolarOrbzPlanetSpawnerGraph::RebuildEmbeddedTerrainChain()
{
	if (!EmbeddedTerrainStack)
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Warning, TEXT("RebuildEmbeddedTerrainChain called with no EmbeddedTerrainStack - ignoring"));
		return;
	}

	UE_LOG(LogSolarOrbzPlanetSpawner, Log, TEXT("RebuildEmbeddedTerrainChain: rebuilding from %d layer(s)"), EmbeddedTerrainStack->Layers.Num());
	bIsRebuildingTerrainChain = true;

	// Only remove terrain-chain nodes - this graph's fixed module/Planet nodes share the same Nodes
	// array and must survive this, unlike USolarOrbzTerrainGraph::RebuildFromLayers() which owns its
	// whole graph and can safely discard everything in it.
	TArray<TObjectPtr<UEdGraphNode>> NodesToRemove;
	for (const TObjectPtr<UEdGraphNode>& Node : Nodes)
	{
		if (Cast<USolarOrbzTerrainGraphNode>(Node.Get()))
		{
			NodesToRemove.Add(Node);
		}
	}
	for (const TObjectPtr<UEdGraphNode>& Node : NodesToRemove)
	{
		RemoveNode(Node.Get(), /*bBreakAllLinks=*/ true);
	}
	EmbeddedTerrainStartNode = nullptr;
	EmbeddedTerrainOutputNode = nullptr;

	constexpr int32 ColumnSpacing = 250;
	constexpr int32 TerrainRowY = 220;

	EmbeddedTerrainStartNode = NewObject<USolarOrbzTerrainGraphNode>(this, NAME_None, RF_Transactional);
	EmbeddedTerrainStartNode->bIsStartNode = true;
	EmbeddedTerrainStartNode->CreateNewGuid();
	EmbeddedTerrainStartNode->AllocateDefaultPins();
	EmbeddedTerrainStartNode->NodePosX = 0;
	EmbeddedTerrainStartNode->NodePosY = TerrainRowY;
	AddNode(EmbeddedTerrainStartNode, false, false);

	EmbeddedTerrainOutputNode = NewObject<USolarOrbzTerrainGraphNode>(this, NAME_None, RF_Transactional);
	EmbeddedTerrainOutputNode->bIsOutputNode = true;
	EmbeddedTerrainOutputNode->CreateNewGuid();
	EmbeddedTerrainOutputNode->AllocateDefaultPins();
	AddNode(EmbeddedTerrainOutputNode, false, false);

	USolarOrbzTerrainGraphNode* PreviousNode = EmbeddedTerrainStartNode;
	int32 ColumnIndex = 1;

	for (int32 LayerIndex = 0; LayerIndex < EmbeddedTerrainStack->Layers.Num(); ++LayerIndex)
	{
		const TObjectPtr<USolarOrbzTerrainLayer>& Layer = EmbeddedTerrainStack->Layers[LayerIndex];
		if (!Layer)
		{
			UE_LOG(LogSolarOrbzPlanetSpawner, Warning, TEXT("RebuildEmbeddedTerrainChain: null entry in Layers at index %d - skipping it"), LayerIndex);
			continue;
		}
		Layer->EnsureEditorNodeId();

		USolarOrbzTerrainGraphNode* LayerNode = NewObject<USolarOrbzTerrainGraphNode>(this, NAME_None, RF_Transactional);
		LayerNode->Layer = Layer;
		LayerNode->CreateNewGuid();
		LayerNode->AllocateDefaultPins();
		AddNode(LayerNode, false, false);

		if (const FVector2D* SavedPosition = EmbeddedTerrainStack->EditorNodePositions.Find(Layer->EditorNodeId))
		{
			LayerNode->NodePosX = (int32)SavedPosition->X;
			LayerNode->NodePosY = (int32)SavedPosition->Y;
		}
		else
		{
			LayerNode->NodePosX = ColumnIndex * ColumnSpacing;
			LayerNode->NodePosY = TerrainRowY;
		}

		PreviousNode->GetHeightOutPin()->MakeLinkTo(LayerNode->GetHeightInPin());
		PreviousNode = LayerNode;
		++ColumnIndex;
	}

	EmbeddedTerrainOutputNode->NodePosX = ColumnIndex * ColumnSpacing;
	EmbeddedTerrainOutputNode->NodePosY = TerrainRowY;
	PreviousNode->GetHeightOutPin()->MakeLinkTo(EmbeddedTerrainOutputNode->GetHeightInPin());

	bIsRebuildingTerrainChain = false;
	UE_LOG(LogSolarOrbzPlanetSpawner, Log, TEXT("RebuildEmbeddedTerrainChain: done, %d node(s) in graph"), Nodes.Num());
}

void USolarOrbzPlanetSpawnerGraph::CompileEmbeddedTerrainChain() const
{
	if (!EmbeddedTerrainStack || !EmbeddedTerrainStartNode || !EmbeddedTerrainOutputNode)
	{
		UE_LOG(LogSolarOrbzPlanetSpawner, Warning, TEXT("CompileEmbeddedTerrainChain called before the chain has Start/Output nodes - ignoring"));
		return;
	}

	TArray<TObjectPtr<USolarOrbzTerrainLayer>> NewLayers;
	TMap<FGuid, FVector2D> NewPositions;

	const USolarOrbzTerrainGraphNode* CurrentNode = EmbeddedTerrainStartNode;
	const int32 MaxSteps = Nodes.Num() + 1;
	bool bReachedOutput = false;

	for (int32 StepCount = 0; CurrentNode && StepCount <= MaxSteps; ++StepCount)
	{
		if (CurrentNode == EmbeddedTerrainOutputNode)
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
		UE_LOG(LogSolarOrbzPlanetSpawner, Warning, TEXT("CompileEmbeddedTerrainChain: chain never reached Output (broken mid-edit?) - truncated to %d layer(s)"), NewLayers.Num());
	}

	UE_LOG(LogSolarOrbzPlanetSpawner, Log, TEXT("CompileEmbeddedTerrainChain: writing %d layer(s) back into the embedded terrain stack"), NewLayers.Num());

	EmbeddedTerrainStack->Modify();
	EmbeddedTerrainStack->Layers = MoveTemp(NewLayers);
	EmbeddedTerrainStack->EditorNodePositions = MoveTemp(NewPositions);
}
