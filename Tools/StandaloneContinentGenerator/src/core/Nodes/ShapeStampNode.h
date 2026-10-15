// Hand-placed terrain "stamps" - the standalone tool's equivalent of World Creator's Shape Layers
// (each one an archetype - Mountain/Volcano/Crater/Canyon/... - placed at an explicit position with
// its own size and a nested noise/fractal overlay, verified against that app's real "Landscape
// Settings" panel: Shape Type dropdown, Base/Placement/Noise/Fractal sections). Scope-cut to 4
// archetypes covering the structurally distinct cases WC's own archetype list falls into - a single
// radial dome (Mountain), a dome with a central dip (Volcano), a dip-plus-rim with no cone (Crater),
// and the one directional/linear case (Canyon) standing in for WC's whole Cliff/Cracks/Furrows
// family. More archetypes (Dunes, Island, Alpine, ...) are the same per-archetype profile-function
// pattern added to ApplyStamp's switch - not a framework change.
//
// Placed by explicit Latitude/Longitude (not grown from a random seed direction like Continent's
// own radial seeds) - matches WC's own numeric, hand-set Offset X/Offset Z fields: a Shape Layer is
// something an author positions deliberately, not something that scatters.
#pragma once

#include "../TerrainNode.h"
#include "../NoiseBasis.h"
#include "../LatLongGrid.h"
#include <vector>

enum class EShapeStampArchetype : int32_t
{
    Mountain,
    Volcano,
    Crater,
    Canyon
};

struct FShapeStamp
{
    bool bEnabled = true;
    EShapeStampArchetype Archetype = EShapeStampArchetype::Mountain;

    // Placement. RadiusDegrees is the stamp's angular footprint for every archetype (half-width for
    // Canyon's across-axis too); LengthDegrees/RotationDegrees only matter for Canyon (ignored,
    // harmlessly, by the radial archetypes) - same "not every field applies to every archetype"
    // shape WC's own panel has (Rotation/Offset only meaningfully vary for its directional shapes).
    float LatitudeDegrees = 0.0f;
    float LongitudeDegrees = 0.0f;
    float RadiusDegrees = 10.0f;
    float LengthDegrees = 20.0f;   // Canyon only - half-length along its axis
    float RotationDegrees = 0.0f; // Canyon only - axis heading, 0 = local north at Latitude/Longitude

    float HeightMeters = 1500.0f;      // base-shape amplitude (Canyon always carves down regardless of sign)
    float CraterDepthFraction = 0.4f;  // Volcano/Crater only - how pronounced the central bowl/rim is, 0..~1

    // Nested noise overlay (WC's own "Noise"/"Fractal" sections under a Shape Layer) - masked by the
    // same footprint envelope as the base shape so detail doesn't bleed past the stamp's edge.
    ENoiseType NoiseType = ENoiseType::Perlin;
    float NoiseStrengthMeters = 200.0f;
    float NoiseFrequency = 4.0f;
    int32_t NoiseOctaves = 4;
    float NoisePersistence = 0.5f;
    float NoiseLacunarity = 2.0f;
};

class ShapeStampNode : public ITerrainNode
{
public:
    std::vector<FShapeStamp> Stamps;

    const char* TypeName() const override { return "Shape Stamp"; }

    void Apply(FHeightField& Field, const FPipelineContext& Ctx) override
    {
        const FLatLongGrid Grid(Field.Width, Field.Height);
        for (size_t StampIndex = 0; StampIndex < Stamps.size(); ++StampIndex)
        {
            const FShapeStamp& Stamp = Stamps[StampIndex];
            if (!Stamp.bEnabled)
            {
                continue;
            }
            // Offsetting by index keeps several stamps of the same archetype from sampling identical
            // noise - same reasoning as NoiseNode's own SeedOffset.
            ApplyStamp(Field, Grid, Stamp, Ctx.Seed + (int32_t)StampIndex * 10007);
        }
    }

private:
    static void ApplyStamp(FHeightField& Field, const FLatLongGrid& Grid, const FShapeStamp& Stamp, int32_t EffectiveSeed)
    {
        const Vec3 Center = DirectionFromLatLongDegrees(Stamp.LatitudeDegrees, Stamp.LongitudeDegrees);

        // Local tangent frame at Center, rotated by RotationDegrees - AlongAxis is Canyon's running
        // direction (0 deg = local north). Cross(Up(), Center) degenerates at the poles exactly like
        // every other direction-derived tangent already in this codebase (ContinentLayer's own
        // SeparationTangent included) - an accepted simplification here too, not fixed.
        const Vec3 ReferenceTangent = Vec3::Cross(Vec3::Up(), Center).GetSafeNormal();
        const Vec3 AlongAxis = MathUtil::RotateAroundAxis(ReferenceTangent, Center, MathUtil::DegreesToRadians(Stamp.RotationDegrees));
        const Vec3 AcrossAxis = Vec3::Cross(Center, AlongAxis);

        const double RadiusRad = MathUtil::DegreesToRadians(std::max(Stamp.RadiusDegrees, 0.01f));
        const double LengthRad = MathUtil::DegreesToRadians(std::max(Stamp.LengthDegrees, 0.01f));
        const Vec3 SeedOffsetVec = NoiseBasis::ComputeSeedOffset(EffectiveSeed);

        Grid.ForEachCell([&](int32_t Idx, const Vec3& Dir, double, double)
        {
            double ShapeValue = 0.0; // archetype profile, roughly -1..1, scaled by HeightMeters below
            double Envelope = 0.0;   // 0..1 - how much of the noise overlay to blend in at this cell

            if (Stamp.Archetype == EShapeStampArchetype::Canyon)
            {
                // Azimuthal-equidistant projection onto the tangent plane at Center - exact (not a
                // flat-earth shortcut): Perp's length after this is the true great-circle angular
                // distance from Center, in whatever bearing Dir actually sits at.
                const double CosAngle = MathUtil::Clamp(Vec3::Dot(Dir, Center), -1.0, 1.0);
                const double Angle = std::acos(CosAngle);
                const Vec3 Perp = (Dir - Center * CosAngle).GetSafeNormal() * Angle;

                const double AlongT = std::abs(Vec3::Dot(Perp, AlongAxis)) / LengthRad;
                const double AcrossT = std::abs(Vec3::Dot(Perp, AcrossAxis)) / RadiusRad;
                if (AlongT >= 1.0 || AcrossT >= 1.0)
                {
                    return;
                }

                const double LengthFalloff = MathUtil::SmoothStep(1.0, 0.0, AlongT);
                const double CrossFalloff = MathUtil::SmoothStep(1.0, 0.0, AcrossT);
                ShapeValue = -CrossFalloff * LengthFalloff; // a V-shaped trench along the axis, fading at both ends
                Envelope = CrossFalloff * LengthFalloff;
            }
            else
            {
                const double Angle = std::acos(MathUtil::Clamp(Vec3::Dot(Dir, Center), -1.0, 1.0));
                const double T = Angle / RadiusRad;
                if (T >= 1.0)
                {
                    return;
                }
                const double Dome = MathUtil::SmoothStep(1.0, 0.0, T); // 1 at center .. 0 at the footprint edge
                Envelope = Dome;

                if (Stamp.Archetype == EShapeStampArchetype::Mountain)
                {
                    ShapeValue = Dome;
                }
                else if (Stamp.Archetype == EShapeStampArchetype::Volcano)
                {
                    // Same cone as Mountain, with a crater notch cut into the peak - the notch is
                    // confined to the inner 30% of the footprint so it reads as a summit crater, not
                    // a second, wider bowl competing with the cone itself.
                    const double CraterT = T / 0.3;
                    const double Dip = (CraterT < 1.0) ? MathUtil::SmoothStep(1.0, 0.0, CraterT) : 0.0;
                    ShapeValue = Dome - Dip * Stamp.CraterDepthFraction;
                }
                else // Crater - a central bowl plus a raised rim, no summit cone
                {
                    constexpr double RimPosition = 0.8; // fixed proportion - WC's own panel doesn't expose this as a separate knob either
                    const double Bowl = MathUtil::SmoothStep(RimPosition, 0.0, T); // 1 at center .. 0 at the rim
                    double RingBump = (T < RimPosition) ? (T / RimPosition) : ((1.0 - T) / (1.0 - RimPosition));
                    RingBump = MathUtil::Clamp(RingBump, 0.0, 1.0);
                    RingBump = RingBump * RingBump * (3.0 - 2.0 * RingBump); // smooth the triangular bump's peak/base creases
                    ShapeValue = RingBump * Stamp.CraterDepthFraction * 0.5 - Bowl * Stamp.CraterDepthFraction;
                }
            }

            const Vec3 SamplePos = Dir * Stamp.NoiseFrequency + SeedOffsetVec;
            const float NoiseValue = NoiseBasis::FractalNoiseGeneric(Stamp.NoiseType, SamplePos, Stamp.NoiseOctaves, Stamp.NoisePersistence, Stamp.NoiseLacunarity, EffectiveSeed);

            Field.HeightMeters[Idx] += (float)(ShapeValue * Stamp.HeightMeters) + NoiseValue * Stamp.NoiseStrengthMeters * (float)Envelope;
        });
    }
};
