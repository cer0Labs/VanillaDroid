#version 320 es
#extension GL_GOOGLE_include_directive : require

// Alpha-key (cut-out) model parts with alpha to coverage (multisampled passes): the alpha test as a coverage ramp.
#define ALPHA_KEY 1
#define ALPHA_TO_COVERAGE 1
#include "model_frag.glsl"
