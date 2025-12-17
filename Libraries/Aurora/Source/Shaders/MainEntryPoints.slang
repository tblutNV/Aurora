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
// Prefix containing the common code used by all material types.
#include "Options.slang"
#include "HitShaderEntryPoints.slang"
#include "DefaultMaterial.slang"

// =================================================================================================
// Ray Gen Shader
// =================================================================================================
// HgiVulkan can only have one main entry point for the shader source.
// To share this file between different renderers, we leverage preprocessors and
// enable one shader stage at a time.
// TODO: Consider to use separate shader files for each shader stage.
#ifndef DISABLE_HGI_SHADER_STAGE_RAY_GEN

// Results of the path tracing sample.
struct Output
{
    // Depth in Normalized Device Coordinates, for entire path.
    float depthNDC;
    // Normal of first collision in path.
    float3 normal;
    // Material metalness of first collision in path.
    float metalness;
    // Material roughness of first collision in path.
    float roughness;
    // Material base color of first collision in path.
    float3 baseColor;
    // Depth in Normalized Device Coordinates, for entire path.
    float depthView;
    // Direct lighting contribution for path.
    float3 direct;
    // Indirect lighting contribution for path (split into diffuse and glossy.)
    IndirectOutput indirect;
    // Alpha for path (0.0 only if all ray segments were transparent)
    float alpha;

    [mutating]
    void clear()
    {
        indirect.clear();
        depthNDC = 1.0f;
        normal = -1.0f;
        metalness = 0.1f;
        roughness = 0.1f;
        baseColor = BLACK;
        depthView = INFINITY;
        direct = 0.0f;
        alpha = 0.0;
    }
}

// The ray generation shader, iteratively shades the entire ray path.
[shader("raygeneration")]
void RayGenShader()
{
    // Get the max trace depth from frame settings.
    int maxTraceDepth = gFrameData.traceDepth;

    // Get the dispatch dimensions (screen size), dispatch index (screen coordinates), and sample
    // index.
    uint2 screenSize = DispatchRaysDimensions().xy;
    uint2 screenCoords = DispatchRaysIndex().xy;
    uint sampleIndex = gSampleData.sampleIndex;

    InstanceRayPayload rayPayload;
    rayPayload.rng             = initRand(sampleIndex, screenSize, screenCoords);
    rayPayload.totalDistance   = 0.0f;
    rayPayload.rayContribution = float3(1.0f, 1.0f, 1.0f);
    rayPayload.packedData      = 0;
    rayPayload.rayType         = CAMERA_RAY;
    rayPayload.misLobeID       = NO_SECONDARY_RAY;
    rayPayload.outputAlpha     = 0.0f;
    rayPayload.isPrimaryRay    = true;
    rayPayload.isInside        = false;

    // Compute a camera ray (origin and direction) for the current screen coordinates. This applies
    // a random offset to support antialiasing and depth of field.
    float3 rayOrigin;
    float3 rayDirection;
    computeCameraRay(screenCoords, screenSize, gFrameData.cameraInvView, gFrameData.viewSize,
        gFrameData.isOrthoProjection, gFrameData.focalDistance, gFrameData.lensRadius, rayPayload.rng, rayOrigin,
        rayDirection);

    // The results of the tracing the entire ray path through the scene.
    Output output;
    output.clear();

    // Prepare the environment data structure.
    Environment environment = prepareEnvironmentValues();

    // Iterate through every segment in the ray path, until maximum depth is reached or path is ended.
    for (int rayDepth = 0; rayDepth <= maxTraceDepth && !rayPayload.pathEnded; rayDepth++)
    {
        // Have we exceeded ray depth for this path?
        if (rayDepth == maxTraceDepth)
        {
            // If this ray collides with nothing, should the background or lighting environment be shaded?
            bool shadeBackgroundOnMiss = rayPayload.isPrimaryRay
                || rayPayload.rayType == TRANSMISSION_LOBE
                || rayPayload.rayType == GROUND_PLANE_REFLECTION_RAY;

            // If we have reached the end of this path, evaluate the environment and terminate the path.
            rayPayload.direct = evaluateEnvironment(environment, rayDirection, shadeBackgroundOnMiss);
            rayPayload.direct *= rayPayload.rayContribution;
            rayPayload.pathEnded = true;
        }
        else
        {
            // The minimum T for this ray segment is zero for the initial camera ray otherwise use
            // a small epsilon to avoid self-intersection.
            float minT = rayDepth == 0 ? 0.0f : M_RAY_TMIN;

            // Trace an instance ray that will return the instance shading result from the closest hit, if any.
            rayPayload.rayDepth = rayDepth;
            traceInstanceRay(gScene, rayOrigin, rayDirection, minT, rayPayload);

            rayOrigin = rayPayload.nextRayOrigin;
            rayDirection = rayPayload.nextRayDirection;
            output.alpha = rayPayload.outputAlpha;

            if (rayPayload.skipContribution)
            {
                rayPayload.skipContribution = false;
                continue;
            }
        }

        // How the result is accumulated depends on whether this ray is a primary ray.
        // TODO: This is never true. Do rayDepth == 0 instead?
        if (rayPayload.isPrimaryRay)
        {
            // If it is a primary ray, accumulate direct and indirect components in to equivalent component in the output data.
            output.direct = rayPayload.direct;
            if (rayPayload.misLobeID == DIFFUSE_LOBE)
            {
                output.indirect.diffuse = rayPayload.indirect;
            }
            else
            {
                output.indirect.glossy = rayPayload.indirect;
            }
        }
        else
        {
            // If this secondary ray compute the total radiance from all the lighting components.
            float3 radiance = rayPayload.direct + rayPayload.indirect;

            // Accumulate the total radiance into diffuse or glossy component of output indirect
            // component, based on lobe this ray segment originated from.
            if (rayPayload.rayType == DIFFUSE_LOBE)
            {
                output.indirect.diffuse += radiance;
                output.indirect.diffuseHitDist = rayPayload.totalDistance;
            }
            else
            {
                output.indirect.glossy += radiance;
                output.indirect.glossyHitDist = rayPayload.totalDistance;
            }
        }

        // Set the ray type for the next segment.
        rayPayload.rayType = rayPayload.nextRayType;
    }

    float3 finalRadiance = output.direct + output.indirect.diffuse + output.indirect.glossy;
    float4 result = float4(finalRadiance, output.alpha);

    // Adjust the radiance of the sample, e.g. to perform corrections.
    adjustRadiance(gFrameData.maxLuminance, gFrameData.isDisplayErrorsEnabled, result.rgb);

    // Store the result in the "result" output texture. If denoising is enabled, only the "direct"
    // (non-denoised) shading is included in the result texture, as the rest of shading is stored in
    // the denoising AOVs below.
    result.rgb = gFrameData.isDenoisingEnabled ? output.direct : result.rgb;
    gResult[screenCoords] = result;

    if (gFrameData.isDenoisingAOVsEnabled)
    {
        // Prepare the diffuse and glossy radiance and hit distances. The hit distances are
        // normalized relative to the scene size, so they are always in [0.0, 1.0].
        float diffuseHitDist = saturate(output.indirect.diffuseHitDist / gFrameData.sceneSize);
        float glossyHitDist = saturate(output.indirect.glossyHitDist / gFrameData.sceneSize);
        float4 diffusePacked = float4(output.indirect.diffuse, diffuseHitDist); // TODO: use NRD packing
        float4 glossyPacked = float4(output.indirect.glossy, glossyHitDist);    // TODO: use NRD packing

        // Store the data for the denoising AOVs.
        gDiffuse[screenCoords] = diffusePacked;
        gGlossy[screenCoords] = glossyPacked;
    }
}
#endif // DISABLE_HGI_SHADER_STAGE_RAY_GEN

// =================================================================================================
// Shadow miss shader
// =================================================================================================
#ifndef DISABLE_HGI_SHADER_STAGE_MISS
// Shadow miss results sets the visibility to WHITE.
[shader("miss")]
void ShadowMissShader(inout ShadowRayPayload rayPayload)
{
    rayPayload.visibility = WHITE;
}
#endif // DISABLE_HGI_SHADER_STAGE_MISS

// =================================================================================================
// Instance miss shader
// =================================================================================================

[shader("miss")]
void InstanceMissShader(inout InstanceRayPayload rayPayload)
{
    float3 rayOrigin           = WorldRayOrigin();
    float3 rayDirection        = WorldRayDirection();
    Environment environment    = prepareEnvironmentValues();

    // Add the contribution of the ground plane, if enabled.
    bool absorbedByGroundPlane = evaluateGroundPlane(rayPayload, rayOrigin, rayDirection, environment);
    if (absorbedByGroundPlane)
        return;

    // If this ray collides with nothing, should the background or lighting environment be shaded?
    bool shadeBackgroundOnMiss = rayPayload.isPrimaryRay
        || rayPayload.rayType == TRANSMISSION_LOBE
        || rayPayload.rayType == GROUND_PLANE_REFLECTION_RAY;

    // No geometry was hit, shade the ray using the background color.
    // If needed otherwise do nothing, as environment lighting was calculated during MIS environment shading.
    if (shadeBackgroundOnMiss)
    {
        rayPayload.direct = evaluateEnvironment(environment, WorldRayDirection(), true);
        rayPayload.direct *= rayPayload.rayContribution;
    }

    // Is this a primary ray?
    if (rayPayload.isPrimaryRay)
    {
        uint2 screenCoords = DispatchRaysIndex().xy;

        // Prepare and store the data for the denoising AOVs, if enabled.
        // NOTE: Only the data for the last sample is stored, with the expectation that the sample count
        // will be one when these AOVs are enabled.
        if (gFrameData.isDenoisingAOVsEnabled)
        {
            // Store the data for the denoising AOVs.
            gDepthView[screenCoords] = INFINITY;
            gNormalRoughness[screenCoords] = float4(0.0f, 0.0f, 0.0f, 0.1f);
            gBaseColorMetalness[screenCoords] = float4(0.0f, 0.0f, 0.0f, 0.1f);
        }

        // Store the NDC depth value, if enabled. The value is only stored if the first sample was
        // computed (i.e. there is no previously stored value), or it is less than the previously stored
        // value.
        if (gFrameData.isDepthNDCEnabled)
        {
            // Set the output Normalized Device Coordinate (NDC) depth from hit position.
            float depthNDC = 1.0f;

            if (gSampleData.sampleIndex == 0 || depthNDC < gDepthNDC[screenCoords])
            {
                gDepthNDC[screenCoords] = depthNDC;
            }
        }
    }

    // Terminate the path.
    rayPayload.pathEnded = true;
}
