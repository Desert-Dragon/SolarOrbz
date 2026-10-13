#include "ThermalErosionNode.h"
#include <cmath>
#include <vector>
#include <algorithm>

void ThermalErosionNode::Apply(FHeightField& Field, const FPipelineContext&)
{
    const float TanTalus = std::tan((float)MathUtil::DegreesToRadians(Config.TalusAngleDegrees));
    const float ThresholdMeters = TanTalus * Config.CellSizeMeters;

    std::vector<float> Delta(Field.HeightMeters.size(), 0.0f);

    for (int32_t Iter = 0; Iter < Config.Iterations; ++Iter)
    {
        std::fill(Delta.begin(), Delta.end(), 0.0f);

        for (int32_t Y = 0; Y < Field.Height; ++Y)
        {
            for (int32_t X = 0; X < Field.Width; ++X)
            {
                const float H = Field.At(X, Y);
                const int32_t Nx[4] = { X + 1, X - 1, X, X };
                const int32_t Ny[4] = { Y, Y, Y + 1, Y - 1 };

                float Excess[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                float ExcessSum = 0.0f;
                for (int32_t i = 0; i < 4; ++i)
                {
                    const float Diff = H - Field.At(Nx[i], Ny[i]);
                    if (Diff > ThresholdMeters)
                    {
                        Excess[i] = Diff - ThresholdMeters;
                        ExcessSum += Excess[i];
                    }
                }
                if (ExcessSum <= 0.0f)
                {
                    continue;
                }

                const size_t SrcIdx = (size_t)Y * Field.Width + X;
                for (int32_t i = 0; i < 4; ++i)
                {
                    if (Excess[i] <= 0.0f)
                    {
                        continue;
                    }
                    // 0.5x the raw excess so a single step redistributes toward (not past) the talus
                    // threshold - without this, a steep single-neighbor slope could overshoot and
                    // start oscillating between the two cells each iteration instead of converging.
                    const float Moved = Config.TalusErosionRate * Excess[i] * 0.5f;
                    Delta[SrcIdx] -= Moved;
                    const int32_t DstX = Field.WrapX(Nx[i]);
                    const int32_t DstY = Field.ClampY(Ny[i]);
                    Delta[(size_t)DstY * Field.Width + DstX] += Moved;
                }
            }
        }

        for (size_t i = 0; i < Field.HeightMeters.size(); ++i)
        {
            Field.HeightMeters[i] += Delta[i];
        }
    }
}
