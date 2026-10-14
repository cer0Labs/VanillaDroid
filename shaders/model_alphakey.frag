#version 320 es
#extension GL_GOOGLE_include_directive : require

// Alpha-key (cut-out) model parts: foliage, fences, grates.
#define ALPHA_KEY 1
#include "model_frag.glsl"
