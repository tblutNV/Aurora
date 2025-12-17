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
#ifndef __MDL_TARGET_CODE_TYPES_H__
#define __MDL_TARGET_CODE_TYPES_H__

// The MDL shading state struct is defined in Geometry.slang

/// The texture wrap modes as defined by \c tex::wrap_mode in the MDL specification.
#define Tex_wrap_mode            int
#define TEX_WRAP_CLAMP           0
#define TEX_WRAP_REPEAT          1
#define TEX_WRAP_MIRRORED_REPEAT 2
#define TEX_WRAP_CLIP            3

/// The type of events created by BSDF importance sampling.
#define Bsdf_event_type   int
#define BSDF_EVENT_ABSORB 0

#define BSDF_EVENT_DIFFUSE      1
#define BSDF_EVENT_GLOSSY       (1 << 1)
#define BSDF_EVENT_SPECULAR     (1 << 2)
#define BSDF_EVENT_REFLECTION   (1 << 3)
#define BSDF_EVENT_TRANSMISSION (1 << 4)

#define BSDF_EVENT_DIFFUSE_REFLECTION    (BSDF_EVENT_DIFFUSE | BSDF_EVENT_REFLECTION)
#define BSDF_EVENT_DIFFUSE_TRANSMISSION  (BSDF_EVENT_DIFFUSE | BSDF_EVENT_TRANSMISSION)
#define BSDF_EVENT_GLOSSY_REFLECTION     (BSDF_EVENT_GLOSSY | BSDF_EVENT_REFLECTION)
#define BSDF_EVENT_GLOSSY_TRANSMISSION   (BSDF_EVENT_GLOSSY | BSDF_EVENT_TRANSMISSION)
#define BSDF_EVENT_SPECULAR_REFLECTION   (BSDF_EVENT_SPECULAR | BSDF_EVENT_REFLECTION)
#define BSDF_EVENT_SPECULAR_TRANSMISSION (BSDF_EVENT_SPECULAR | BSDF_EVENT_TRANSMISSION)

#define Edf_event_type int
#define EDF_EVENT_NONE 0

#define EDF_EVENT_EMISSION 1

/// MBSDFs can consist of two parts, which can be selected using this enumeration.
#define Mbsdf_part              int
#define MBSDF_DATA_REFLECTION   0
#define MBSDF_DATA_TRANSMISSION 1

/// The calling code can mark the \c x component of an IOR field in *_data with
/// \c BSDF_USE_MATERIAL_IOR, to make the BSDF functions use the MDL material's IOR
/// for this IOR field.
#define BSDF_USE_MATERIAL_IOR (-1.0f)

/// Flags controlling the calculation of DF results.
/// This cannot be represented as a real enum, because the MDL SDK HLSL backend only sees enums
/// as ints on LLVM level and would create wrong types for temporary variables
#define Df_flags                            int
#define DF_FLAGS_NONE                       0 ///< allows nothing -> black
#define DF_FLAGS_ALLOW_REFLECT              1
#define DF_FLAGS_ALLOW_TRANSMIT             2
#define DF_FLAGS_ALLOW_REFLECT_AND_TRANSMIT (DF_FLAGS_ALLOW_REFLECT | DF_FLAGS_ALLOW_TRANSMIT)
#define DF_FLAGS_ALLOWED_SCATTER_MODE_MASK  (DF_FLAGS_ALLOW_REFLECT_AND_TRANSMIT)

/// Input and output structure for BSDF sampling data.
struct Bsdf_sample_data {
    float3 ior1;                    ///< mutual input: IOR current medium
    float3 ior2;                    ///< mutual input: IOR other side
    float3 k1;                      ///< mutual input: outgoing direction

    float3 k2;                      ///< output: incoming direction
    float4 xi;                      ///< input: pseudo-random sample numbers in range [0, 1)
    float pdf;                      ///< output: pdf (non-projected hemisphere)
    float3 bsdf_over_pdf;           ///< output: bsdf * dot(normal, k2) / pdf
    Bsdf_event_type event_type;     ///< output: the type of event for the generated sample
    int handle;                     ///< output: handle of the sampled elemental BSDF (lobe)

    Df_flags flags;                 ///< input: flags controlling calculation of result
                                    ///<     (optional depending on backend options)
};

/// Input and output structure for BSDF evaluation data.
struct Bsdf_evaluate_data {
    float3 ior1;                    ///< mutual input: IOR current medium
    float3 ior2;                    ///< mutual input: IOR other side
    float3 k1;                      ///< mutual input: outgoing direction

    float3 k2;                      ///< input: incoming direction
    float3 bsdf_diffuse;            ///< output: (diffuse part of the) bsdf * dot(normal, k2)
    float3 bsdf_glossy;             ///< output: (glossy part of the) bsdf * dot(normal, k2)
    float pdf;                      ///< output: pdf (non-projected hemisphere)

    Df_flags flags;                 ///< input: flags controlling calculation of result
                                    ///<     (optional depending on backend options)
};

/// Input and output structure for BSDF PDF calculation data.
struct Bsdf_pdf_data {
    float3 ior1;                    ///< mutual input: IOR current medium
    float3 ior2;                    ///< mutual input: IOR other side
    float3 k1;                      ///< mutual input: outgoing direction

    float3 k2;                      ///< input: incoming direction
    float pdf;                      ///< output: pdf (non-projected hemisphere)

    Df_flags flags;                 ///< input: flags controlling calculation of result
                                    ///<     (optional depending on backend options)
};

/// Input and output structure for BSDF auxiliary calculation data.
struct Bsdf_auxiliary_data {
    float3 ior1;                    ///< mutual input: IOR current medium
    float3 ior2;                    ///< mutual input: IOR other side
    float3 k1;                      ///< mutual input: outgoing direction

    float3 albedo_diffuse;          ///< output: (diffuse part of the) albedo
    float3 albedo_glossy;           ///< output: (glossy part of the) albedo
    float3 normal;                  ///< output: normal

    float3 roughness;               ///< output: glossy rougness_u,
                                    ///<     glossy roughness_v, bsdf_weight

    Df_flags flags;                 ///< input: flags controlling calculation of result
                                    ///<     (optional depending on backend options)
};

/// Input and output structure for EDF sampling data.
struct Edf_sample_data
{
    float4 xi;                      ///< input: pseudo-random sample numbers in range [0, 1)
    float3 k1;                      ///< output: outgoing direction
    float pdf;                      ///< output: pdf (non-projected hemisphere)
    float3 edf_over_pdf;            ///< output: edf * dot(normal,k1) / pdf
    Edf_event_type event_type;      ///< output: the type of event for the generated sample
    int handle;                     ///< output: handle of the sampled elemental EDF (lobe)
};

/// Input and output structure for EDF evaluation data.
struct Edf_evaluate_data
{
    float3 k1;                      ///< input: outgoing direction
    float cos;                      ///< output: dot(normal, k1)
    float3 edf;                     ///< output: edf
    float pdf;                      ///< output: pdf (non-projected hemisphere)
};

/// Input and output structure for EDF PDF calculation data.
struct Edf_pdf_data
{
    float3 k1;                      ///< input: outgoing direction
    float pdf;                      ///< output: pdf (non-projected hemisphere)
};

/// Input and output structure for EDF PDF calculation data.
struct Edf_auxiliary_data
{
    float3 k1;                      ///< input: outgoing direction

    // reserved for future use
};

#endif // __MDL_TARGET_CODE_TYPES_H__
