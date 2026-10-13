// CLI frontend: config (JSON) in, heightmap + biome-color (+ optional temperature/moisture) PNGs
// and stats out. Builds a small ordered pipeline (TerrainNode.h) instead of a single Continent
// bake - see that header and src/core/README-level docs for the "linear stack, not an arbitrary
// node graph" scoping decision. No UE dependency - see src/core/ContinentLayer.h for the Continent-
// specific scope-cut/fidelity notes (RNG, noise, mesh).
#include "TerrainNode.h"
#include "Nodes/ContinentNode.h"
#include "Nodes/NoiseNode.h"
#include "Nodes/HeightAdjustNode.h"
#include "Nodes/CombineNode.h"
#include "Nodes/HydraulicErosionNode.h"
#include "Nodes/ThermalErosionNode.h"
#include "Climate.h"
#include "BiomeClassifier.h"
#include "LatLongGrid.h"
#include <nlohmann/json.hpp>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>

namespace
{
    EContinentAlgorithm AlgorithmFromString(const std::string& S)
    {
        if (S == "VoronoiGrowth") return EContinentAlgorithm::VoronoiGrowth;
        if (S == "PlateTectonics") return EContinentAlgorithm::PlateTectonics;
        return EContinentAlgorithm::RadialSeeds;
    }

    const char* AlgorithmToString(EContinentAlgorithm A)
    {
        switch (A)
        {
        case EContinentAlgorithm::VoronoiGrowth: return "VoronoiGrowth";
        case EContinentAlgorithm::PlateTectonics: return "PlateTectonics";
        case EContinentAlgorithm::RadialSeeds: default: return "RadialSeeds";
        }
    }

    ENoiseType NoiseTypeFromString(const std::string& S)
    {
        if (S == "Ridged") return ENoiseType::Ridged;
        if (S == "Billow") return ENoiseType::Billow;
        if (S == "Value") return ENoiseType::Value;
        if (S == "Voronoi") return ENoiseType::Voronoi;
        return ENoiseType::Perlin;
    }

    // Every field is named identically to FContinentConfig's own member - mirrors the UE plugin's
    // UPROPERTY names (see ContinentLayer.h) so a config file reads like the plugin's own Details panel.
    void ApplyJsonToContinentConfig(const nlohmann::json& J, FContinentConfig& Config)
    {
        if (J.contains("Algorithm")) Config.Algorithm = AlgorithmFromString(J.at("Algorithm").get<std::string>());
        if (J.contains("NumContinents")) Config.NumContinents = J.at("NumContinents").get<int32_t>();
        if (J.contains("MinContinentRadiusDegrees")) Config.MinContinentRadiusDegrees = J.at("MinContinentRadiusDegrees").get<float>();
        if (J.contains("MaxContinentRadiusDegrees")) Config.MaxContinentRadiusDegrees = J.at("MaxContinentRadiusDegrees").get<float>();
        if (J.contains("NumIslands")) Config.NumIslands = J.at("NumIslands").get<int32_t>();
        if (J.contains("MinIslandRadiusDegrees")) Config.MinIslandRadiusDegrees = J.at("MinIslandRadiusDegrees").get<float>();
        if (J.contains("MaxIslandRadiusDegrees")) Config.MaxIslandRadiusDegrees = J.at("MaxIslandRadiusDegrees").get<float>();
        if (J.contains("bHasNorthPolarContinent")) Config.bHasNorthPolarContinent = J.at("bHasNorthPolarContinent").get<bool>();
        if (J.contains("bHasSouthPolarContinent")) Config.bHasSouthPolarContinent = J.at("bHasSouthPolarContinent").get<bool>();
        if (J.contains("PolarContinentRadiusDegrees")) Config.PolarContinentRadiusDegrees = J.at("PolarContinentRadiusDegrees").get<float>();
        if (J.contains("CoastlineWarpStrength")) Config.CoastlineWarpStrength = J.at("CoastlineWarpStrength").get<float>();
        if (J.contains("CoastlineWarpFrequency")) Config.CoastlineWarpFrequency = J.at("CoastlineWarpFrequency").get<float>();
        if (J.contains("CoastlineNoiseOctaves")) Config.CoastlineNoiseOctaves = J.at("CoastlineNoiseOctaves").get<int32_t>();
        if (J.contains("CoastlineNoisePersistence")) Config.CoastlineNoisePersistence = J.at("CoastlineNoisePersistence").get<float>();
        if (J.contains("CoastlineNoiseLacunarity")) Config.CoastlineNoiseLacunarity = J.at("CoastlineNoiseLacunarity").get<float>();
        if (J.contains("SubSeedsPerLandmass")) Config.SubSeedsPerLandmass = J.at("SubSeedsPerLandmass").get<int32_t>();
        if (J.contains("SubSeedScatterFraction")) Config.SubSeedScatterFraction = J.at("SubSeedScatterFraction").get<float>();
        if (J.contains("MinSubSeedRadiusFraction")) Config.MinSubSeedRadiusFraction = J.at("MinSubSeedRadiusFraction").get<float>();
        if (J.contains("MaxSubSeedRadiusFraction")) Config.MaxSubSeedRadiusFraction = J.at("MaxSubSeedRadiusFraction").get<float>();
        if (J.contains("BakeGridWidth")) Config.BakeGridWidth = J.at("BakeGridWidth").get<int32_t>();
        if (J.contains("BakeGridHeight")) Config.BakeGridHeight = J.at("BakeGridHeight").get<int32_t>();
        if (J.contains("GrowthDecayMin")) Config.GrowthDecayMin = J.at("GrowthDecayMin").get<float>();
        if (J.contains("GrowthDecayMax")) Config.GrowthDecayMax = J.at("GrowthDecayMax").get<float>();
        if (J.contains("GrowthJitter")) Config.GrowthJitter = J.at("GrowthJitter").get<float>();
        if (J.contains("NumPlates")) Config.NumPlates = J.at("NumPlates").get<int32_t>();
        if (J.contains("OceanicPlateFraction")) Config.OceanicPlateFraction = J.at("OceanicPlateFraction").get<float>();
        if (J.contains("BoundaryInfluenceDegrees")) Config.BoundaryInfluenceDegrees = J.at("BoundaryInfluenceDegrees").get<float>();
        if (J.contains("MountainHeightMeters")) Config.MountainHeightMeters = J.at("MountainHeightMeters").get<float>();
        if (J.contains("TrenchDepthMeters")) Config.TrenchDepthMeters = J.at("TrenchDepthMeters").get<float>();
        if (J.contains("RidgeHeightMeters")) Config.RidgeHeightMeters = J.at("RidgeHeightMeters").get<float>();
        if (J.contains("RiftDepthMeters")) Config.RiftDepthMeters = J.at("RiftDepthMeters").get<float>();
        if (J.contains("InitialDriftSpeedDegreesPerMyr")) Config.InitialDriftSpeedDegreesPerMyr = J.at("InitialDriftSpeedDegreesPerMyr").get<float>();
        if (J.contains("StepsPerRun")) Config.StepsPerRun = J.at("StepsPerRun").get<int32_t>();
        if (J.contains("MillionYearsPerStep")) Config.MillionYearsPerStep = J.at("MillionYearsPerStep").get<float>();
        if (J.contains("StressMatureTimescaleMyr")) Config.StressMatureTimescaleMyr = J.at("StressMatureTimescaleMyr").get<float>();
        if (J.contains("MaxStressMultiplier")) Config.MaxStressMultiplier = J.at("MaxStressMultiplier").get<float>();
        if (J.contains("PlateSplitThresholdMyr")) Config.PlateSplitThresholdMyr = J.at("PlateSplitThresholdMyr").get<float>();
        if (J.contains("CoastlineNoiseAmplitude")) Config.CoastlineNoiseAmplitude = J.at("CoastlineNoiseAmplitude").get<float>();
        if (J.contains("CoastlineNoiseFrequency")) Config.CoastlineNoiseFrequency = J.at("CoastlineNoiseFrequency").get<float>();
        if (J.contains("CoastlineSharpness")) Config.CoastlineSharpness = J.at("CoastlineSharpness").get<float>();
        if (J.contains("OceanFloorDepthMeters")) Config.OceanFloorDepthMeters = J.at("OceanFloorDepthMeters").get<float>();
        if (J.contains("LandPlateauHeightMeters")) Config.LandPlateauHeightMeters = J.at("LandPlateauHeightMeters").get<float>();
    }

    void ApplyJsonToNoiseConfig(const nlohmann::json& J, FNoiseNodeConfig& Config)
    {
        if (J.contains("Type")) Config.Type = NoiseTypeFromString(J.at("Type").get<std::string>());
        if (J.contains("Octaves")) Config.Octaves = J.at("Octaves").get<int32_t>();
        if (J.contains("Frequency")) Config.Frequency = J.at("Frequency").get<float>();
        if (J.contains("Persistence")) Config.Persistence = J.at("Persistence").get<float>();
        if (J.contains("Lacunarity")) Config.Lacunarity = J.at("Lacunarity").get<float>();
        if (J.contains("AmplitudeMeters")) Config.AmplitudeMeters = J.at("AmplitudeMeters").get<float>();
        if (J.contains("SeedOffset")) Config.SeedOffset = J.at("SeedOffset").get<int32_t>();
    }

    void ApplyJsonToHeightAdjustConfig(const nlohmann::json& J, FHeightAdjustConfig& Config)
    {
        if (J.contains("ScaleMultiplier")) Config.ScaleMultiplier = J.at("ScaleMultiplier").get<float>();
        if (J.contains("OffsetMeters")) Config.OffsetMeters = J.at("OffsetMeters").get<float>();
        if (J.contains("PowerCurve")) Config.PowerCurve = J.at("PowerCurve").get<float>();
        if (J.contains("CurveReferenceMeters")) Config.CurveReferenceMeters = J.at("CurveReferenceMeters").get<float>();
        if (J.contains("ClampMinMeters")) Config.ClampMinMeters = J.at("ClampMinMeters").get<float>();
        if (J.contains("ClampMaxMeters")) Config.ClampMaxMeters = J.at("ClampMaxMeters").get<float>();
    }

    void ApplyJsonToHydraulicErosionConfig(const nlohmann::json& J, FHydraulicErosionConfig& Config)
    {
        if (J.contains("NumDroplets")) Config.NumDroplets = J.at("NumDroplets").get<int32_t>();
        if (J.contains("MaxDropletLifetime")) Config.MaxDropletLifetime = J.at("MaxDropletLifetime").get<int32_t>();
        if (J.contains("Inertia")) Config.Inertia = J.at("Inertia").get<float>();
        if (J.contains("SedimentCapacityFactor")) Config.SedimentCapacityFactor = J.at("SedimentCapacityFactor").get<float>();
        if (J.contains("MinSlope")) Config.MinSlope = J.at("MinSlope").get<float>();
        if (J.contains("ErosionRate")) Config.ErosionRate = J.at("ErosionRate").get<float>();
        if (J.contains("DepositionRate")) Config.DepositionRate = J.at("DepositionRate").get<float>();
        if (J.contains("EvaporationRate")) Config.EvaporationRate = J.at("EvaporationRate").get<float>();
        if (J.contains("Gravity")) Config.Gravity = J.at("Gravity").get<float>();
        if (J.contains("InitialWaterVolume")) Config.InitialWaterVolume = J.at("InitialWaterVolume").get<float>();
        if (J.contains("InitialSpeed")) Config.InitialSpeed = J.at("InitialSpeed").get<float>();
        if (J.contains("ErosionStrengthMeters")) Config.ErosionStrengthMeters = J.at("ErosionStrengthMeters").get<float>();
    }

    void ApplyJsonToThermalErosionConfig(const nlohmann::json& J, FThermalErosionConfig& Config)
    {
        if (J.contains("Iterations")) Config.Iterations = J.at("Iterations").get<int32_t>();
        if (J.contains("TalusAngleDegrees")) Config.TalusAngleDegrees = J.at("TalusAngleDegrees").get<float>();
        if (J.contains("TalusErosionRate")) Config.TalusErosionRate = J.at("TalusErosionRate").get<float>();
        if (J.contains("CellSizeMeters")) Config.CellSizeMeters = J.at("CellSizeMeters").get<float>();
    }

    void ApplyJsonToClimateConfig(const nlohmann::json& J, FClimateConfig& Config)
    {
        if (J.contains("EquatorTempC")) Config.EquatorTempC = J.at("EquatorTempC").get<float>();
        if (J.contains("PoleTempC")) Config.PoleTempC = J.at("PoleTempC").get<float>();
        if (J.contains("LapseRateCPerKm")) Config.LapseRateCPerKm = J.at("LapseRateCPerKm").get<float>();
        if (J.contains("OceanMoistureBaseline")) Config.OceanMoistureBaseline = J.at("OceanMoistureBaseline").get<float>();
        if (J.contains("OceanEvaporationRate")) Config.OceanEvaporationRate = J.at("OceanEvaporationRate").get<float>();
        if (J.contains("RainShadowSensitivity")) Config.RainShadowSensitivity = J.at("RainShadowSensitivity").get<float>();
        if (J.contains("RainfallMaxFraction")) Config.RainfallMaxFraction = J.at("RainfallMaxFraction").get<float>();
    }

    void ApplyJsonToBiomeConfig(const nlohmann::json& J, FBiomeConfig& Config)
    {
        if (J.contains("BeachThresholdMeters")) Config.BeachThresholdMeters = J.at("BeachThresholdMeters").get<float>();
        if (J.contains("MountainThresholdMeters")) Config.MountainThresholdMeters = J.at("MountainThresholdMeters").get<float>();
        if (J.contains("ColdTempC")) Config.ColdTempC = J.at("ColdTempC").get<float>();
        if (J.contains("HotTempC")) Config.HotTempC = J.at("HotTempC").get<float>();
        if (J.contains("AridMoisture")) Config.AridMoisture = J.at("AridMoisture").get<float>();
        if (J.contains("DryMoisture")) Config.DryMoisture = J.at("DryMoisture").get<float>();
        if (J.contains("WetMoisture")) Config.WetMoisture = J.at("WetMoisture").get<float>();
    }

    void WriteGrayscalePng(const std::string& Path, const FHeightField& Field, float MinVal, float MaxVal)
    {
        const float Range = std::max(MaxVal - MinVal, 1e-6f);
        std::vector<uint8_t> Pixels(Field.HeightMeters.size());
        for (size_t i = 0; i < Field.HeightMeters.size(); ++i)
        {
            const float Normalized = MathUtil::Clamp((Field.HeightMeters[i] - MinVal) / Range, 0.0f, 1.0f);
            Pixels[i] = (uint8_t)std::lround(Normalized * 255.0f);
        }
        stbi_write_png(Path.c_str(), Field.Width, Field.Height, 1, Pixels.data(), Field.Width);
    }

    // A single linear MinVal..MaxVal grayscale ramp looks nearly flat for a real planet's height
    // field: ocean trenches (thousands of meters deep) and eroded land relief (tens to low
    // thousands of meters) sit on wildly different scales, so land ends up crushed into a sliver
    // near one end of the 0..255 range. Scaling below/above sea level separately (0 maps to a
    // fixed mid-gray) gives both sides real visual contrast regardless of how that magnitude
    // compares - the same split-range idea the GUI's vertex-color ramp already uses.
    void WriteSeaLevelSplitPng(const std::string& Path, const FHeightField& Field, float MinVal, float MaxVal)
    {
        const float OceanRange = std::max(-MinVal, 1e-6f);
        const float LandRange = std::max(MaxVal, 1e-6f);
        std::vector<uint8_t> Pixels(Field.HeightMeters.size());
        for (size_t i = 0; i < Field.HeightMeters.size(); ++i)
        {
            const float H = Field.HeightMeters[i];
            float Normalized;
            if (H <= 0.0f)
            {
                Normalized = 0.5f - 0.5f * MathUtil::Clamp(-H / OceanRange, 0.0f, 1.0f);
            }
            else
            {
                Normalized = 0.5f + 0.5f * MathUtil::Clamp(H / LandRange, 0.0f, 1.0f);
            }
            Pixels[i] = (uint8_t)std::lround(Normalized * 255.0f);
        }
        stbi_write_png(Path.c_str(), Field.Width, Field.Height, 1, Pixels.data(), Field.Width);
    }

    void PrintUsage(const char* Argv0)
    {
        std::cout <<
            "Usage: " << Argv0 << " [options]\n"
            "  --config <path.json>      Load a pipeline config JSON file (see README.md for the schema)\n"
            "  --out-prefix <prefix>     Output file prefix (default: \"out\" -> out_height.png, out_biome.png, ...)\n"
            "  --image-width <N>         Output image width in pixels (default: 1024)\n"
            "  --image-height <N>        Output image height in pixels (default: 512)\n"
            "  --seed <N>                Override the pipeline's Seed\n"
            "  --help                    Show this message\n";
    }
}

int main(int argc, char** argv)
{
    nlohmann::json RootJson = nlohmann::json::object();
    std::string OutPrefix = "out";
    int32_t ImageWidth = 1024;
    int32_t ImageHeight = 512;
    bool bHasSeedOverride = false;
    int32_t SeedOverride = 0;

    for (int i = 1; i < argc; ++i)
    {
        const std::string Arg = argv[i];
        auto NextArg = [&](const char* Flag) -> std::string
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for " << Flag << "\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (Arg == "--help" || Arg == "-h") { PrintUsage(argv[0]); return 0; }
        else if (Arg == "--config")
        {
            const std::string Path = NextArg("--config");
            std::ifstream In(Path);
            if (!In)
            {
                std::cerr << "Couldn't open config file: " << Path << "\n";
                return 1;
            }
            In >> RootJson;
        }
        else if (Arg == "--out-prefix") OutPrefix = NextArg("--out-prefix");
        else if (Arg == "--image-width") ImageWidth = std::stoi(NextArg("--image-width"));
        else if (Arg == "--image-height") ImageHeight = std::stoi(NextArg("--image-height"));
        else if (Arg == "--seed") { SeedOverride = std::stoi(NextArg("--seed")); bHasSeedOverride = true; }
        else
        {
            std::cerr << "Unknown argument: " << Arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    TerrainPipeline Pipeline;
    if (RootJson.contains("GridWidth")) Pipeline.GridWidth = RootJson.at("GridWidth").get<int32_t>();
    if (RootJson.contains("GridHeight")) Pipeline.GridHeight = RootJson.at("GridHeight").get<int32_t>();
    if (RootJson.contains("Seed")) Pipeline.Context.Seed = RootJson.at("Seed").get<int32_t>();
    if (bHasSeedOverride) Pipeline.Context.Seed = SeedOverride;

    // Continent: always present (the base layer), defaults match ContinentLayer's own if the
    // config file omits the "Continent" key entirely.
    auto ContinentNodePtr = std::make_unique<ContinentNode>();
    if (RootJson.contains("Continent")) ApplyJsonToContinentConfig(RootJson.at("Continent"), ContinentNodePtr->Config);
    ContinentNode* ContinentNodeRaw = ContinentNodePtr.get();
    Pipeline.Nodes.push_back(std::move(ContinentNodePtr));

    if (RootJson.contains("Noise") && RootJson.at("Noise").is_array())
    {
        for (const nlohmann::json& NoiseJson : RootJson.at("Noise"))
        {
            auto Node = std::make_unique<NoiseNode>();
            ApplyJsonToNoiseConfig(NoiseJson, Node->Config);
            Pipeline.Nodes.push_back(std::move(Node));
        }
    }

    if (RootJson.contains("HeightAdjust"))
    {
        auto Node = std::make_unique<HeightAdjustNode>();
        ApplyJsonToHeightAdjustConfig(RootJson.at("HeightAdjust"), Node->Config);
        Pipeline.Nodes.push_back(std::move(Node));
    }

    if (RootJson.contains("HydraulicErosion"))
    {
        auto Node = std::make_unique<HydraulicErosionNode>();
        ApplyJsonToHydraulicErosionConfig(RootJson.at("HydraulicErosion"), Node->Config);
        Pipeline.Nodes.push_back(std::move(Node));
    }

    if (RootJson.contains("ThermalErosion"))
    {
        auto Node = std::make_unique<ThermalErosionNode>();
        ApplyJsonToThermalErosionConfig(RootJson.at("ThermalErosion"), Node->Config);
        Pipeline.Nodes.push_back(std::move(Node));
    }

    int32_t AdvanceSteps = RootJson.contains("AdvanceSteps") ? RootJson.at("AdvanceSteps").get<int32_t>() : 0;
    float AdvanceMyr = RootJson.contains("AdvanceMyr") ? RootJson.at("AdvanceMyr").get<float>() : ContinentNodeRaw->Config.MillionYearsPerStep;
    if (AdvanceSteps > 0)
    {
        if (ContinentNodeRaw->Config.Algorithm != EContinentAlgorithm::PlateTectonics)
        {
            std::cerr << "Warning: AdvanceSteps ignored - Continent.Algorithm is not PlateTectonics.\n";
        }
        else
        {
            ContinentNodeRaw->Layer.Config = ContinentNodeRaw->Config;
            ContinentNodeRaw->Layer.Config.Seed = Pipeline.Context.Seed;
            ContinentNodeRaw->Layer.AdvanceTectonicSimulation(AdvanceSteps, AdvanceMyr);
        }
    }

    const FHeightField Terrain = Pipeline.Bake();

    FClimateConfig ClimateConfig;
    if (RootJson.contains("Climate")) ApplyJsonToClimateConfig(RootJson.at("Climate"), ClimateConfig);
    const FClimateField Climate = SimulateClimate(Terrain, ClimateConfig);

    FBiomeConfig BiomeConfig;
    if (RootJson.contains("Biome")) ApplyJsonToBiomeConfig(RootJson.at("Biome"), BiomeConfig);
    const FBiomeField Biomes = ClassifyBiomes(Terrain, Climate, BiomeConfig);

    // Resample everything onto the requested output image resolution (the internal bake grid and
    // the output image resolution are independent - GridWidth/Height is how finely erosion/climate
    // actually simulate, ImageWidth/Height is just the PNG's pixel size).
    const FLatLongGrid ImageGrid(ImageWidth, ImageHeight);
    const FLatLongGrid BakeGrid(Terrain.Width, Terrain.Height);

    FHeightField HeightImage(ImageWidth, ImageHeight);
    std::vector<uint8_t> BiomePixels((size_t)ImageWidth * ImageHeight * 3);
    FHeightField TempImage(ImageWidth, ImageHeight);
    FHeightField MoistureImage(ImageWidth, ImageHeight);

    float MinHeight = 1e9f, MaxHeight = -1e9f;
    ImageGrid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double)
    {
        const FLatLongGrid::FBilinearCell Cell = BakeGrid.ComputeBilinearCell(Dir);
        const float H = FLatLongGrid::SampleAtCell(Terrain.HeightMeters, Terrain.Width, Cell);
        HeightImage.HeightMeters[Idx] = H;
        MinHeight = std::min(MinHeight, H);
        MaxHeight = std::max(MaxHeight, H);

        // Nearest-neighbor for biome ID (it's a discrete category, not something to blend) and
        // bilinear for the continuous climate fields.
        const int32_t NearestX = Cell.Tx < 0.5 ? Cell.X0 : Cell.X1;
        const int32_t NearestY = Cell.Ty < 0.5 ? Cell.Y0 : Cell.Y1;
        const EBiome Biome = (EBiome)Biomes.BiomeId[(size_t)NearestY * Terrain.Width + NearestX];
        const FBiomeColor Color = GetBiomeColor(Biome);
        BiomePixels[(size_t)Idx * 3 + 0] = Color.R;
        BiomePixels[(size_t)Idx * 3 + 1] = Color.G;
        BiomePixels[(size_t)Idx * 3 + 2] = Color.B;

        TempImage.HeightMeters[Idx] = FLatLongGrid::SampleAtCell(Climate.TemperatureC, Terrain.Width, Cell);
        MoistureImage.HeightMeters[Idx] = FLatLongGrid::SampleAtCell(Climate.MoistureNormalized, Terrain.Width, Cell);
    });

    const std::string HeightPath = OutPrefix + "_height.png";
    const std::string BiomePath = OutPrefix + "_biome.png";
    const std::string TempPath = OutPrefix + "_temperature.png";
    const std::string MoisturePath = OutPrefix + "_moisture.png";

    WriteSeaLevelSplitPng(HeightPath, HeightImage, MinHeight, MaxHeight);
    stbi_write_png(BiomePath.c_str(), ImageWidth, ImageHeight, 3, BiomePixels.data(), ImageWidth * 3);
    WriteGrayscalePng(TempPath, TempImage, ClimateConfig.PoleTempC, ClimateConfig.EquatorTempC);
    WriteGrayscalePng(MoisturePath, MoistureImage, 0.0f, 1.0f);

    // Diagnostic land-coverage/biome-mix sample, independent of ImageWidth/Height.
    int32_t BiomeCounts[(size_t)EBiome::Count] = { 0 };
    for (uint8_t B : Biomes.BiomeId) { ++BiomeCounts[B]; }
    const double TotalCells = (double)Biomes.BiomeId.size();

    const FContinentStatus Status = ContinentNodeRaw->Layer.GetStatus();
    std::cout << "SolarOrbz Standalone Terrain Pipeline\n"
        << "  Algorithm:          " << AlgorithmToString(ContinentNodeRaw->Config.Algorithm) << "\n"
        << "  Seed:               " << Pipeline.Context.Seed << "\n"
        << "  Pipeline nodes:     ";
    for (size_t i = 0; i < Pipeline.Nodes.size(); ++i)
    {
        std::cout << Pipeline.Nodes[i]->TypeName() << (i + 1 < Pipeline.Nodes.size() ? " -> " : "\n");
    }
    if (ContinentNodeRaw->Config.Algorithm == EContinentAlgorithm::PlateTectonics)
    {
        std::cout << "  Simulated age:      " << Status.SimulatedAgeMyr << " Myr\n"
            << "  Plate count:        " << Status.CurrentPlateCount << "\n";
    }
    std::cout << "  Height range:       " << MinHeight << "m..." << MaxHeight << "m\n"
        << "  Biome mix:\n";
    for (int32_t i = 0; i < (int32_t)EBiome::Count; ++i)
    {
        if (BiomeCounts[i] == 0) continue;
        std::cout << "    " << GetBiomeName((EBiome)i) << ": " << (100.0 * BiomeCounts[i] / TotalCells) << "%\n";
    }
    std::cout << "  Wrote:              " << HeightPath << ", " << BiomePath << ", " << TempPath << ", " << MoisturePath << "\n";

    return 0;
}
