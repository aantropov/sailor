using System;

namespace Sailor
{
    public static class Mathf
    {
        public const float PI = 3.14159265358979323846f;

        public static float Acos(float d) => (float)Math.Acos(d);

        public static float Asin(float d) => (float)Math.Asin(d);

        public static float Atan(float d) => (float)Math.Atan(d);

        public static float Atan2(float y, float x) => (float)Math.Atan2(y, x);

        public static float Ceiling(float a) => (float)Math.Ceiling(a);

        public static float Cos(float d) => (float)Math.Cos(d);

        public static float Cosh(float value) => (float)Math.Cosh(value);

        public static float Floor(float d) => (float)Math.Floor(d);

        public static float Sin(float a) => (float)Math.Sin(a);

        public static float Tan(float a) => (float)Math.Tan(a);

        public static float Sinh(float value) => (float)Math.Sinh(value);

        public static float Tanh(float value) => (float)Math.Tanh(value);

        public static float Round(float a) => (float)Math.Round(a);

        public static float Truncate(float d) => (float)Math.Truncate(d);

        public static float Sqrt(float d) => (float)Math.Sqrt(d);

        public static float Log(float d) => (float)Math.Log(d);

        public static float Log10(float d) => (float)Math.Log10(d);

        public static float Exp(float d) => (float)Math.Exp(d);

        public static float Pow(float x, float y) => (float)Math.Pow(x, y);

        public static float IEEERemainder(float x, float y) => (float)Math.IEEERemainder(x, y);

        public static float Abs(float value) => (float)Math.Abs(value);

        public static float Max(float val1, float val2) => (float)Math.Max(val1, val2);

        public static float Min(float val1, float val2) => (float)Math.Min(val1, val2);

        public static float Log(float a, float newBase) => (float)Math.Log(a, newBase);

        public static float ToRadians(float degrees) => degrees * (PI / 180.0f);

        public static float ToDegrees(float radians) => radians * (180.0f / PI);
    }
}
