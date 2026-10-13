#include "ContinentLayer.h"
#include "LatLongGrid.h"
#include "Random.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace
{
    // Hard cap on live plates regardless of simulated splitting - matches the UE plugin's Num Plates
    // ClampMax, so a layout split up to this point reads no differently from one authored at this
    // count from the start.
    constexpr int32_t MaxSimulatedPlateCount = 64;

    // Internal grid resolution for UpdateBoundaryStress's adjacency detection - deliberately much
    // cheaper than BakeGridWidth/Height, since this runs once per simulation STEP rather than once
    // per regenerate.
    constexpr int32_t AdjacencyDetectionGridWidth = 48;
    constexpr int32_t AdjacencyDetectionGridHeight = 24;

    constexpr float KindaSmallNumber = 1.0e-4f;

    float FractalNoise3DWithOffset(const Vec3& Pos, int32_t Octaves, float Persistence, float Lacunarity, int32_t Seed)
    {
        return NoiseBasis::FractalNoise3D(Pos, Octaves, Persistence, Lacunarity, Seed);
    }
}

void ContinentLayer::Bake()
{
    CachedSeeds.clear();
    BakedHeightCm.clear();
    BakedGridWidth = 0;
    BakedGridHeight = 0;

    switch (Config.Algorithm)
    {
    case EContinentAlgorithm::VoronoiGrowth:
        BakeVoronoiGrowth();
        break;
    case EContinentAlgorithm::PlateTectonics:
        BakePlateTectonics();
        break;
    case EContinentAlgorithm::RadialSeeds:
    default:
        BakeRadialSeeds();
        break;
    }
}

void ContinentLayer::BakeRadialSeeds()
{
    FRandomStream Stream(Config.Seed);
    const double PolarRadiusRadians = MathUtil::DegreesToRadians(Config.PolarContinentRadiusDegrees);

    int32_t SeedIndex = 0;
    auto AddSeedAt = [&](const Vec3& Direction, double RadiusRadians) -> FContinentSeedData
    {
        FContinentSeedData S;
        S.Direction = Direction;
        S.RadiusRadians = RadiusRadians;
        S.NoiseOffset = Vec3(SeedIndex * 17.3, SeedIndex * 29.7, SeedIndex * 53.1);
        CachedSeeds.push_back(S);
        ++SeedIndex;
        return S;
    };
    auto AddSeed = [&](const Vec3& Direction, float MinRadiusDegrees, float MaxRadiusDegrees) -> FContinentSeedData
    {
        return AddSeedAt(Direction, MathUtil::DegreesToRadians(Stream.FRandRange(MinRadiusDegrees, MaxRadiusDegrees)));
    };

    auto OverlapsEnabledPole = [&](const FContinentSeedData& S)
    {
        if (Config.bHasNorthPolarContinent)
        {
            const double AngleToNorth = std::acos(MathUtil::Clamp(Vec3::Dot(S.Direction, Vec3::Up()), -1.0, 1.0));
            if (AngleToNorth < S.RadiusRadians + PolarRadiusRadians)
            {
                return true;
            }
        }
        if (Config.bHasSouthPolarContinent)
        {
            const double AngleToSouth = std::acos(MathUtil::Clamp(Vec3::Dot(S.Direction, -Vec3::Up()), -1.0, 1.0));
            if (AngleToSouth < S.RadiusRadians + PolarRadiusRadians)
            {
                return true;
            }
        }
        return false;
    };

    auto AddSubSeeds = [&](const FContinentSeedData& Parent)
    {
        for (int32_t i = 0; i < Config.SubSeedsPerLandmass; ++i)
        {
            const Vec3 RandomPoint = RandomPointOnUnitSphere(Stream);
            Vec3 Tangent = RandomPoint - Parent.Direction * Vec3::Dot(RandomPoint, Parent.Direction);
            if (Tangent.IsNearlyZero())
            {
                continue;
            }
            Tangent = Tangent.GetSafeNormal();

            const double OffsetRadians = Stream.FRandRange(0.0f, Config.SubSeedScatterFraction) * Parent.RadiusRadians;
            const Vec3 SubDirection = (Parent.Direction * std::cos(OffsetRadians) + Tangent * std::sin(OffsetRadians)).GetSafeNormal();
            const double SubRadiusRadians = Parent.RadiusRadians * Stream.FRandRange(Config.MinSubSeedRadiusFraction, Config.MaxSubSeedRadiusFraction);

            const FContinentSeedData SubSeed = AddSeedAt(SubDirection, SubRadiusRadians);
            if (OverlapsEnabledPole(SubSeed))
            {
                CachedSeeds.pop_back();
                --SeedIndex;
            }
        }
    };

    constexpr int32_t MaxPlacementAttempts = 32;
    auto AddRandomSeedClearOfPoles = [&](float MinRadiusDegrees, float MaxRadiusDegrees)
    {
        for (int32_t Attempt = 0; Attempt < MaxPlacementAttempts; ++Attempt)
        {
            const Vec3 Direction = RandomPointOnUnitSphere(Stream);
            const FContinentSeedData Placed = AddSeed(Direction, MinRadiusDegrees, MaxRadiusDegrees);
            if (!OverlapsEnabledPole(Placed))
            {
                AddSubSeeds(Placed);
                return;
            }
            if (Attempt + 1 == MaxPlacementAttempts)
            {
                AddSubSeeds(Placed);
                return;
            }
            CachedSeeds.pop_back();
            --SeedIndex;
        }
    };

    for (int32_t i = 0; i < Config.NumContinents; ++i)
    {
        AddRandomSeedClearOfPoles(Config.MinContinentRadiusDegrees, Config.MaxContinentRadiusDegrees);
    }
    for (int32_t i = 0; i < Config.NumIslands; ++i)
    {
        AddRandomSeedClearOfPoles(Config.MinIslandRadiusDegrees, Config.MaxIslandRadiusDegrees);
    }
    if (Config.bHasNorthPolarContinent)
    {
        AddSeed(Vec3(0.0, 0.0, 1.0), Config.PolarContinentRadiusDegrees, Config.PolarContinentRadiusDegrees);
    }
    if (Config.bHasSouthPolarContinent)
    {
        AddSeed(Vec3(0.0, 0.0, -1.0), Config.PolarContinentRadiusDegrees, Config.PolarContinentRadiusDegrees);
    }
}

void ContinentLayer::BakeVoronoiGrowth()
{
    const int32_t GridW = std::max(Config.BakeGridWidth, 8);
    const int32_t GridH = std::max(Config.BakeGridHeight, 4);
    BakedGridWidth = GridW;
    BakedGridHeight = GridH;
    const int32_t NumCells = GridW * GridH;
    const FLatLongGrid Grid(GridW, GridH);

    std::vector<Vec3> CellDirections(NumCells);
    Grid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double) { CellDirections[Idx] = Dir; });

    FRandomStream Stream(Config.Seed);

    std::vector<int32_t> OwnerId(NumCells, -1);
    std::vector<float> OwnerEnergyRemaining(NumCells, 0.0f);
    std::vector<float> OwnerStartEnergy;

    auto GrowLandmass = [&](int32_t StartIdx, float StopThreshold) -> bool
    {
        if (OwnerId[StartIdx] != -1)
        {
            return false;
        }

        OwnerStartEnergy.push_back(1.0f);
        const int32_t OwnerTag = (int32_t)OwnerStartEnergy.size() - 1;
        OwnerId[StartIdx] = OwnerTag;
        OwnerEnergyRemaining[StartIdx] = 1.0f;

        struct FFrontierEntry { int32_t Idx; float Energy; };
        std::vector<FFrontierEntry> Frontier;
        Frontier.push_back({ StartIdx, 1.0f });

        while (!Frontier.empty())
        {
            const int32_t PickIndex = Stream.RandRange(0, (int32_t)Frontier.size() - 1);
            const FFrontierEntry Current = Frontier[PickIndex];
            Frontier[PickIndex] = Frontier.back();
            Frontier.pop_back();

            int32_t Neighbors[4];
            GetGridNeighbors4(Current.Idx, GridW, GridH, Neighbors);
            for (int32_t NeighborIdx : Neighbors)
            {
                if (OwnerId[NeighborIdx] != -1)
                {
                    continue;
                }

                const float Decay = MathUtil::Clamp(Stream.FRandRange(Config.GrowthDecayMin, Config.GrowthDecayMax) + Stream.FRandRange(-Config.GrowthJitter, Config.GrowthJitter), 0.0f, 1.0f);
                const float NextEnergy = Current.Energy * Decay;
                if (NextEnergy <= StopThreshold)
                {
                    continue;
                }

                OwnerId[NeighborIdx] = OwnerTag;
                OwnerEnergyRemaining[NeighborIdx] = NextEnergy;
                Frontier.push_back({ NeighborIdx, NextEnergy });
            }
        }
        return true;
    };

    const float DegreesPerHop = 0.5f * (360.0f / GridW + 180.0f / std::max(GridH - 1, 1));
    const float AverageDecay = MathUtil::Clamp((Config.GrowthDecayMin + Config.GrowthDecayMax) * 0.5f, 0.01f, 0.999f);
    auto StopThresholdForRadius = [&](float RadiusDegrees)
    {
        const float NumHops = std::max(RadiusDegrees / DegreesPerHop, 1.0f);
        return std::pow(AverageDecay, NumHops);
    };

    constexpr int32_t MaxPlacementAttempts = 16;
    auto GrowRandomLandmass = [&](float MinRadiusDegrees, float MaxRadiusDegrees)
    {
        const float StopThreshold = StopThresholdForRadius(Stream.FRandRange(MinRadiusDegrees, MaxRadiusDegrees));
        for (int32_t Attempt = 0; Attempt < MaxPlacementAttempts; ++Attempt)
        {
            const int32_t StartIdx = DirectionToNearestCellIdx(Grid, RandomPointOnUnitSphere(Stream));
            if (GrowLandmass(StartIdx, StopThreshold))
            {
                return;
            }
        }
    };

    auto ClaimPolarCap = [&](const Vec3& PoleDirection)
    {
        const double PolarRadiusRadians = MathUtil::DegreesToRadians(Config.PolarContinentRadiusDegrees);
        OwnerStartEnergy.push_back(1.0f);
        const int32_t OwnerTag = (int32_t)OwnerStartEnergy.size() - 1;
        for (int32_t Idx = 0; Idx < NumCells; ++Idx)
        {
            if (OwnerId[Idx] != -1)
            {
                continue;
            }
            const double Angle = std::acos(MathUtil::Clamp(Vec3::Dot(CellDirections[Idx], PoleDirection), -1.0, 1.0));
            if (Angle <= PolarRadiusRadians)
            {
                OwnerId[Idx] = OwnerTag;
                OwnerEnergyRemaining[Idx] = (float)MathUtil::Clamp(1.0 - Angle / std::max(PolarRadiusRadians, (double)KindaSmallNumber), 0.0, 1.0);
            }
        }
    };

    if (Config.bHasNorthPolarContinent)
    {
        ClaimPolarCap(Vec3(0.0, 0.0, 1.0));
    }
    if (Config.bHasSouthPolarContinent)
    {
        ClaimPolarCap(Vec3(0.0, 0.0, -1.0));
    }

    for (int32_t i = 0; i < Config.NumContinents; ++i)
    {
        GrowRandomLandmass(Config.MinContinentRadiusDegrees, Config.MaxContinentRadiusDegrees);
    }
    for (int32_t i = 0; i < Config.NumIslands; ++i)
    {
        GrowRandomLandmass(Config.MinIslandRadiusDegrees, Config.MaxIslandRadiusDegrees);
    }

    BakedHeightCm.resize(NumCells);
    for (int32_t Idx = 0; Idx < NumCells; ++Idx)
    {
        float BaseMeters = Config.OceanFloorDepthMeters;
        if (OwnerId[Idx] != -1)
        {
            const float T = MathUtil::Clamp(OwnerEnergyRemaining[Idx] / std::max(OwnerStartEnergy[OwnerId[Idx]], KindaSmallNumber), 0.0f, 1.0f);
            const float Shaped = std::pow(T, std::max(Config.CoastlineSharpness, 0.01f));
            BaseMeters = (float)MathUtil::Lerp(Config.OceanFloorDepthMeters, Config.LandPlateauHeightMeters, Shaped);
        }

        const float NoiseMeters = Config.CoastlineNoiseAmplitude * (Config.LandPlateauHeightMeters - Config.OceanFloorDepthMeters) * 0.1f *
            NoiseBasis::SampleBasis(ENoiseType::Perlin, CellDirections[Idx] * Config.CoastlineNoiseFrequency, Config.Seed);

        BakedHeightCm[Idx] = (BaseMeters + NoiseMeters) * 100.0f;
    }
}

void ContinentLayer::BakePlateTectonics()
{
    const int32_t GridW = std::max(Config.BakeGridWidth, 8);
    const int32_t GridH = std::max(Config.BakeGridHeight, 4);
    BakedGridWidth = GridW;
    BakedGridHeight = GridH;
    const int32_t NumCells = GridW * GridH;
    const FLatLongGrid Grid(GridW, GridH);

    std::vector<Vec3> CellDirections(NumCells);
    Grid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double) { CellDirections[Idx] = Dir; });

    EnsurePlatesInitialized();
    const int32_t PlateCount = (int32_t)Plates.size();

    std::vector<int32_t> PlateId(NumCells);
    for (int32_t Idx = 0; Idx < NumCells; ++Idx)
    {
        int32_t BestPlate = 0;
        double BestDot = -2.0;
        for (int32_t P = 0; P < PlateCount; ++P)
        {
            const double Dot = Vec3::Dot(CellDirections[Idx], Plates[P].SeedDirection);
            if (Dot > BestDot)
            {
                BestDot = Dot;
                BestPlate = P;
            }
        }
        PlateId[Idx] = BestPlate;
    }

    constexpr int32_t PolarContinentalSentinel = -2;
    const double PolarRadiusRadians = MathUtil::DegreesToRadians(Config.PolarContinentRadiusDegrees);
    auto ApplyPolarOverride = [&](const Vec3& PoleDirection)
    {
        for (int32_t Idx = 0; Idx < NumCells; ++Idx)
        {
            const double Angle = std::acos(MathUtil::Clamp(Vec3::Dot(CellDirections[Idx], PoleDirection), -1.0, 1.0));
            if (Angle <= PolarRadiusRadians)
            {
                PlateId[Idx] = PolarContinentalSentinel;
            }
        }
    };
    if (Config.bHasNorthPolarContinent)
    {
        ApplyPolarOverride(Vec3(0.0, 0.0, 1.0));
    }
    if (Config.bHasSouthPolarContinent)
    {
        ApplyPolarOverride(Vec3(0.0, 0.0, -1.0));
    }

    auto IsOceanic = [this](int32_t Plate) { return Plate >= 0 && Plates[Plate].bOceanic; };
    auto IsContinental = [this](int32_t Plate) { return Plate == PolarContinentalSentinel || (Plate >= 0 && !Plates[Plate].bOceanic); };

    std::vector<int32_t> DistanceHops(NumCells, -1);
    std::vector<float> BoundaryModifierMeters(NumCells, 0.0f);

    const float DegreesPerHop = 0.5f * (360.0f / GridW + 180.0f / std::max(GridH - 1, 1));
    const int32_t CutoffHops = std::max((int32_t)std::lround(Config.BoundaryInfluenceDegrees / DegreesPerHop), 1);

    std::vector<int32_t> Frontier;
    for (int32_t Idx = 0; Idx < NumCells; ++Idx)
    {
        int32_t Neighbors[4];
        GetGridNeighbors4(Idx, GridW, GridH, Neighbors);

        const int32_t PlateA = PlateId[Idx];
        float BestModifier = 0.0f;
        bool bIsBoundary = false;
        for (int32_t NeighborIdx : Neighbors)
        {
            const int32_t PlateB = PlateId[NeighborIdx];
            if (PlateB == PlateA)
            {
                continue;
            }
            bIsBoundary = true;

            const Vec3 SeedDirA = (PlateA >= 0) ? Plates[PlateA].SeedDirection : CellDirections[Idx];
            const Vec3 SeedDirB = (PlateB >= 0) ? Plates[PlateB].SeedDirection : CellDirections[NeighborIdx];
            const Vec3 BoundaryAxis = (SeedDirB - SeedDirA).GetSafeNormal();
            const Vec3 DriftA = (PlateA >= 0) ? Plates[PlateA].GetInstantaneousDrift() : Vec3::Zero();
            const Vec3 DriftB = (PlateB >= 0) ? Plates[PlateB].GetInstantaneousDrift() : Vec3::Zero();
            const double Closure = Vec3::Dot(DriftA, BoundaryAxis) - Vec3::Dot(DriftB, BoundaryAxis);

            constexpr double TransformThreshold = 0.25;
            float Modifier = 0.0f;
            if (Closure > TransformThreshold)
            {
                if (IsContinental(PlateA) && IsContinental(PlateB))
                {
                    Modifier = Config.MountainHeightMeters;
                }
                else if (IsOceanic(PlateA) && IsOceanic(PlateB))
                {
                    Modifier = Config.MountainHeightMeters * 0.3f;
                }
                else
                {
                    Modifier = IsOceanic(PlateA) ? Config.TrenchDepthMeters : Config.MountainHeightMeters * 0.5f;
                }
            }
            else if (Closure < -TransformThreshold)
            {
                Modifier = (IsOceanic(PlateA) && IsOceanic(PlateB)) ? Config.RidgeHeightMeters : Config.RiftDepthMeters;
            }

            if (Modifier != 0.0f && PlateA >= 0 && PlateB >= 0 && Config.MaxStressMultiplier > 0.0f)
            {
                if (const FTectonicBoundaryState* State = FindBoundaryState(Plates[PlateA].PlateId, Plates[PlateB].PlateId))
                {
                    const float RelevantMyr = (Closure > TransformThreshold) ? State->AccumulatedConvergenceMyr
                        : (Closure < -TransformThreshold ? State->AccumulatedDivergenceMyr : 0.0f);
                    const float StressMultiplier = 1.0f + MathUtil::Clamp(RelevantMyr / Config.StressMatureTimescaleMyr, 0.0f, Config.MaxStressMultiplier);
                    Modifier *= StressMultiplier;
                }
            }

            if (std::abs(Modifier) > std::abs(BestModifier))
            {
                BestModifier = Modifier;
            }
        }

        if (bIsBoundary)
        {
            DistanceHops[Idx] = 0;
            BoundaryModifierMeters[Idx] = BestModifier;
            Frontier.push_back(Idx);
        }
    }

    for (int32_t Hop = 0; Hop < CutoffHops && !Frontier.empty(); ++Hop)
    {
        std::vector<int32_t> NextFrontier;
        for (int32_t Idx : Frontier)
        {
            int32_t Neighbors[4];
            GetGridNeighbors4(Idx, GridW, GridH, Neighbors);
            for (int32_t NeighborIdx : Neighbors)
            {
                if (DistanceHops[NeighborIdx] != -1)
                {
                    continue;
                }
                DistanceHops[NeighborIdx] = Hop + 1;
                BoundaryModifierMeters[NeighborIdx] = BoundaryModifierMeters[Idx];
                NextFrontier.push_back(NeighborIdx);
            }
        }
        Frontier = std::move(NextFrontier);
    }

    BakedHeightCm.resize(NumCells);
    for (int32_t Idx = 0; Idx < NumCells; ++Idx)
    {
        const float BaseMeters = IsOceanic(PlateId[Idx]) ? Config.OceanFloorDepthMeters : Config.LandPlateauHeightMeters;

        float ModifierMeters = 0.0f;
        if (DistanceHops[Idx] != -1)
        {
            const double Falloff = 1.0 - MathUtil::SmoothStep(0.0, (double)CutoffHops, (double)DistanceHops[Idx]);
            ModifierMeters = (float)(BoundaryModifierMeters[Idx] * Falloff);
        }

        const float NoiseMeters = Config.CoastlineNoiseAmplitude * (Config.LandPlateauHeightMeters - Config.OceanFloorDepthMeters) * 0.1f *
            NoiseBasis::SampleBasis(ENoiseType::Perlin, CellDirections[Idx] * Config.CoastlineNoiseFrequency, Config.Seed);

        BakedHeightCm[Idx] = (BaseMeters + ModifierMeters + NoiseMeters) * 100.0f;
    }
}

void ContinentLayer::EnsurePlatesInitialized()
{
    const int32_t PlateCount = std::max(Config.NumPlates, 2);
    if (bPlatesEverSeeded && PlatesSeededFrom == Config.Seed && PlatesSeededFromNumPlates == PlateCount)
    {
        return;
    }

    FRandomStream Stream(Config.Seed);
    Plates.assign(PlateCount, FTectonicPlate());
    NextPlateId = 0;
    for (int32_t P = 0; P < PlateCount; ++P)
    {
        FTectonicPlate& Plate = Plates[P];
        Plate.PlateId = NextPlateId++;
        Plate.SeedDirection = RandomPointOnUnitSphere(Stream);
        Plate.RotationAxis = RandomPointOnUnitSphere(Stream);
        Plate.AngularRateRadPerMyr = (float)MathUtil::DegreesToRadians(Stream.FRandRange(0.1f, 1.0f) * Config.InitialDriftSpeedDegreesPerMyr);
        Plate.bOceanic = Stream.FRand() < Config.OceanicPlateFraction;
        Plate.AgeMyr = 0.0f;
    }
    BoundaryStates.clear();

    PlatesSeededFrom = Config.Seed;
    PlatesSeededFromNumPlates = PlateCount;
    bPlatesEverSeeded = true;
    Status.SimulatedAgeMyr = 0.0f;
    Status.CurrentPlateCount = (int32_t)Plates.size();
}

void ContinentLayer::AdvanceTectonicSimulation(int32_t NumSteps, float DeltaMyr)
{
    if (Config.Algorithm != EContinentAlgorithm::PlateTectonics)
    {
        return;
    }

    EnsurePlatesInitialized();

    NumSteps = std::max(NumSteps, 0);
    DeltaMyr = std::max(DeltaMyr, 0.0f);

    for (int32_t Step = 0; Step < NumSteps; ++Step)
    {
        for (FTectonicPlate& Plate : Plates)
        {
            Plate.SeedDirection = MathUtil::RotateAroundAxis(Plate.SeedDirection, Plate.RotationAxis, Plate.AngularRateRadPerMyr * DeltaMyr).GetSafeNormal();
            Plate.AgeMyr += DeltaMyr;
        }
        Status.SimulatedAgeMyr += DeltaMyr;

        UpdateBoundaryStress(DeltaMyr);
    }

    Status.CurrentPlateCount = (int32_t)Plates.size();
}

void ContinentLayer::UpdateBoundaryStress(float DeltaMyr)
{
    const int32_t GridW = AdjacencyDetectionGridWidth;
    const int32_t GridH = AdjacencyDetectionGridHeight;
    const int32_t NumCells = GridW * GridH;
    const FLatLongGrid Grid(GridW, GridH);

    std::vector<Vec3> CellDirections(NumCells);
    Grid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double) { CellDirections[Idx] = Dir; });

    const int32_t PlateCount = (int32_t)Plates.size();
    std::vector<int32_t> PlateIdx(NumCells);
    for (int32_t Idx = 0; Idx < NumCells; ++Idx)
    {
        int32_t BestPlate = 0;
        double BestDot = -2.0;
        for (int32_t P = 0; P < PlateCount; ++P)
        {
            const double Dot = Vec3::Dot(CellDirections[Idx], Plates[P].SeedDirection);
            if (Dot > BestDot)
            {
                BestDot = Dot;
                BestPlate = P;
            }
        }
        PlateIdx[Idx] = BestPlate;
    }

    std::set<int32_t> LivePlateIds;
    for (const FTectonicPlate& Plate : Plates)
    {
        LivePlateIds.insert(Plate.PlateId);
    }
    BoundaryStates.erase(std::remove_if(BoundaryStates.begin(), BoundaryStates.end(),
        [&LivePlateIds](const FTectonicBoundaryState& State)
        {
            return LivePlateIds.find(State.PlateIdA) == LivePlateIds.end() || LivePlateIds.find(State.PlateIdB) == LivePlateIds.end();
        }), BoundaryStates.end());

    constexpr double TransformThreshold = 0.25;
    std::set<int64_t> SeenThisStep;
    std::map<int32_t, int32_t> PendingSplitsByPlateId;

    for (int32_t Idx = 0; Idx < NumCells; ++Idx)
    {
        int32_t Neighbors[4];
        GetGridNeighbors4(Idx, GridW, GridH, Neighbors);

        const int32_t PlateA = PlateIdx[Idx];
        for (int32_t NeighborIdx : Neighbors)
        {
            const int32_t PlateB = PlateIdx[NeighborIdx];
            if (PlateB == PlateA)
            {
                continue;
            }

            const int32_t IdA = Plates[PlateA].PlateId;
            const int32_t IdB = Plates[PlateB].PlateId;
            const int32_t KeyLo = std::min(IdA, IdB);
            const int32_t KeyHi = std::max(IdA, IdB);
            const int64_t PairKey = (int64_t(KeyLo) << 32) | (uint32_t)KeyHi;
            if (SeenThisStep.find(PairKey) != SeenThisStep.end())
            {
                continue;
            }
            SeenThisStep.insert(PairKey);

            const Vec3 BoundaryAxis = (Plates[PlateB].SeedDirection - Plates[PlateA].SeedDirection).GetSafeNormal();
            const double Closure = Vec3::Dot(Plates[PlateA].GetInstantaneousDrift(), BoundaryAxis)
                                  - Vec3::Dot(Plates[PlateB].GetInstantaneousDrift(), BoundaryAxis);

            FTectonicBoundaryState& State = FindOrAddBoundaryState(IdA, IdB);
            if (Closure > TransformThreshold)
            {
                State.AccumulatedConvergenceMyr = std::min(State.AccumulatedConvergenceMyr + DeltaMyr, 1.0e6f);
            }
            else if (Closure < -TransformThreshold)
            {
                State.AccumulatedDivergenceMyr = std::min(State.AccumulatedDivergenceMyr + DeltaMyr, 1.0e6f);

                if (Config.PlateSplitThresholdMyr > 0.0f && State.AccumulatedDivergenceMyr >= Config.PlateSplitThresholdMyr
                    && (int32_t)Plates.size() < MaxSimulatedPlateCount)
                {
                    int32_t CountA = 0, CountB = 0;
                    for (int32_t P : PlateIdx)
                    {
                        if (P == PlateA) ++CountA;
                        else if (P == PlateB) ++CountB;
                    }
                    const bool bSplitA = CountA >= CountB;
                    PendingSplitsByPlateId[bSplitA ? IdA : IdB] = bSplitA ? IdB : IdA;
                }
            }
        }
    }

    for (const auto& SplitRequest : PendingSplitsByPlateId)
    {
        SplitPlate(SplitRequest.first, SplitRequest.second);
    }
}

void ContinentLayer::SplitPlate(int32_t PlateIdToSplit, int32_t OtherPlateId)
{
    if ((int32_t)Plates.size() >= MaxSimulatedPlateCount)
    {
        return;
    }

    const auto SplitIt = std::find_if(Plates.begin(), Plates.end(), [PlateIdToSplit](const FTectonicPlate& P) { return P.PlateId == PlateIdToSplit; });
    const auto OtherIt = std::find_if(Plates.begin(), Plates.end(), [OtherPlateId](const FTectonicPlate& P) { return P.PlateId == OtherPlateId; });
    if (SplitIt == Plates.end() || OtherIt == Plates.end())
    {
        return;
    }

    const FTectonicPlate Parent = *SplitIt;
    const Vec3& OriginalSeed = Parent.SeedDirection;
    const Vec3 SeparationDirection = (OtherIt->SeedDirection - OriginalSeed).GetSafeNormal();
    const Vec3 SeparationTangent = (SeparationDirection - OriginalSeed * Vec3::Dot(SeparationDirection, OriginalSeed)).GetSafeNormal();

    constexpr double OffsetAngleRadians = MathUtil::PI_D / 18.0; // 10 degrees

    FTectonicPlate ChildA = Parent;
    ChildA.PlateId = NextPlateId++;
    ChildA.SeedDirection = (OriginalSeed * std::cos(OffsetAngleRadians) + SeparationTangent * std::sin(OffsetAngleRadians)).GetSafeNormal();
    ChildA.RotationAxis = Vec3::Cross(ChildA.SeedDirection, SeparationTangent).GetSafeNormal();
    ChildA.AgeMyr = 0.0f;

    FTectonicPlate ChildB = Parent;
    ChildB.PlateId = NextPlateId++;
    ChildB.SeedDirection = (OriginalSeed * std::cos(OffsetAngleRadians) - SeparationTangent * std::sin(OffsetAngleRadians)).GetSafeNormal();
    ChildB.RotationAxis = Vec3::Cross(ChildB.SeedDirection, -1.0 * SeparationTangent).GetSafeNormal();
    ChildB.AgeMyr = 0.0f;

    Plates.erase(SplitIt);
    Plates.push_back(ChildA);
    Plates.push_back(ChildB);

    BoundaryStates.erase(std::remove_if(BoundaryStates.begin(), BoundaryStates.end(),
        [PlateIdToSplit](const FTectonicBoundaryState& State)
        {
            return State.PlateIdA == PlateIdToSplit || State.PlateIdB == PlateIdToSplit;
        }), BoundaryStates.end());
}

FTectonicBoundaryState& ContinentLayer::FindOrAddBoundaryState(int32_t PlateIdA, int32_t PlateIdB)
{
    const int32_t KeyLo = std::min(PlateIdA, PlateIdB);
    const int32_t KeyHi = std::max(PlateIdA, PlateIdB);
    for (FTectonicBoundaryState& State : BoundaryStates)
    {
        if (State.PlateIdA == KeyLo && State.PlateIdB == KeyHi)
        {
            return State;
        }
    }

    FTectonicBoundaryState NewState;
    NewState.PlateIdA = KeyLo;
    NewState.PlateIdB = KeyHi;
    BoundaryStates.push_back(NewState);
    return BoundaryStates.back();
}

const FTectonicBoundaryState* ContinentLayer::FindBoundaryState(int32_t PlateIdA, int32_t PlateIdB) const
{
    const int32_t KeyLo = std::min(PlateIdA, PlateIdB);
    const int32_t KeyHi = std::max(PlateIdA, PlateIdB);
    for (const FTectonicBoundaryState& State : BoundaryStates)
    {
        if (State.PlateIdA == KeyLo && State.PlateIdB == KeyHi)
        {
            return &State;
        }
    }
    return nullptr;
}

float ContinentLayer::GetRawHeightMeters(const Vec3& UnitDirection) const
{
    switch (Config.Algorithm)
    {
    case EContinentAlgorithm::VoronoiGrowth:
    case EContinentAlgorithm::PlateTectonics:
        return GetRawHeightFromBakedGrid(UnitDirection);
    case EContinentAlgorithm::RadialSeeds:
    default:
        return GetRawHeightRadialSeeds(UnitDirection);
    }
}

float ContinentLayer::GetRawHeightRadialSeeds(const Vec3& UnitDirection) const
{
    if (CachedSeeds.empty())
    {
        return Config.OceanFloorDepthMeters;
    }

    Vec3 WarpedDirection = UnitDirection;
    if (Config.CoastlineWarpStrength > 0.0f)
    {
        const Vec3 WarpPos = UnitDirection * Config.CoastlineWarpFrequency;
        const Vec3 Warp(
            NoiseBasis::PerlinNoise3D(WarpPos),
            NoiseBasis::PerlinNoise3D(WarpPos + Vec3(31.7, 0.0, 0.0)),
            NoiseBasis::PerlinNoise3D(WarpPos + Vec3(0.0, 57.3, 0.0)));
        WarpedDirection = (UnitDirection + Warp * Config.CoastlineWarpStrength).GetSafeNormal();
    }

    float MaxInfluence = 0.0f;
    for (const FContinentSeedData& S : CachedSeeds)
    {
        const double CosAngle = Vec3::Dot(WarpedDirection, S.Direction);
        const double Angle = std::acos(MathUtil::Clamp(CosAngle, -1.0, 1.0));

        const float CoastlineNoise = FractalNoise3DWithOffset(WarpedDirection * Config.CoastlineNoiseFrequency + S.NoiseOffset, Config.CoastlineNoiseOctaves, Config.CoastlineNoisePersistence, Config.CoastlineNoiseLacunarity, Config.Seed);
        const double PerturbedRadius = std::max(S.RadiusRadians * (1.0 + Config.CoastlineNoiseAmplitude * CoastlineNoise), 0.0);

        const double Influence = 1.0 - MathUtil::SmoothStep(0.0, std::max(PerturbedRadius, (double)KindaSmallNumber), Angle);
        MaxInfluence = std::max(MaxInfluence, (float)Influence);
    }

    MaxInfluence = std::pow(MathUtil::Clamp(MaxInfluence, 0.0f, 1.0f), std::max(Config.CoastlineSharpness, 0.01f));

    return (float)MathUtil::Lerp(Config.OceanFloorDepthMeters, Config.LandPlateauHeightMeters, MaxInfluence);
}

float ContinentLayer::GetRawHeightFromBakedGrid(const Vec3& UnitDirection) const
{
    if (BakedHeightCm.empty() || BakedGridWidth <= 0 || BakedGridHeight <= 0)
    {
        return Config.OceanFloorDepthMeters;
    }

    return FLatLongGrid(BakedGridWidth, BakedGridHeight).SampleBilinear(BakedHeightCm, UnitDirection) / 100.0f;
}

float ContinentLayer::EstimateLandCoveragePercent() const
{
    constexpr int32_t CoverageSamplesW = 64, CoverageSamplesH = 32;
    int32_t LandSamples = 0;
    FLatLongGrid(CoverageSamplesW, CoverageSamplesH).ForEachCell([&](int32_t, const Vec3& Dir, double, double)
    {
        if (GetRawHeightMeters(Dir) > 0.0f)
        {
            ++LandSamples;
        }
    });
    return 100.0f * LandSamples / (float)(CoverageSamplesW * CoverageSamplesH);
}
