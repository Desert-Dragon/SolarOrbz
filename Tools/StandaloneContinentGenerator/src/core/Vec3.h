// Portable double-precision 3D vector - stands in for UE's FVector (which the plugin's Continent
// Layer uses as a unit-sphere direction/position type) without pulling in any UE dependency. Only
// the operations the ported Continent/Tectonics math actually uses are implemented.
#pragma once

#include <cmath>
#include <algorithm>

struct Vec3
{
    double X = 0.0, Y = 0.0, Z = 0.0;

    Vec3() = default;
    Vec3(double InX, double InY, double InZ) : X(InX), Y(InY), Z(InZ) {}

    static Vec3 Up() { return Vec3(0.0, 0.0, 1.0); }
    static Vec3 Zero() { return Vec3(0.0, 0.0, 0.0); }

    Vec3 operator+(const Vec3& Other) const { return Vec3(X + Other.X, Y + Other.Y, Z + Other.Z); }
    Vec3 operator-(const Vec3& Other) const { return Vec3(X - Other.X, Y - Other.Y, Z - Other.Z); }
    Vec3 operator-() const { return Vec3(-X, -Y, -Z); }
    Vec3 operator*(double Scalar) const { return Vec3(X * Scalar, Y * Scalar, Z * Scalar); }
    Vec3& operator+=(const Vec3& Other) { X += Other.X; Y += Other.Y; Z += Other.Z; return *this; }
    Vec3& operator*=(double Scalar) { X *= Scalar; Y *= Scalar; Z *= Scalar; return *this; }

    static double Dot(const Vec3& A, const Vec3& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }
    static Vec3 Cross(const Vec3& A, const Vec3& B)
    {
        return Vec3(A.Y * B.Z - A.Z * B.Y, A.Z * B.X - A.X * B.Z, A.X * B.Y - A.Y * B.X);
    }

    double Length() const { return std::sqrt(X * X + Y * Y + Z * Z); }
    bool IsNearlyZero(double Tolerance = 1e-8) const { return Length() < Tolerance; }

    Vec3 GetSafeNormal(double Tolerance = 1e-8) const
    {
        const double Len = Length();
        if (Len < Tolerance)
        {
            return Vec3::Zero();
        }
        return Vec3(X / Len, Y / Len, Z / Len);
    }
};

inline Vec3 operator*(double Scalar, const Vec3& V) { return V * Scalar; }

namespace MathUtil
{
    constexpr double PI_D = 3.14159265358979323846;

    inline double DegreesToRadians(double Degrees) { return Degrees * (PI_D / 180.0); }
    inline double RadiansToDegrees(double Radians) { return Radians * (180.0 / PI_D); }

    template <typename T>
    inline T Clamp(T Value, T Min, T Max) { return std::max(Min, std::min(Max, Value)); }

    inline double Lerp(double A, double B, double T) { return A + (B - A) * T; }

    // Same cubic smoothstep shape as UE's FMath::SmoothStep (0 at/below EdgeA, 1 at/above EdgeB).
    inline double SmoothStep(double EdgeA, double EdgeB, double X)
    {
        if (EdgeA == EdgeB) return X < EdgeA ? 0.0 : 1.0;
        const double T = Clamp((X - EdgeA) / (EdgeB - EdgeA), 0.0, 1.0);
        return T * T * (3.0 - 2.0 * T);
    }

    // Rodrigues' rotation formula - rotates V around UnitAxis (must already be unit length) by
    // AngleRadians. Stands in for UE's FQuat(Axis, Angle).RotateVector(V) without a quaternion type.
    inline Vec3 RotateAroundAxis(const Vec3& V, const Vec3& UnitAxis, double AngleRadians)
    {
        const double CosA = std::cos(AngleRadians);
        const double SinA = std::sin(AngleRadians);
        return V * CosA + Vec3::Cross(UnitAxis, V) * SinA + UnitAxis * (Vec3::Dot(UnitAxis, V) * (1.0 - CosA));
    }
}
