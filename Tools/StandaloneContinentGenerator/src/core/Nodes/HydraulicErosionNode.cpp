#include "HydraulicErosionNode.h"
#include "../Random.h"
#include <algorithm>
#include <cmath>

namespace
{
    // Spreads a height delta across the 4 integer cells surrounding a fractional position, weighted
    // by the same bilinear weights FHeightField::SampleBilinear reads with - the standard inverse
    // operation so repeated erode/deposit at drifting fractional positions doesn't introduce grid-
    // aligned artifacts.
    void DepositBilinear(FHeightField& Field, double PosX, double PosY, float Amount)
    {
        double IntPart;
        const double FxFrac = std::modf(PosX, &IntPart);
        const double Fx = FxFrac < 0.0 ? FxFrac + 1.0 : FxFrac;
        const int32_t X0 = Field.WrapX((int32_t)std::floor(PosX));
        const int32_t X1 = Field.WrapX(X0 + 1);
        const double PosYClamped = MathUtil::Clamp(PosY, 0.0, (double)(Field.Height - 1));
        const int32_t Y0 = Field.ClampY((int32_t)std::floor(PosYClamped));
        const int32_t Y1 = Field.ClampY(Y0 + 1);
        const double Fy = PosYClamped - std::floor(PosYClamped);

        Field.At(X0, Y0) += (float)(Amount * (1.0 - Fx) * (1.0 - Fy));
        Field.At(X1, Y0) += (float)(Amount * Fx * (1.0 - Fy));
        Field.At(X0, Y1) += (float)(Amount * (1.0 - Fx) * Fy);
        Field.At(X1, Y1) += (float)(Amount * Fx * Fy);
    }

    // Bilinear-interpolated gradient at a fractional position, built from the 4 surrounding cells'
    // own central-difference gradients (FHeightField::Gradient) - the standard construction from
    // the published droplet algorithm, giving a smoothly-varying descent direction instead of one
    // that jumps every time the droplet crosses an integer cell boundary.
    void BilinearGradient(const FHeightField& Field, double PosX, double PosY, float& OutGx, float& OutGy)
    {
        const int32_t X0 = Field.WrapX((int32_t)std::floor(PosX));
        const int32_t Y0 = Field.ClampY((int32_t)std::floor(MathUtil::Clamp(PosY, 0.0, (double)(Field.Height - 1))));
        double IntPart;
        const double FxFrac = std::modf(PosX, &IntPart);
        const double Fx = FxFrac < 0.0 ? FxFrac + 1.0 : FxFrac;
        const double PosYClamped = MathUtil::Clamp(PosY, 0.0, (double)(Field.Height - 1));
        const double Fy = PosYClamped - std::floor(PosYClamped);

        float Gx00, Gy00, Gx10, Gy10, Gx01, Gy01, Gx11, Gy11;
        Field.Gradient(X0, Y0, Gx00, Gy00);
        Field.Gradient(X0 + 1, Y0, Gx10, Gy10);
        Field.Gradient(X0, Y0 + 1, Gx01, Gy01);
        Field.Gradient(X0 + 1, Y0 + 1, Gx11, Gy11);

        OutGx = (float)(Gx00 * (1 - Fx) * (1 - Fy) + Gx10 * Fx * (1 - Fy) + Gx01 * (1 - Fx) * Fy + Gx11 * Fx * Fy);
        OutGy = (float)(Gy00 * (1 - Fx) * (1 - Fy) + Gy10 * Fx * (1 - Fy) + Gy01 * (1 - Fx) * Fy + Gy11 * Fx * Fy);
    }
}

void HydraulicErosionNode::Apply(FHeightField& Field, const FPipelineContext& Ctx)
{
    FRandomStream Stream(Ctx.Seed + Config.SeedOffset);

    for (int32_t DropletIdx = 0; DropletIdx < Config.NumDroplets; ++DropletIdx)
    {
        double PosX = Stream.FRandRange(0.0f, (float)(Field.Width - 1));
        double PosY = Stream.FRandRange(0.0f, (float)(Field.Height - 1));
        double DirX = 0.0, DirY = 0.0;
        float Speed = Config.InitialSpeed;
        float Water = Config.InitialWaterVolume;
        float Sediment = 0.0f;

        for (int32_t Step = 0; Step < Config.MaxDropletLifetime; ++Step)
        {
            float GradX, GradY;
            BilinearGradient(Field, PosX, PosY, GradX, GradY);

            DirX = DirX * Config.Inertia - (double)GradX * (1.0 - Config.Inertia);
            DirY = DirY * Config.Inertia - (double)GradY * (1.0 - Config.Inertia);
            const double Len = std::sqrt(DirX * DirX + DirY * DirY);
            if (Len < 1e-8)
            {
                const double RandAngle = Stream.FRandRange(0.0f, 2.0f * (float)MathUtil::PI_D);
                DirX = std::cos(RandAngle);
                DirY = std::sin(RandAngle);
            }
            else
            {
                DirX /= Len;
                DirY /= Len;
            }

            const double NewPosX = PosX + DirX;
            const double NewPosY = PosY + DirY;
            if (NewPosY < 0.0 || NewPosY > (double)(Field.Height - 1))
            {
                break; // wandered off a pole edge - stop this droplet
            }

            const float OldHeight = Field.SampleBilinear(PosX, PosY);
            const float NewHeight = Field.SampleBilinear(NewPosX, NewPosY);
            const float DeltaHeight = NewHeight - OldHeight;

            const float SedimentCapacity = std::max(-DeltaHeight, Config.MinSlope) * Speed * Water * Config.SedimentCapacityFactor;

            if (Sediment > SedimentCapacity || DeltaHeight > 0.0f)
            {
                const float DepositAmount = (DeltaHeight > 0.0f)
                    ? std::min(DeltaHeight, Sediment)
                    : (Sediment - SedimentCapacity) * Config.DepositionRate;
                Sediment -= DepositAmount;
                DepositBilinear(Field, PosX, PosY, DepositAmount * Config.ErosionStrengthMeters);
            }
            else
            {
                const float ErodeAmount = std::min((SedimentCapacity - Sediment) * Config.ErosionRate, -DeltaHeight);
                DepositBilinear(Field, PosX, PosY, -ErodeAmount * Config.ErosionStrengthMeters);
                Sediment += ErodeAmount;
            }

            Speed = std::sqrt(std::max(Speed * Speed - DeltaHeight * Config.Gravity, 0.0f));
            Water *= (1.0f - Config.EvaporationRate);

            PosX = NewPosX;
            PosY = NewPosY;

            if (Water < 0.01f)
            {
                break;
            }
        }
    }
}
