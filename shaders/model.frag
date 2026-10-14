#version 320 es
#extension GL_GOOGLE_include_directive : require

// Opaque and blended model parts: no discard, so the opaque ones keep early depth rejection (LRZ).
#include "model_frag.glsl"
