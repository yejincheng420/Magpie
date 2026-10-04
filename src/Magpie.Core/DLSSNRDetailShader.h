#pragma once
#include <string_view>

namespace Magpie {
// Appended after the resample cbuffer declaration. Working textures
// stay signed RGB FP16; Lab deltas are never added directly to stored RGB.
inline constexpr std::string_view DLSSNR_DETAIL_HLSL = R"hlsl(
float3 ApplyOklabControls(float3 original, float3 denoised, out float4 metrics) {
    metrics = float4(0,1,1,1); // delta L, protection, compression, gamut scale
    float3 residual = (denoised-original)*ResidualMultiplier;
    if (ResidualMultiplier == 0 || all(residual == 0)) return original;
    float3 o = RGBToLab(original);
    // Extended sRGB and signed cube roots retain the unclipped RGB candidate.
    float3 d = RGBToLab(original+residual)-o;
    d.x = (min(d.x,0)*ShadowStructureMultiplier + max(d.x,0)*ReflectionGlowMultiplier)*ResidualLightness;
    d.yz *= ResidualSaturation;
    if (HueProtection > 0) {
        float c = length(o.yz);
        // Smoothly vanish near gray. No arbitrary hue or angle wrap.
        float weight = HueProtection*smoothstep(0,.02,c);
        float2 n = o.yz/max(c,1e-12);
        float2 target = o.yz+d.yz;
        // Projection cannot overshoot through the origin into the opposite hue.
        float2 protectedColor = n*max(dot(target,n),0);
        d.yz = lerp(target,protectedColor,weight)-o.yz;
    }
    float dark = DarkProtection*(1-smoothstep(.08,.35,o.x));
    float bright = HighlightProtection*smoothstep(.72,.98,o.x);
    // White-area darkening can recover detail: protect it at half strength.
    // Saturated channels with no outward headroom receive extra protection.
    float3 outward = max(residual,0);
    float headroom = max(outward.r/(1-original.r+1e-4),
        max(outward.g/(1-original.g+1e-4),outward.b/(1-original.b+1e-4)));
    bright *= lerp(.5,1,smoothstep(0,.02,d.x));
    bright = max(bright,HighlightProtection*smoothstep(.72,.98,o.x)*smoothstep(.5,2,headroom));
    metrics.y = (1-dark)*(1-bright);
    d *= metrics.y;
    float magnitude = length(d);
    float excess = max(magnitude-.04,0);
    // Identity below the knee, matching first derivative at the knee.
    float compressed = .04 + excess/(1+excess/.12);
    metrics.z = lerp(1,compressed/max(magnitude,1e-12),LocalCompression);
    if (magnitude <= .04) metrics.z = 1;
    d *= metrics.z;
    metrics.x = d.x;
    return MapLab(o+d,metrics.w);
}
float3 ControlledColor(int2 p, out float4 metrics) {
    float3 o = ReducedColor.Load(int3(p,0)).rgb;
    float3 n = ReducedDenoised.Load(int3(p,0)).rgb;
    metrics = float4(0,1,1,1);
    if (!all(isfinite(o))) return 0;
    if (!all(isfinite(n))) return o;
    // Exact neutral output, no conversion work. Near-neutral values converge
    // to this candidate because the gamut map is identity on in-gamut RGB.
    if (ResidualMultiplier == 1 && ResidualSaturation == 1 && ResidualLightness == 1 &&
        ShadowStructureMultiplier == 1 && ReflectionGlowMultiplier == 1 &&
        HueProtection == 0 && DarkProtection == 0 && HighlightProtection == 0 &&
        LocalCompression == 0 && DebugView == 0) return n;
    return ApplyOklabControls(o,n,metrics);
}
[numthreads(8,8,1)]
void PrepareResidual(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    int2 p = tid.xy;
    float3 o = ReducedColor.Load(int3(p,0)).rgb;
    if (!all(isfinite(o))) o = 0;
    float4 metrics;
    float3 color = ControlledColor(p,metrics);
    float3 residual = color-o;
    // Edge-aware 3x3 (radius 1) complementary split of controlled RGB deltas.
    // Neutral frequency gains bypass every neighbor load and conversion.
    if ((LowFrequencyGain != 1 || DetailGain != 1) && ResidualMultiplier != 0) {
        float3 low = 0;
        float mass = 0;
        [unroll] for (int y=-1;y<=1;++y) {
            [unroll] for (int x=-1;x<=1;++x) {
                int2 q = clamp(p+int2(x,y),0,int2(TargetExtent)-1);
                float3 guide = ReducedColor.Load(int3(q,0)).rgb;
                if (!all(isfinite(guide))) continue;
                float3 diff = guide-o;
                float spatial = (x==0 ? 2 : 1)*(y==0 ? 2 : 1);
                float w = spatial*exp(-dot(diff,diff)/.005);
                float4 unused;
                low += (ControlledColor(q,unused)-guide)*w;
                mass += w;
            }
        }
        low /= max(mass,1e-12);
        float3 high = residual-low;
        residual = LowFrequencyGain*low+DetailGain*high;
        float scale;
        color = MapLab(RGBToLab(o+residual),scale);
        residual = color-o;
        metrics.w *= scale;
    }
    // Views are opt-in; no extra NR evaluation. Temporal output is bypassed
    // while viewing diagnostics, preserving the NR chain's raw observations.
    if (DebugView != 0) {
        float3 raw = ReducedDenoised.Load(int3(p,0)).rgb-o;
        if (DebugView == 1) color = saturate(.5+raw*4);
        else if (DebugView == 2) color = saturate(.5+residual*4);
        else if (DebugView == 3) color = saturate(.5+metrics.x*4);
        else if (DebugView == 4) color = saturate(length((RGBToLab(o+residual)-RGBToLab(o)).yz)*4);
        else if (DebugView == 5) color = metrics.y;
        else if (DebugView == 6) color = metrics.w;
        else if (DebugView == 7) color = metrics.z;
        residual = color-o;
    }
    ControlledResidual[p] = float4(DebugView != 0 ? color : residual,0);
}
)hlsl";
}
