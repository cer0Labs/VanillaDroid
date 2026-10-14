// Phase 11: the 1.12 client's full-screen effects (FFXEffects.cpp), applied by every world fragment shader to the
// color it produces, after the fog, instead of in passes of their own over the whole screen (a tiled GPU would flush
// its tiles for each). Include after frame.glsl. Its CPU mirror is render/ScreenFx.h.
//
// - "Full-Screen Glow" (CVar ffxGlow, Shaders\Pixel\FFXGlow.bls): scene + glow^2 * LightParams.glow, where `glow` is
//   the scene blurred at a lower resolution. Here the fragment's own color stands in for the blurred one: the same
//   brightening of light colors, without the halo around them.
// - "Death Effect" (CVar ffxDeath, Shaders\Pixel\FFXDeath.bls), the ghost's world: the luminance of scene + glow^2 *
//   glow weight, plus a grey-blue tint on the middle tones: L + tint * saturate(4 L (1 - L)). The luminance weights
//   (0.299, 0.587, 0.144) and the tint (0x5393A8) are the client's own.
// frame.screenFx: x = the glow's weight (0 = no glow), y = 1 in the ghost's world with the death effect on.
#ifndef VD_SCREENFX_GLSL
#define VD_SCREENFX_GLSL

const vec3 kScreenFxLuminance = vec3(0.299, 0.587, 0.144);
const vec3 kScreenFxDeathTint = vec3(83.0, 147.0, 168.0) / 255.0;
const vec3 kScreenFxGray = vec3(0.299, 0.587, 0.114); // Shaders\Pixel\Desaturate.bls: keeps white white
// pack::ModelBlend. Macros, not constants: a uint constant of the same value declared by the including shader would
// be merged with ours and decorated RelaxedPrecision twice (invalid SPIR-V).
#define VD_SCREENFX_BLEND_ADD 3u
#define VD_SCREENFX_BLEND_MOD 4u
#define VD_SCREENFX_BLEND_MOD2X 5u

// A surface's color (opaque, alpha-tested or alpha-blended).
vec3 screenFx(vec3 color) {
    color += frame.screenFx.x * color * color;
    if (frame.screenFx.y > 0.0) {
        float l = clamp(dot(color, kScreenFxLuminance), 0.0, 1.0);
        color = vec3(l) + kScreenFxDeathTint * clamp(4.0 * l * (1.0 - l), 0.0, 1.0);
    }
    return color;
}

// Any blend (pack::ModelBlend): light added over the scene glows but takes no tint of its own (the scene under it has
// it), and colors that multiply the scene only lose their hue in the ghost's world.
vec3 screenFxBlend(vec3 color, uint blend) {
    if (blend == VD_SCREENFX_BLEND_MOD || blend == VD_SCREENFX_BLEND_MOD2X) {
        return frame.screenFx.y > 0.0 ? vec3(dot(color, kScreenFxGray)) : color;
    }
    if (blend == VD_SCREENFX_BLEND_ADD) {
        color += frame.screenFx.x * color * color;
        return frame.screenFx.y > 0.0 ? vec3(dot(color, kScreenFxGray)) : color;
    }
    return screenFx(color);
}

#endif
