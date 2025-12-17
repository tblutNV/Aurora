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
#ifndef __GEOMETRY_H__
#define __GEOMETRY_H__

#include "Globals.slang"

interface IGeometry
{
    uint3 getIndices(int bufferLocation);
    float3 getPosition(int bufferLocation);
    float3 getNormal(int bufferLocation);
    float3 getTangent(int bufferLocation);
    float2 getTexCoord(int bufferLocation);
    bool hasTexCoords();
    bool hasNormals();
    bool hasTangents();
    bool isOpaque();
    int getBufferOffset();
};

float4x4 to4x4(float3x4 m)
{
    return float4x4(m[0], m[1], m[2], float4(0.0f, 0.0f, 0.0f, 1.0f));
}

float3x4 to3x4(float4x4 m)
{
    return float3x4(m[0], m[1], m[2]);
}

// Shading data for an intersection point, with interpolated values from adjacent vertices.
struct ShadingData
{
    property float2 texCoord
    {
        get { return text_coords[0].xy; }
        set { text_coords[0] = float3(newValue.x, newValue.y, 0.0f); }
    }

    property float3 tangent
    {
        get { return tangent_u[0]; }
        set { tangent_u[0] = newValue; }
    }

    property float3 bitangent
    {
        get { return tangent_v[0]; }
        set { tangent_v[0] = newValue; }
    }

    // MaterialX (>= 1.39.x) compatibility alias (same as bitangent)
    property float3 bitangentWorld
    {
        get { return tangent_v[0]; }
        set { tangent_v[0] = newValue; }
    }

    property float3x4 objToWorld
    {
        get { return to3x4(object_to_world); }
        set { object_to_world = to4x4(newValue); }
    }

    // MDL shading state fields
    float3 normal;
    float3 geom_normal;
    float3 position;
    float animation_time;
    float3 text_coords[1];   // NOTE: Must match the value set for "num_texture_spaces "in MdlSdk.cpp
    float3 tangent_u[1];     // NOTE: Must match the value set for "num_texture_spaces in MdlSdk.cpp
    float3 tangent_v[1];     // NOTE: Must match the value set for "num_texture_spaces in MdlSdk.cpp
    float4 text_results[16]; // NOTE: Must match the value set for "num_texture_results" in in MdlSdk.cpp
    uint ro_data_segment_offset;
    float4x4 world_to_object;
    float4x4 object_to_world;
    uint object_id;
    float meters_per_scene_unit;
    uint arg_block_offset;

    // Aurora specific fields
    float3 geomPosition;
    uint3 indices;
    float3 barycentrics;
    float2 ior; // x = IOR current medium, y = IOR other side
};


// Computes the barycentric coordinates from the specified triangle intersection.
float3 computeBarycentrics(BuiltInTriangleIntersectionAttributes attribs)
{
    float2 bary2 = attribs.barycentrics;
    return float3(1.0f - bary2.x - bary2.y, bary2.x, bary2.y);
}

// Computes the barycentric coordinates from the specified triangle intersection.
float3 computeBarycentrics(float2 bary2)
{
    return float3(1.0f - bary2.x - bary2.y, bary2.x, bary2.y);
}
// Projects the specified position (point) onto the plane with the specified origin and normal.
float3 projectOnPlane(float3 position, float3 origin, float3 normal)
{
    return position - dot(position - origin, normal) * normal;
}

// Computes the shading position of the specified geometric position and vertex positions and
// normals. For a triangle with normals describing a convex surface, this point will be slightly
// above the surface. For a concave surface, the geometry position is used directly.
// NOTE: The difference between the shading position and geometry position is significant when
// casting shadow rays. If the geometric position is used, a triangle may fully shadow itself when
// it should be partly lit based on the shading normals; this is the "shadow terminator" problem.
float3 computeShadingPosition(float3 geomPosition, float3 shadingNormal, float3 positions[3],
    float3 normals[3], float3 barycentrics)
{
    // Project the geometric position (inside the triangle) to the planes defined by the vertex
    // positions and normals.
    float3 p0 = projectOnPlane(geomPosition, positions[0], normals[0]);
    float3 p1 = projectOnPlane(geomPosition, positions[1], normals[1]);
    float3 p2 = projectOnPlane(geomPosition, positions[2], normals[2]);

    // Interpolate the projected positions using the barycentric coordinates, which gives the
    // shading position.
    float3 shadingPosition = p0 * barycentrics.x + p1 * barycentrics.y + p2 * barycentrics.z;

    // Return the shading position for a convex triangle, where the shading point is above the
    // triangle based on the shading normal. Otherwise use the geometric position.
    bool convex = dot(shadingPosition - geomPosition, shadingNormal) > 0.0f;
    return convex ? shadingPosition : geomPosition;
}

// Computes the shading data (e.g. interpolated vertex data) for the triangle with the specified
// (primitive) index, at the specified barycentric coordinates.
ShadingData computeShadingData<GeometryType : IGeometry>(
    GeometryType geometry, uint triangleIndex, float2 bary2, float3x4 objToWorld, float3x4 worldToObj)
{
    // Expand barycentrics to 3d.
    float3 barycentrics = computeBarycentrics(bary2);

    // Initialize a result structure.
    ShadingData shading;
    shading.position     = float3(0.0f, 0.0f, 0.0f);
    shading.geomPosition = float3(0.0f, 0.0f, 0.0f);
    shading.normal       = float3(0.0f, 0.0f, 0.0f);
    shading.texCoord     = float2(0.0f, 0.0f);
    shading.tangent      = float3(0.0f, 0.0f, 0.0f);

    // Load the indices for the vertices of the triangle with the specified index.
    uint3 indices = geometry.getIndices(triangleIndex);

    // Compute interpolated values for each of the data channels at the specified barycentric
    // coordinates. As byte address buffers, the data arrays must be indexed by byte, and loaded
    // as 32-bit unsigned ints, which are then read as floats.
    float3 positions[3];
    float3 normals[3];
    float3 tangents[3];
    for (uint i = 0; i < 3; i++)
    {
        positions[i] = (geometry.getPosition(indices[i]));

        // Accumulate the positions, weighted by the barycentric coordinates.
        // NOTE: An alternative is to compute the position with ray intrinsics, as follows:
        // WorldRayOrigin() + WorldRayDirection() * RayTCurrent(). However this will be less
        // accurate due to floating-point precision; see "Ray Tracing Gems" chapter 6.
        shading.geomPosition += positions[i] * barycentrics[i];
        
        // Accumulate the optional normals, weighted by the barycentric coordinates.
        if (geometry.hasNormals())
        {
            normals[i] = (geometry.getNormal(indices[i]));
            shading.normal += normals[i] * barycentrics[i];
        }

        // Accumulate the optional normals, weighted by the barycentric coordinates.
        if (geometry.hasTangents())
        {
            tangents[i] = (geometry.getTangent(indices[i]));
            shading.tangent += tangents[i] * barycentrics[i];
        }

        // Accumulate the optional texture coordinates, weighted by the barycentric coordinates.
        if (geometry.hasTexCoords())
        {
            shading.texCoord += (geometry.getTexCoord(indices[i])) * barycentrics[i];
        }
    }

    // Normalize the interpolated (shading) normal. If no normals are provided, compute a geometric
    // normal using the vertex positions; this will yield flat shading.
    // NOTE: If a provided normal has zero length, then the normal will have undefined components
    // (likely NaN), leading to rendering artifacts (likely black pixels) along any path that
    // accesses the normal.
    float3 objectNormal = normalize(geometry.hasNormals()
            ? shading.normal
            : cross(positions[1] - positions[0], positions[2] - positions[0]));

    // Compute the shading position from the geometry position and vertex data. If no normals are
    // provided, use the geometric position.
    float3 objectPosition = geometry.hasNormals() ? computeShadingPosition(shading.geomPosition,
                                                   shading.normal, positions, normals, barycentrics)
                                                  : shading.geomPosition;

    // Transform the position, geometric position, and normal to world space.
    // NOTE: Renormalize the normal as the object-to-world matrix can have a scale.
    shading.position     = mul(objToWorld, float4(objectPosition, 1.0f));
    shading.geomPosition = mul(objToWorld, float4(shading.geomPosition, 1.0f));
    shading.normal       = normalize(mul((float3x3)objToWorld, objectNormal));
    
    // arbitrary up vector used to calculate tangents, if none provided.
    float3 up =
        abs(shading.normal.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);

    // Compute arbitrary shading tangent from up vector, or use provided one transformed in world space.
    shading.tangent = normalize(geometry.hasTangents() ? normalize(mul((float3x3)objToWorld, shading.tangent))
                                                       : cross(shading.normal, up));
    // NOTE: Using the provided geometry tangents will make unit tests fail, because the code used for generating the
    //       golden images seemed to simply always compute the tangent as normalize(cross(shading.normal, up)) rather
    //       than using the geometry tangents.

    // Generate bitangent from normal and tangent.
    shading.bitangent = computeBitangent(shading.normal, shading.tangent);
    shading.bitangentWorld = shading.bitangent; // Keep MaterialX (>= 1.39.x) alias in sync

    shading.object_to_world = to4x4(objToWorld);
    shading.world_to_object = to4x4(worldToObj);

    shading.indices = indices;

    shading.barycentrics = barycentrics;

    // MDL specific state
    float3 geomNormalObj = normalize(cross(positions[1] - positions[0], positions[2] - positions[0]));
    shading.geom_normal = normalize(mul((float3x3)objToWorld, geomNormalObj));
    shading.animation_time = 0.0f;
    // shading.text_results doesn't need to be initialized
    shading.ro_data_segment_offset = 0;
    shading.object_id = 0;
    shading.meters_per_scene_unit = 1.0f;
    shading.arg_block_offset = 0;

    return shading;
}

#endif // __GEOMETRY_H__