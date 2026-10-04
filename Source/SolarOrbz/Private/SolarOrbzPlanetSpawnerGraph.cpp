#include "SolarOrbzPlanetSpawnerGraph.h"

DEFINE_LOG_CATEGORY(LogSolarOrbzPlanetSpawner);

const FName USolarOrbzPlanetSpawnerGraphNode::ModulePinCategory(TEXT("SolarOrbzPlanetModule"));
const FName USolarOrbzPlanetSpawnerGraphNode::ModuleOutPinName(TEXT("Out"));

namespace
{
	const FName PlanetPin_BaseSphere(TEXT("Base Sphere"));
	const FName PlanetPin_Terrain(TEXT("Terrain"));
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
		CreatePin(EGPD_Input, PinType, PlanetPin_Terrain, INDEX_NONE);
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
	UE_LOG(LogSolarOrbzPlanetSpawner, Verbose, TEXT("CanCreateConnection: disallowed - this graph's layout is fixed"));
	return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, TEXT("This graph's layout is fixed - module assignments are edited via the Details panel, not by rewiring connections."));
}

// ================================================================================================
// USolarOrbzPlanetSpawnerGraph
// ================================================================================================

void USolarOrbzPlanetSpawnerGraph::BuildFixedLayout()
{
	UE_LOG(LogSolarOrbzPlanetSpawner, Log, TEXT("BuildFixedLayout: building the fixed 5-module + Planet layout"));

	Schema = USolarOrbzPlanetSpawnerGraphSchema::StaticClass();

	BaseSphereConfig = NewObject<USolarOrbzPlanetBaseSphereConfig>(this, NAME_None, RF_Transactional);
	TerrainModule = NewObject<USolarOrbzPlanetTerrainModule>(this, NAME_None, RF_Transactional);
	BiomeModule = NewObject<USolarOrbzPlanetBiomeModule>(this, NAME_None, RF_Transactional);
	ClimateModule = NewObject<USolarOrbzPlanetClimateModule>(this, NAME_None, RF_Transactional);
	ProfileModule = NewObject<USolarOrbzPlanetProfileModule>(this, NAME_None, RF_Transactional);

	constexpr int32 ColumnSpacing = 260;
	constexpr int32 RowSpacing = 90;

	auto MakeModuleNode = [this](UObject* Config, const FText& Title, int32 Row) -> USolarOrbzPlanetSpawnerGraphNode*
	{
		USolarOrbzPlanetSpawnerGraphNode* Node = NewObject<USolarOrbzPlanetSpawnerGraphNode>(this, NAME_None, RF_Transactional);
		Node->ModuleConfig = Config;
		Node->DisplayTitle = Title;
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		Node->NodePosX = 0;
		Node->NodePosY = Row * RowSpacing;
		AddNode(Node, /*bIsUserAction=*/ false, /*bSelectNewNode=*/ false);
		return Node;
	};

	USolarOrbzPlanetSpawnerGraphNode* BaseSphereNode = MakeModuleNode(BaseSphereConfig, NSLOCTEXT("SolarOrbzPlanetSpawner", "BaseSphereNodeTitle", "Base Sphere"), 0);
	USolarOrbzPlanetSpawnerGraphNode* TerrainNode = MakeModuleNode(TerrainModule, NSLOCTEXT("SolarOrbzPlanetSpawner", "TerrainNodeTitle", "Terrain Stack"), 1);
	USolarOrbzPlanetSpawnerGraphNode* BiomeNode = MakeModuleNode(BiomeModule, NSLOCTEXT("SolarOrbzPlanetSpawner", "BiomeNodeTitle", "Biome Stack"), 2);
	USolarOrbzPlanetSpawnerGraphNode* ClimateNode = MakeModuleNode(ClimateModule, NSLOCTEXT("SolarOrbzPlanetSpawner", "ClimateNodeTitle", "Climate Simulation"), 3);
	USolarOrbzPlanetSpawnerGraphNode* ProfileNode = MakeModuleNode(ProfileModule, NSLOCTEXT("SolarOrbzPlanetSpawner", "ProfileNodeTitle", "Profile"), 4);

	PlanetNode = NewObject<USolarOrbzPlanetSpawnerGraphNode>(this, NAME_None, RF_Transactional);
	PlanetNode->bIsPlanetNode = true;
	PlanetNode->DisplayTitle = NSLOCTEXT("SolarOrbzPlanetSpawner", "PlanetNodeTitle", "Planet");
	PlanetNode->CreateNewGuid();
	PlanetNode->AllocateDefaultPins();
	PlanetNode->NodePosX = ColumnSpacing;
	PlanetNode->NodePosY = 2 * RowSpacing;
	AddNode(PlanetNode, false, false);

	BaseSphereNode->GetModuleOutPin()->MakeLinkTo(PlanetNode->GetPlanetInputPin(PlanetPin_BaseSphere));
	TerrainNode->GetModuleOutPin()->MakeLinkTo(PlanetNode->GetPlanetInputPin(PlanetPin_Terrain));
	BiomeNode->GetModuleOutPin()->MakeLinkTo(PlanetNode->GetPlanetInputPin(PlanetPin_Biome));
	ClimateNode->GetModuleOutPin()->MakeLinkTo(PlanetNode->GetPlanetInputPin(PlanetPin_Climate));
	ProfileNode->GetModuleOutPin()->MakeLinkTo(PlanetNode->GetPlanetInputPin(PlanetPin_Profile));

	UE_LOG(LogSolarOrbzPlanetSpawner, Log, TEXT("BuildFixedLayout: done, %d node(s) in graph"), Nodes.Num());
}
