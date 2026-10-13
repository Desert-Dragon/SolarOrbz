// CLI frontend: config (JSON) in, heightmap PNG + stats out. No UE dependency - see
// src/core/ContinentLayer.h for the full scope-cut/fidelity notes (RNG, noise, mesh).
#include "ContinentLayer.h"
#include "LatLongGrid.h"
#include <nlohmann/json.hpp>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

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

    // Every field is named identically to FContinentConfig's own member - mirrors the UE plugin's
    // UPROPERTY names (see ContinentLayer.h) so a config file reads like the plugin's own Details panel.
    void ApplyJsonToConfig(const nlohmann::json& J, FContinentConfig& Config)
    {
        if (J.contains("Seed")) Config.Seed = J.at("Seed").get<int32_t>();
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

    void PrintUsage(const char* Argv0)
    {
        std::cout <<
            "Usage: " << Argv0 << " [options]\n"
            "  --config <path.json>      Load a FContinentConfig JSON file (overlays onto defaults)\n"
            "  --out <path.png>          Output heightmap PNG path (default: heightmap.png)\n"
            "  --image-width <N>         Output image width in pixels (default: 1024)\n"
            "  --image-height <N>        Output image height in pixels (default: 512)\n"
            "  --advance-steps <N>       Advance the tectonic simulation N steps before baking (Plate Tectonics only)\n"
            "  --advance-myr <F>         Million years per advance step (default: Config's MillionYearsPerStep)\n"
            "  --seed <N>                Override Config.Seed\n"
            "  --algorithm <name>        Override Config.Algorithm: RadialSeeds|VoronoiGrowth|PlateTectonics\n"
            "  --help                    Show this message\n";
    }
}

int main(int argc, char** argv)
{
    FContinentConfig Config;
    std::string OutPath = "heightmap.png";
    int32_t ImageWidth = 1024;
    int32_t ImageHeight = 512;
    int32_t AdvanceSteps = 0;
    float AdvanceMyr = -1.0f;
    bool bHasSeedOverride = false;
    int32_t SeedOverride = 0;
    bool bHasAlgorithmOverride = false;
    std::string AlgorithmOverride;

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
            nlohmann::json J;
            In >> J;
            ApplyJsonToConfig(J, Config);
        }
        else if (Arg == "--out") OutPath = NextArg("--out");
        else if (Arg == "--image-width") ImageWidth = std::stoi(NextArg("--image-width"));
        else if (Arg == "--image-height") ImageHeight = std::stoi(NextArg("--image-height"));
        else if (Arg == "--advance-steps") AdvanceSteps = std::stoi(NextArg("--advance-steps"));
        else if (Arg == "--advance-myr") AdvanceMyr = std::stof(NextArg("--advance-myr"));
        else if (Arg == "--seed") { SeedOverride = std::stoi(NextArg("--seed")); bHasSeedOverride = true; }
        else if (Arg == "--algorithm") { AlgorithmOverride = NextArg("--algorithm"); bHasAlgorithmOverride = true; }
        else
        {
            std::cerr << "Unknown argument: " << Arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (bHasSeedOverride) Config.Seed = SeedOverride;
    if (bHasAlgorithmOverride) Config.Algorithm = AlgorithmFromString(AlgorithmOverride);
    if (AdvanceMyr < 0.0f) AdvanceMyr = Config.MillionYearsPerStep;

    ContinentLayer Layer;
    Layer.Config = Config;
    Layer.Bake();

    if (AdvanceSteps > 0)
    {
        if (Config.Algorithm != EContinentAlgorithm::PlateTectonics)
        {
            std::cerr << "Warning: --advance-steps ignored - Algorithm is not PlateTectonics.\n";
        }
        else
        {
            Layer.AdvanceTectonicSimulation(AdvanceSteps, AdvanceMyr);
            Layer.Bake(); // re-bake so the grid reflects the plates' new positions
        }
    }

    std::vector<uint8_t> Pixels((size_t)ImageWidth * ImageHeight);
    const FLatLongGrid ImageGrid(ImageWidth, ImageHeight);
    const float MinMeters = std::min({ Config.OceanFloorDepthMeters, Config.TrenchDepthMeters, Config.RiftDepthMeters });
    const float MaxMeters = std::max({ Config.LandPlateauHeightMeters, Config.MountainHeightMeters, Config.RidgeHeightMeters });
    const float Range = std::max(MaxMeters - MinMeters, 1.0f);

    ImageGrid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double)
    {
        const float HeightMeters = Layer.GetRawHeightMeters(Dir);
        const float Normalized = MathUtil::Clamp((HeightMeters - MinMeters) / Range, 0.0f, 1.0f);
        Pixels[Idx] = (uint8_t)std::lround(Normalized * 255.0f);
    });

    if (!stbi_write_png(OutPath.c_str(), ImageWidth, ImageHeight, 1, Pixels.data(), ImageWidth))
    {
        std::cerr << "Failed to write " << OutPath << "\n";
        return 1;
    }

    const FContinentStatus Status = Layer.GetStatus();
    std::cout << "SolarOrbz Standalone Continent Generator\n"
        << "  Algorithm:          " << AlgorithmToString(Config.Algorithm) << "\n"
        << "  Seed:               " << Config.Seed << "\n"
        << "  Land coverage:      " << Layer.EstimateLandCoveragePercent() << "%\n";
    if (Config.Algorithm == EContinentAlgorithm::PlateTectonics)
    {
        std::cout << "  Simulated age:      " << Status.SimulatedAgeMyr << " Myr\n"
            << "  Plate count:        " << Status.CurrentPlateCount << "\n";
    }
    std::cout << "  Heightmap written:  " << OutPath << " (" << ImageWidth << "x" << ImageHeight << ", grayscale, "
        << MinMeters << "m..." << MaxMeters << "m)\n";

    return 0;
}
