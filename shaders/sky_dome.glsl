// The sky dome of the 1.12 client (DayNight in WoW.exe): a unit sphere lowered by cos(45 degrees), so that its
// ring at 45 degrees from the top is the horizon around the camera, with rings at these angles from the top and
// 24 segments. Seen from the camera the rings stand at 90, 16.8, 9.8, 3.7, 1.8 and 0 degrees of elevation; the
// last ring closes the sphere straight below. Generated from gl_VertexIndex: 6 vertices (2 triangles) per segment
// of each gap between two rings.

const int kSkySegments = 24;
const float kSkyRingPolar[7] = float[](0.0, 30.6, 36.0, 41.4, 43.2, 45.0, 180.0); // degrees from the top
const ivec2 kSkyCorners[6] = ivec2[](ivec2(0, 0), ivec2(1, 0), ivec2(1, 1), ivec2(0, 0), ivec2(1, 1), ivec2(0, 1));

// The vertex's position around the camera (not unit length) and its ring (0 = the top).
highp vec3 skyDomeVertex(int vertexIndex, out int ring) {
    int quad = vertexIndex / 6;
    ivec2 corner = kSkyCorners[vertexIndex - quad * 6];
    ring = quad / kSkySegments + corner.x;
    int segment = quad - (quad / kSkySegments) * kSkySegments + corner.y;
    highp float polar = radians(kSkyRingPolar[ring]);
    highp float azimuth = float(segment) * (6.2831853 / float(kSkySegments));
    return vec3(sin(polar) * cos(azimuth), sin(polar) * sin(azimuth), cos(polar) - 0.70710678);
}
