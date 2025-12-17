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
#ifndef __MDL_RENDERER_RUNTIME_H__
#define __MDL_RENDERER_RUNTIME_H__

#include "MDLTargetCodeTypes.slang"
#include "Geometry.slang"
#include "GlobalPipelineState.slang"
#include "GlobalBufferAccessors.slang"
#include "InstancePipelineState.slang"

// MDL defines UV origin bottom left, D3D top left.
#if !defined(TEXTURE_VERTICAL_FLIP)
#define TEXTURE_VERTICAL_FLIP 1
#endif

// ------------------------------------------------------------------------------------------------
// Argument block access for dynamic parameters in class compilation mode
// ------------------------------------------------------------------------------------------------

int argblock_offset(int offs)
{
    return gInstanceMaterialBufferOffset + kMaterialHeaderSize + gInstanceMaterialArgumentsOffset + offs;
}

float mdl_read_argblock_as_float(int offs)
{
    return asfloat(gGlobalMaterialConstants.Load(argblock_offset(offs)));
}

double mdl_read_argblock_as_double(int offs)
{
    return asdouble(
        gGlobalMaterialConstants.Load(argblock_offset(offs)),
        gGlobalMaterialConstants.Load(argblock_offset(offs + 4)));
}

int mdl_read_argblock_as_int(int offs)
{
    return asint(gGlobalMaterialConstants.Load(argblock_offset(offs)));
}

uint mdl_read_argblock_as_uint(int offs)
{
    return gGlobalMaterialConstants.Load(argblock_offset(offs));
}

bool mdl_read_argblock_as_bool(int offs)
{
    uint val = gGlobalMaterialConstants.Load(argblock_offset(offs & ~3));
    return (val & (0xffU << (8 * (offs & 3)))) != 0;
}


int rodata_offset(int offs)
{
    return gInstanceMaterialBufferOffset + kMaterialHeaderSize + offs;
}

float mdl_read_rodata_as_float(int offs)
{
    return asfloat(gGlobalMaterialConstants.Load(rodata_offset(offs)));
}

double mdl_read_rodata_as_double(int offs)
{
    return asdouble(
        gGlobalMaterialConstants.Load(rodata_offset(offs)),
        gGlobalMaterialConstants.Load(rodata_offset(offs + 4)));
}

int mdl_read_rodata_as_int(int offs)
{
    return asint(gGlobalMaterialConstants.Load(rodata_offset(offs)));
}

uint mdl_read_rodata_as_uint(int offs)
{
    return gGlobalMaterialConstants.Load(rodata_offset(offs));
}

bool mdl_read_rodata_as_bool(int offs)
{
    uint val = gGlobalMaterialConstants.Load(rodata_offset(offs & ~3));
    return (val & (0xffU << (8 * (offs & 3)))) != 0;
}

// ------------------------------------------------------------------------------------------------
// Texturing functions, check if valid
// ------------------------------------------------------------------------------------------------

// corresponds to ::tex::texture_isvalid(uniform texture_2d tex)
// corresponds to ::tex::texture_isvalid(uniform texture_3d tex)
// corresponds to ::tex::texture_isvalid(uniform texture_cube tex) // not supported by this example
// corresponds to ::tex::texture_isvalid(uniform texture_ptex tex) // not supported by this example
bool tex_texture_isvalid(int tex)
{
    // assuming that there is no indexing out of bounds of the resource_infos and the view arrays
    return tex != 0; // invalid texture
}

// helper function to realize wrap and crop.
// Out of bounds case for TEX_WRAP_CLIP must already be handled.
float apply_wrap_and_crop(
    float coord,
    int wrap,
    float2 crop,
    int res)
{
    if (wrap != TEX_WRAP_REPEAT || any(crop != float2(0, 1)))
    {
        if (wrap == TEX_WRAP_REPEAT)
        {
            coord -= floor(coord);
        }
        else
        {
            if (wrap == TEX_WRAP_MIRRORED_REPEAT)
            {
                float floored_val = floor(coord);
                if ((int(floored_val) & 1) != 0)
                    coord = 1 - (coord - floored_val);
                else
                    coord -= floored_val;
            }
            float inv_hdim = 0.5f / float(res);
            coord = clamp(coord, inv_hdim, 1.f - inv_hdim);
        }
        coord = coord * (crop.y - crop.x) + crop.x;
    }
    // Manually implement repeat here. The sampler is configured to clamp.
    // Because frac(1.0) = 0.0, we need to handle this separately.
    // floor(0.0) needs to be 0.
    // floor(n) is 1, which is achieved by the clamp mode.
    float truncated = floor(coord);
    return coord == truncated ? truncated : frac(coord);
}

// Modify texture coordinates to get better texture filtering,
// see http://www.iquilezles.org/www/articles/texture/texture.htm
float2 apply_smootherstep_filter(float2 uv, uint2 size)
{
    float2 res;
    res = uv * size + 0.5f;

    float u_i = floor(res.x), v_i = floor(res.y);
    float u_f = res.x - u_i;
    float v_f = res.y - v_i;
    u_f = u_f * u_f * u_f * (u_f * (u_f * 6.f - 15.f) + 10.f);
    v_f = v_f * v_f * v_f * (v_f * (v_f * 6.f - 15.f) + 10.f);
    res.x = u_i + u_f;
    res.y = v_i + v_f;

    res = (res - 0.5f) / size;
    return res;
}

// ------------------------------------------------------------------------------------------------
// Texturing functions, 2D
// ------------------------------------------------------------------------------------------------

int2 tex_res_2d(int tex, int2 uv_tile, float frame)
{
    if (tex == 0) return int2(0, 0); // invalid texture

    int index = materialTextureIndex(gInstanceMaterialBufferOffset, tex - 1);

    uint2 res;
    gGlobalMaterialTextures[NonUniformResourceIndex(index)].GetDimensions(res.x, res.y);
    return int2(res);
    
}

// corresponds to ::tex::width(uniform texture_2d tex, int2 uv_tile, float frame)
int tex_width_2d(int tex, int2 uv_tile, float frame)
{
    return tex_res_2d(tex, uv_tile, frame).x;
}

// corresponds to ::tex::height(uniform texture_2d tex, int2 uv_tile)
int tex_height_2d(int tex, int2 uv_tile, float frame)
{
    return tex_res_2d(tex, uv_tile, frame).y;
}

// corresponds to ::tex::first_frame(uniform texture_2d)
int tex_first_frame_2d(int tex)
{
    return 0;
}

// corresponds to ::tex::last_frame(uniform texture_2d)
int tex_last_frame_2d(int tex)
{
    return 0;
}

// corresponds to ::tex::lookup_float4(uniform texture_2d tex, float2 coord, ...)
float4 tex_lookup_float4_2d(
    int tex,
    float2 coord,
    int wrap_u,
    int wrap_v,
    float2 crop_u,
    float2 crop_v,
    float frame)
{
    if (tex == 0) return float4(0, 0, 0, 0); // invalid texture

    if (wrap_u == TEX_WRAP_CLIP && (coord.x < 0.0 || coord.x >= 1.0))
        return float4(0, 0, 0, 0);
    if (wrap_v == TEX_WRAP_CLIP && (coord.y < 0.0 || coord.y >= 1.0))
        return float4(0, 0, 0, 0);

    int index = materialTextureIndex(gInstanceMaterialBufferOffset, tex - 1);

    uint2 res;
    gGlobalMaterialTextures[NonUniformResourceIndex(index)].GetDimensions(res.x, res.y);
    coord.x = apply_wrap_and_crop(coord.x, wrap_u, crop_u, res.x);
    coord.y = apply_wrap_and_crop(coord.y, wrap_v, crop_v, res.y);

    coord = apply_smootherstep_filter(coord, res);

#if (TEXTURE_VERTICAL_FLIP == 1)
    coord.y = 1.0 - coord.y;
#endif

    // Note, since we don't have ddx and ddy in the compute pipeline, TextureObject::Sample() is not
    // available, we use SampleLevel instead and go for the most detailed level. Therefore, we don't
    // need mipmaps. Manual mip level computation is possible though.
    return gGlobalMaterialTextures[NonUniformResourceIndex(index)].SampleLevel(
        gSamplerArray[0], coord, /*lod=*/0.0f, /*offset=*/int2(0, 0));
}

float3 tex_lookup_float3_2d(int tex, float2 coord, int wrap_u, int wrap_v, float2 crop_u, float2 crop_v, float frame)
{
    return tex_lookup_float4_2d(tex, coord, wrap_u, wrap_v, crop_u, crop_v, frame).xyz;
}

float3 tex_lookup_color_2d(int tex, float2 coord, int wrap_u, int wrap_v, float2 crop_u, float2 crop_v, float frame)
{
    return tex_lookup_float4_2d(tex, coord, wrap_u, wrap_v, crop_u, crop_v, frame).xyz;
}

float2 tex_lookup_float2_2d(int tex, float2 coord, int wrap_u, int wrap_v, float2 crop_u, float2 crop_v, float frame)
{
    return tex_lookup_float4_2d(tex, coord, wrap_u, wrap_v, crop_u, crop_v, frame).xy;
}

float tex_lookup_float_2d(int tex, float2 coord, int wrap_u, int wrap_v, float2 crop_u, float2 crop_v, float frame)
{
    return tex_lookup_float4_2d(tex, coord, wrap_u, wrap_v, crop_u, crop_v, frame).x;
}

// corresponds to ::tex::texel_float4(uniform texture_2d tex, float2 coord, int2 uv_tile)
float4 tex_texel_float4_2d(
    int tex,
    int2 coord,
    int2 uv_tile,
    float frame)
{
    if (tex == 0) return float4(0, 0, 0, 0); // invalid texture

    int index = materialTextureIndex(gInstanceMaterialBufferOffset, tex - 1);

    uint2 res;
    gGlobalMaterialTextures[NonUniformResourceIndex(index)].GetDimensions(res.x, res.y);
    if (0 > coord.x || res.x <= coord.x || 0 > coord.y || res.y <= coord.y)
        return float4(0, 0, 0, 0); // out of bounds

#if (TEXTURE_VERTICAL_FLIP == 1)
    coord.y = res.y - coord.y - 1;
#endif

    return gGlobalMaterialTextures[NonUniformResourceIndex(index)].Load(
        int3(coord, /*mipmaplevel=*/0));
}

float3 tex_texel_float3_2d(int tex, int2 coord, int2 uv_tile, float frame)
{
    return tex_texel_float4_2d(tex, coord, uv_tile, frame).xyz;
}

float3 tex_texel_color_2d(int tex, int2 coord, int2 uv_tile, float frame)
{
    return tex_texel_float3_2d(tex, coord, uv_tile, frame);
}

float2 tex_texel_float2_2d(int tex, int2 coord, int2 uv_tile, float frame)
{
    return tex_texel_float4_2d(tex, coord, uv_tile, frame).xy;
}

float tex_texel_float_2d(int tex, int2 coord, int2 uv_tile, float frame)
{
    return tex_texel_float4_2d(tex, coord, uv_tile, frame).x;
}

// ------------------------------------------------------------------------------------------------
// Texturing functions, 3D
// ------------------------------------------------------------------------------------------------

int3 tex_res_3d(int tex, float frame)
{
    if (tex == 0) return uint3(0, 0, 0); // invalid texture

    int index = materialTextureIndex(gInstanceMaterialBufferOffset, tex - 1);

    uint3 res;
    gGlobalMaterialTextures3D[NonUniformResourceIndex(index)].GetDimensions(res.x, res.y, res.z);
    return int3(res);
}

// corresponds to ::tex::first__frame(uniform texture_3d)
int tex_first_frame_3d(int tex)
{
    return 0;
}

// corresponds to ::tex::last_frame(uniform texture_3d)
int tex_last_frame_3d(int tex)
{
    return 0;
}

// corresponds to ::tex::width(uniform texture_3d tex, int2 uv_tile)
int tex_width_3d(int tex, float frame) { return tex_res_3d(tex, frame).x; }

// corresponds to ::tex::height(uniform texture_3d tex, int2 uv_tile)
int tex_height_3d(int tex, float frame) { return tex_res_3d(tex, frame).y; }

// corresponds to ::tex::depth(uniform texture_3d tex, int2 uv_tile)
int tex_depth_3d(int tex, float frame) { return tex_res_3d(tex, frame).z; }

// corresponds to ::tex::lookup_float4(uniform texture_3d tex, float2 coord, ...)
float4 tex_lookup_float4_3d(
    int tex,
    float3 coord,
    int wrap_u,
    int wrap_v,
    int wrap_w,
    float2 crop_u,
    float2 crop_v,
    float2 crop_w,
    float frame)
{
    if (tex == 0) return float4(0, 0, 0, 0); // invalid texture

    if (wrap_u == TEX_WRAP_CLIP && (coord.x < 0.0 || coord.x >= 1.0))
        return float4(0, 0, 0, 0);
    if (wrap_v == TEX_WRAP_CLIP && (coord.y < 0.0 || coord.y >= 1.0))
        return float4(0, 0, 0, 0);
    if (wrap_w == TEX_WRAP_CLIP && (coord.z < 0.0 || coord.z >= 1.0))
        return float4(0, 0, 0, 0);

    int index = materialTextureIndex(gInstanceMaterialBufferOffset, tex - 1);

    uint width, height, depth;
    gGlobalMaterialTextures3D[NonUniformResourceIndex(index)].GetDimensions(width, height, depth);
    coord.x = apply_wrap_and_crop(coord.x, wrap_u, crop_u, width);
    coord.y = apply_wrap_and_crop(coord.y, wrap_v, crop_v, height);
    coord.z = apply_wrap_and_crop(coord.z, wrap_w, crop_w, depth);

    // Note, since we don't have ddx and ddy in the compute pipeline, TextureObject::Sample() is not
    // available, we use SampleLevel instead and go for the most detailed level. Therefore, we don't
    // need mipmaps. Manual mip level computation is possible though.
    return gGlobalMaterialTextures3D[NonUniformResourceIndex(index)].SampleLevel(
        gSamplerArray[0], coord, /*lod=*/0.0f, /*offset=*/int3(0, 0, 0));
}

float3 tex_lookup_float3_3d(int tex, float3 coord, int wrap_u, int wrap_v, int wrap_w, float2 crop_u, float2 crop_v, float2 crop_w, float frame)
{
    return tex_lookup_float4_3d(tex, coord, wrap_u, wrap_v, wrap_w, crop_u, crop_v, crop_w, frame).xyz;
}

float3 tex_lookup_color_3d(int tex, float3 coord, int wrap_u, int wrap_v, int wrap_w, float2 crop_u, float2 crop_v, float2 crop_w, float frame)
{
    return tex_lookup_float4_3d(tex, coord, wrap_u, wrap_v, wrap_w, crop_u, crop_v, crop_w, frame).xyz;
}

float2 tex_lookup_float2_3d(int tex, float3 coord, int wrap_u, int wrap_v, int wrap_w, float2 crop_u, float2 crop_v, float2 crop_w, float frame)
{
    return tex_lookup_float4_3d(tex, coord, wrap_u, wrap_v, wrap_w, crop_u, crop_v, crop_w, frame).xy;
}

float tex_lookup_float_3d(int tex, float3 coord, int wrap_u, int wrap_v, int wrap_w, float2 crop_u, float2 crop_v, float2 crop_w, float frame)
{
    return tex_lookup_float4_3d(tex, coord, wrap_u, wrap_v, wrap_w, crop_u, crop_v, crop_w, frame).x;
}

// corresponds to ::tex::texel_float4(uniform texture_3d tex, float3 coord)
float4 tex_texel_float4_3d(int tex, int3 coord, float frame)
{
    if (tex == 0) return float4(0, 0, 0, 0); // invalid texture

    int index = materialTextureIndex(gInstanceMaterialBufferOffset, tex - 1);

    uint3 res;
    gGlobalMaterialTextures3D[NonUniformResourceIndex(index)].GetDimensions(res.x, res.y, res.z);
    if (0 > coord.x || res.x <= coord.x || 0 > coord.y || res.y <= coord.y || 0 > coord.z || res.z <= coord.z)
        return float4(0, 0, 0, 0); // out of bounds

    return gGlobalMaterialTextures3D[NonUniformResourceIndex(index)].Load(
        int4(coord, /*mipmaplevel=*/0));
}

float3 tex_texel_float3_3d(int tex, int3 coord, float frame)
{
    return tex_texel_float4_3d(tex, coord, frame).xyz;
}

float3 tex_texel_color_3d(int tex, int3 coord, float frame)
{
    return tex_texel_float3_3d(tex, coord, frame);
}

float2 tex_texel_float2_3d(int tex, int3 coord, float frame)
{
    return tex_texel_float4_3d(tex, coord, frame).xy;
}

float tex_texel_float_3d(int tex, int3 coord, float frame)
{
    return tex_texel_float4_3d(tex, coord, frame).x;
}

// ------------------------------------------------------------------------------------------------
// Texturing functions, Cube (not supported)
// ------------------------------------------------------------------------------------------------

int tex_width_cube(int tex) { return 0; }
int tex_height_cube(int tex) { return 0; }

float4 tex_lookup_float4_cube(int tex, float3 coord)
{
    return float4(0.0);
}

float3 tex_lookup_float3_cube(int tex, float3 coord)
{
    return tex_lookup_float4_cube(tex, coord).xyz;
}

float3 tex_lookup_color_cube(int tex, float3 coord)
{
    return tex_lookup_float4_cube(tex, coord).xyz;
}

float2 tex_lookup_float2_cube(int tex, float3 coord)
{
    return tex_lookup_float4_cube(tex, coord).xy;
}

float tex_lookup_float_cube(int tex, float3 coord)
{
    return tex_lookup_float4_cube(tex, coord).x;
}

float4 tex_texel_float4_cube(int tex, int3 coord)
{
    return float4(0.0);
}

float3 tex_texel_float3_cube(int tex, int3 coord)
{
    return tex_texel_float4_cube(tex, coord).xyz;
}

float3 tex_texel_color_cube(int tex, int3 coord)
{
    return tex_texel_float4_cube(tex, coord).xyz;
}

float2 tex_texel_float2_cube(int tex, int3 coord)
{
    return tex_texel_float4_cube(tex, coord).xy;
}

float tex_texel_float_cube(int tex, int3 coord)
{
    return tex_texel_float4_cube(tex, coord).x;
}

// ------------------------------------------------------------------------------------------------
// Texturing functions, PTEX (not supported)
// ------------------------------------------------------------------------------------------------

float4 tex_lookup_float4_ptex(int tex, int channel)
{
    return float4(0.0);
}

float3 tex_lookup_float3_ptex(int tex, int channel)
{
    return tex_lookup_float4_ptex(tex, channel).xyz;
}

float3 tex_lookup_color_ptex(int tex, int channel)
{
    return tex_lookup_float3_ptex(tex, channel);
}

float2 tex_lookup_float2_ptex(int tex, int channel)
{
    return tex_lookup_float4_ptex(tex, channel).xy;
}

float tex_lookup_float_ptex(int tex, int channel)
{
    return tex_lookup_float4_ptex(tex, channel).x;
}

// ------------------------------------------------------------------------------------------------
// Light Profiles (not supported)
// ------------------------------------------------------------------------------------------------

bool df_light_profile_isvalid(int lp_idx)
{
    // assuming that there is no indexing out of bounds of the light_profile_infos and the view arrays
    return lp_idx != 0; // 0 is the invalid light profile
}

float df_light_profile_power(int lp_idx)
{
    return 0.0;
}

float df_light_profile_maximum(int lp_idx)
{
    return 0.0;
}

float df_light_profile_evaluate(int lp_idx, float2 theta_phi)
{
    return 0.0;
}

float3 df_light_profile_sample(int lp_idx, float3 xi)
{
    return float3(
        -1.0,  // negative theta (x value) means no emission
        -1.0,
         0.0); // invalid light profile
}

float df_light_profile_pdf(int lp_idx, float2 theta_phi)
{
    return 0.0;
}

// ------------------------------------------------------------------------------------------------
// Measured BSDFs (not supported)
// ------------------------------------------------------------------------------------------------

bool df_bsdf_measurement_isvalid(int bm_idx)
{
    // assuming that there is no indexing out of bounds of the mbsdf_infos and the view arrays
    return bm_idx != 0; // 0 is the invalid bsdf measurement
}

int3 df_bsdf_measurement_resolution(int bm_idx, int part)
{
    return int3(0);
}

float3 df_bsdf_measurement_evaluate(
    int bm_idx,
    float2 theta_phi_in,
    float2 theta_phi_out,
    int part)
{
    return float3(0.0);
}

// output: theta, phi, pdf
float3 df_bsdf_measurement_sample(
    int bm_idx,
    float2 theta_phi_out,
    float3 xi,
    int part)
{
    return float3(
        -1.0,  // negative theta (x value) means absorption
        -1.0,
         0.0); // invalid bsdf measurement
}

float df_bsdf_measurement_pdf(
    int bm_idx,
    float2 theta_phi_in,
    float2 theta_phi_out,
    int part)
{
    return 0.0;
}

// output: max (in case of color) albedo for the selected direction (x) and global (y)
float2 df_bsdf_measurement_albedo(int bm_idx, float2 theta_phi, int part)
{
    return float2(0.0);
}

float4 df_bsdf_measurement_albedos(int bm_idx, float2 theta_phi)
{
    return float4(0.0);
}

// ------------------------------------------------------------------------------------------------
// Scene Data API (not supported)
// ------------------------------------------------------------------------------------------------

bool scene_data_isvalid(
    inout ShadingData state, // MDL state that also contains a custom renderer state
    int scene_data_id)       // the scene_data_id (from target code or manually added)
{
    return false;
}

float4 scene_data_lookup_floatX(
    ShadingData state,        // MDL state that also contains a custom renderer state
    int scene_data_id,        // the scene_data_id (from target code or manually added)
    float4 default_value,     // default value in case the requested data is not valid
    bool uniform_lookup,      // true if a uniform lookup is requested
    int number_of_components) // 1, 2, 3, or 4
{
    // dummy implementation
    return default_value;
}

float4 scene_data_lookup_float4(
    inout ShadingData state,
    int scene_data_id,
    float4 default_value,
    bool uniform_lookup)
{
    return scene_data_lookup_floatX(state, scene_data_id, default_value, uniform_lookup, 4);
}

float3 scene_data_lookup_float3(
    inout ShadingData state,
    int scene_data_id,
    float3 default_value,
    bool uniform_lookup)
{
    return scene_data_lookup_floatX(state, scene_data_id, default_value.xyzx, uniform_lookup, 3).xyz;
}

float3 scene_data_lookup_color(
    inout ShadingData state,
    int scene_data_id,
    float3 default_value,
    bool uniform_lookup)
{
    return scene_data_lookup_floatX(state, scene_data_id, default_value.xyzx, uniform_lookup, 3).xyz;
}

float2 scene_data_lookup_float2(
    inout ShadingData state,
    int scene_data_id,
    float2 default_value,
    bool uniform_lookup)
{
    return scene_data_lookup_floatX(state, scene_data_id, default_value.xyxx, uniform_lookup, 2).xy;
}

float scene_data_lookup_float(
    inout ShadingData state,
    int scene_data_id,
    float default_value,
    bool uniform_lookup)
{
    return scene_data_lookup_floatX(state, scene_data_id, default_value.xxxx, uniform_lookup, 1).x;
}

float4x4 scene_data_lookup_float4x4(
    inout ShadingData state,
    int scene_data_id,
    float4x4 default_value,
    bool uniform_lookup)
{
    // dummy implementation
    return default_value;
}

int4 scene_data_lookup_intX(
    ShadingData state,
    int scene_data_id,
    int4 default_value,
    bool uniform_lookup,
    int number_of_components)
{
    // dummy implementation
    return default_value;
}

int4 scene_data_lookup_int4(
    inout ShadingData state,
    int scene_data_id,
    int4 default_value,
    bool uniform_lookup)
{
    return scene_data_lookup_intX(state, scene_data_id, default_value, uniform_lookup, 4);
}

int3 scene_data_lookup_int3(
    inout ShadingData state,
    int scene_data_id,
    int3 default_value,
    bool uniform_lookup)
{
    return scene_data_lookup_intX(state, scene_data_id, default_value.xyzx, uniform_lookup, 3).xyz;
}

int2 scene_data_lookup_int2(
    inout ShadingData state,
    int scene_data_id,
    int2 default_value,
    bool uniform_lookup)
{
    return scene_data_lookup_intX(state, scene_data_id, default_value.xyxx, uniform_lookup, 2).xy;
}

int scene_data_lookup_int(
    inout ShadingData state,
    int scene_data_id,
    int default_value,
    bool uniform_lookup)
{
    return scene_data_lookup_intX(state, scene_data_id, default_value.xxxx, uniform_lookup, 1).x;
}

#endif // __MDL_RENDERER_RUNTIME_H__
