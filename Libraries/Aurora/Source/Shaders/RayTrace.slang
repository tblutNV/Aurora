// Copyright 2025 Autodesk, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef __RAY_TRACE_H__
#define __RAY_TRACE_H__

#include "Environment.slang"
#include "Globals.slang"
#include "Random.slang"
#include "Sampling.slang"

// A structure for the results of indirect lighting and environment light sampling, which is used
// for denoising.
struct IndirectOutput
{
    float3 diffuse;
    float diffuseHitDist;
    float3 glossy;
    float glossyHitDist;

    // Clears the indirect output to values expected for a miss.
    [mutating]
    void clear()
    {
        diffuse        = BLACK;
        diffuseHitDist = INFINITY;
        glossy         = BLACK;
        glossyHitDist  = INFINITY;
    }
};

// Instance ray payload.  Used by all closest hit shaders.
struct InstanceRayPayload {
    Random rng;

    float totalDistance = 0.0f;

    uint packedData = 0;
    float3 rayContribution;

    float3 nextRayOrigin;
    float3 nextRayDirection;

    float3 direct;
    float3 indirect;

    [mutating]
    void clear()
    {
        direct = float3(0.0f);
        indirect = float3(0.0f);
    }

    property bool isPrimaryRay
    {
        get { return bool(packedData & 1); }
        set { packedData = (packedData & ~1) | (int(newValue) << 0); }
    }

    property bool pathEnded
    {
        get { return bool(packedData & 2); }
        set { packedData = (packedData & ~2) | (int(newValue) << 1); }
    }

    property bool skipContribution
    {
        get { return bool(packedData & 4); }
        set { packedData = (packedData & ~4) | (int(newValue) << 2); }
    }

    property int rayType
    {
        get { return (packedData >> 3) & 0x1f; }
        set { packedData = (packedData & ~(0x1f << 3)) | (newValue << 3); }
    }

    property int nextRayType
    {
        get { return (packedData >> 8) & 0x1f; }
        set { packedData = (packedData & ~(0x1f << 8)) | (newValue << 8); }
    }

    property int misLobeID
    {
        get { return (packedData >> 13) & 0x1f; }
        set { packedData = (packedData & ~(0x1f << 13)) | (newValue << 13); }
    }

    property int rayDepth
    {
        get { return (packedData >> 18) & 0xff; }
        set { packedData = (packedData & ~(0xff << 18)) | (newValue << 18); }
    }

    property float outputAlpha
    {
        get { return float((packedData >> 26) & 1); }
        set { packedData = (packedData & ~(1 << 26)) | int(newValue) << 26; }
    }

    property bool isInside
    {
        get { return bool(packedData & 27); }
        set { packedData = (packedData & ~(1 << 27)) | (int(newValue) << 27); }
    }
};

// Computes the origin and direction of a ray at the specified screen coordinates, using the
// specified view parameters (orientation, position, and FOV).
void computeCameraRay(float2 screenCoords, float2 screenSize, float4x4 invView, float2 viewSize,
    bool isOrtho, float focalDistance, float lensRadius, inout Random rng, out float3 origin,
    out float3 direction)
{
    // Apply a random offset to the screen coordinates, for antialiasing. Convert the screen
    // coordinates to normalized device coordinates (NDC), i.e. the range [-1, 1] in X and Y. Also
    // flip the Y component, so that +Y is up.
    screenCoords += random2D(rng);
    float2 ndc = (screenCoords / screenSize) * 2.0f - 1.0f;
    ndc.y      = -ndc.y;

    // Get the world-space orientation vectors from the inverse view matrix.
    float3 right = invView[0].xyz;  // right: row 0
    float3 up    = invView[1].xyz;  // up: row 1
    float3 front = -invView[2].xyz; // front: row 2, negated for RH coordinates

    // Build a world-space offset on the view plane, based on the view size and the right and up
    // vectors.
    float2 size            = viewSize * 0.5f;
    float3 offsetViewPlane = size.x * ndc.x * right + size.y * ndc.y * up;

    // Compute the ray origin and direction:
    // - Direction: For orthographic projection, this is just the front direction (i.e. all rays are
    //   parallel). For perspective, it is the normalized combination of the front direction and the
    //   view plane offset.
    // - Origin: For orthographic projection, this is the eye position (row 3 of the view matrix),
    //   translated by the view plane offset. For perspective, it is just the eye position.
    //
    // NOTE: It is common to "unproject" a NDC point using the view-projection matrix, and subtract
    // that from the eye position to get a direction. However, this is numerically unstable when the
    // eye position has very large coordinates and the projection matrix has small (nearby) clipping
    // distances. Clipping is not relevant for ray tracing anyway.
    if (isOrtho)
    {
        direction = front;
        origin    = invView[3].xyz + offsetViewPlane;
    }
    else
    {
        direction = normalize(front + offsetViewPlane);
        origin    = invView[3].xyz;
    }

    // Adjust the ray origin and direction if depth of field is enabled. The ray must pass through
    // the focal point (along the original direction, at the focal distance), with an origin that
    // is offset on the lens, represented as a disk.
    if (lensRadius > 0.0f)
    {
        float3 focalPoint   = origin + direction * focalDistance;
        float2 originOffset = sampleDisk(random2D(rng), lensRadius);
        origin              = origin + originOffset.x * right + originOffset.y * up;
        direction           = normalize(focalPoint - origin);
    }
}

// Traces a ray in the specified direction, returning the radiance from that direction.
void traceInstanceRay(RaytracingAccelerationStructure scene, 
    float3 origin, float3 dir, float tMin, inout InstanceRayPayload rayPayload)
{
    // Set the force opaque ray flag to treat all objects as opaque, so that the any hit shader is
    // not called.
    uint rayFlags = RAY_FLAG_FORCE_OPAQUE;

    // Prepare the ray.
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = dir;
    ray.TMin      = tMin;
    ray.TMax      = INFINITY;

    rayPayload.clear();

    // Trace the ray.
    TraceRay(scene,    // acceleration structure to be traced against
        rayFlags,      // flags to control ray behavior
        0xFF,          // instance mask
        0,             // ray contribution to hit group index
        0,             // multiplier for geometry contribution to hit group index
        kMissInstance, // miss shader index
        ray,           // ray to be traced
        rayPayload);   // in/out data for shaders invoked during tracing
}


// The payload for shadow rays.
struct ShadowRayPayload
{
    float3 visibility;
};

// Traces a shadow ray for the specified sample position and light direction, returning the
// visibiliity of the light in that direction.
float3 traceShadowRay(RaytracingAccelerationStructure scene, float3 origin, float3 L, float tMin,
    bool onlyOpaqueHits, int depth, int maxDepth)
{
    // If the maximum trace recursion depth has been reached, treat the light as visible. This will
    // mean there is more light than expected, but that works better than blocking the light, for
    // typical scenes. Depth is not tracked in the shadow ray payload because there should not be
    // any further ray tracing from shadow rays. Shadow rays are traced one level deeper than
    // radiance rays, to avoid distracting shadow-less results.
    if (depth == maxDepth + 1)
    {
        return WHITE;
    }

    // Build a ray payload, starting with either:
    // - Full visibility for normal shadow rays. This assumes a miss since the any hit shader
    //   will reduce visibility from there.
    // - Zero visibility for shadow rays that treat all objects as opaque. if the miss shader is
    //   evaluated, it will set full visibility.
    ShadowRayPayload rayPayload;
    rayPayload.visibility = onlyOpaqueHits ? BLACK : WHITE;

    // Prepare the shadow ray, with a small offset from the origin to avoid self-intersection.
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = L;
    ray.TMin      = tMin;
    ray.TMax      = INFINITY;

    // Trace the shadow ray. This is different from standard tracing for performance and behavior,
    // as follows:
    // - Skip the closest hit shader, as none is needed for shadow rays.
    // - For normal shadow rays, which support non-opaque objects, the any hit shader is used. So
    //   don't use a miss shader, as none is needed when the any hit shader is used.
    // - For shadow rays that treat all objects as opaque (i.e. forced opaque):
    //   - Stop searching as soon as the first hit is found, which is not necessarily the closest.
    //   - Treat all intersections as opaque, so that the shadow any hit shader is never called.
    //   - Use the shadow miss shader, which simply sets full visibility on ray payload.
    uint rayFlags = RAY_FLAG_SKIP_CLOSEST_HIT_SHADER;
    rayFlags |= onlyOpaqueHits ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE : 0;
#if DIRECTX
    uint missShaderIndex = onlyOpaqueHits ? kMissShadow : kMissNull;
#else
    uint missShaderIndex = kMissShadow;
#endif

    TraceRay(scene, rayFlags, 0xFF, 0, 0, missShaderIndex, ray, rayPayload);

    // Return the shadow ray visibility.
    return rayPayload.visibility;
}

#endif // __RAY_TRACE_H__
