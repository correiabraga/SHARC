/*
 * Copyright (c) 2023-2026, NVIDIA CORPORATION. All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

// Version
#define SHARC_VERSION_MAJOR                     1
#define SHARC_VERSION_MINOR                     8
#define SHARC_VERSION_BUILD                     3
#define SHARC_VERSION_REVISION                  0

// SHaRC usage overview
//
// SHaRC uses a world-space radiance cache to reduce path tracing cost. The
// integration typically consists of three render-time passes:
//
// 1. SHaRC Update
//    Sparse RT pass, compiled with SHARC_UPDATE=1, that traces a subset of
//    paths and writes newly observed radiance into the accumulation cache.
//    Call SharcInit() for each sampled path, SharcUpdateHit() on hits, and
//    SharcUpdateMiss() on misses. If SharcUpdateHit() returns false, the path
//    must be terminated early. Each path segment is treated independently:
//    after selecting the next ray, call SharcSetThroughput() with the segment
//    throughput, then reset path throughput to 1.0 for the next segment.
//
// 2. SHaRC Resolve
//    Compute pass that calls SharcResolveEntry() for each cache entry. This
//    combines per-frame accumulation data with previously resolved data,
//    updates temporal accumulation, handles stale entry eviction, and clears
//    accumulation data for the next frame.
//
// 3. SHaRC Render / Query
//    Main render RT pass, compiled with SHARC_QUERY=1, that traces normally but
//    queries the resolved cache on eligible non-primary hits using
//    SharcGetCachedRadiance(). On a successful query, cached radiance is used
//    and the path can terminate early.
//
// Required resources are hash entries, accumulation, and resolved buffers.
// All buffers must have the same number of entries and be zero-initialized.
// Proper UAV barriers are required between passes to guarantee correctness.
//
// HLSL shaders that include SHARC headers must be compiled with DXC
// -enable-16bit-types and, for DXIL, Shader Model 6.2 or newer.

// Constants
#define SHARC_ACCUMULATED_FRAME_NUM_BIT_OFFSET  0
#define SHARC_ACCUMULATED_FRAME_NUM_BIT_NUM     16
#define SHARC_ACCUMULATED_FRAME_NUM_BIT_MASK    ((1 << SHARC_ACCUMULATED_FRAME_NUM_BIT_NUM) - 1)
#define SHARC_STALE_FRAME_NUM_BIT_OFFSET        16
#define SHARC_STALE_FRAME_NUM_BIT_NUM           16
#define SHARC_STALE_FRAME_NUM_BIT_MASK          ((1 << SHARC_STALE_FRAME_NUM_BIT_NUM) - 1)
#define SHARC_GRID_LOGARITHM_BASE               2.0f
#define SHARC_ACCUMULATED_FRAME_NUM_MIN         1       // minimum number of frames to use for data accumulation
#define SHARC_ACCUMULATED_FRAME_NUM_MAX         1024    // maximum number of frames to use for data accumulation
#define SHARC_STALE_FRAME_NUM_MAX               1024    // maximum number of frames without new samples before the cache entry is evicted

#if SHARC_ENABLE_RESPONSIVE_LIGHTING
#define SHARC_CACHE_INDEX_BIT_NUM               26
#define SHARC_CACHE_INDEX_BIT_MASK              ((1 << SHARC_CACHE_INDEX_BIT_NUM) - 1)
#define SHARC_RESPONSIVE_INDEX_OFFSET_BIT_NUM   6
#define SHARC_RESPONSIVE_INDEX_OFFSET_BIT_MASK  ((1 << SHARC_RESPONSIVE_INDEX_OFFSET_BIT_NUM) - 1)
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING

// Tweakable parameters
#ifndef SHARC_SAMPLE_NUM_THRESHOLD
#define SHARC_SAMPLE_NUM_THRESHOLD              0       // elements with sample count above this threshold will be used for early-out/resampling
#endif

#ifndef SHARC_SEPARATE_EMISSIVE
#define SHARC_SEPARATE_EMISSIVE                 0       // if enabled, emissive values must be provided separately during updates. For cache queries, you can either supply them directly or include them in the query result
#endif

#ifndef SHARC_MATERIAL_DEMODULATION
#define SHARC_MATERIAL_DEMODULATION             0       // enable material demodulation to preserve material details; requires sampling material data to reconstruct shading from cached values
#endif

#ifndef SHARC_ENABLE_SH_ENCODING
#define SHARC_ENABLE_SH_ENCODING                0       // store resolved radiance as mixed YCoCg SH2 data: Y L0/L1 plus Co/Cg L0
#endif

#ifndef SHARC_LINEAR_PROBE_WINDOW_SIZE
#define SHARC_LINEAR_PROBE_WINDOW_SIZE          8       // size of the linear search window for probe lookups
#endif

#ifndef SHARC_ENABLE_CACHE_RESAMPLING
#define SHARC_ENABLE_CACHE_RESAMPLING           SHARC_UPDATE // resamples the cache during update step
#endif

#ifndef SHARC_PROPAGATION_DEPTH
#if SHARC_ENABLE_CACHE_RESAMPLING
#define SHARC_PROPAGATION_DEPTH                 2       // controls the amount of vertices stored in memory for signal backpropagation with cache resampling
#else // !SHARC_ENABLE_CACHE_RESAMPLING
#define SHARC_PROPAGATION_DEPTH                 4       // controls the amount of vertices stored in memory for signal backpropagation
#endif // SHARC_ENABLE_CACHE_RESAMPLING
#endif

#ifndef SHARC_BLEND_ADJACENT_LEVELS
#define SHARC_BLEND_ADJACENT_LEVELS             1       // combine the data from adjacent levels on camera movement
#endif

#ifndef SHARC_ENABLE_FADE_ACCELERATION
#define SHARC_ENABLE_FADE_ACCELERATION          0       // boost accumulator convergence when signal amplitude is fading
#endif

#ifndef SHARC_RESAMPLING_DEPTH_MIN
#define SHARC_RESAMPLING_DEPTH_MIN              1       // controls minimum path depth which can be used with cache resampling
#endif

#ifndef SHARC_STALE_FRAME_NUM_MIN
#define SHARC_STALE_FRAME_NUM_MIN               8       // minimum number of frames to keep the element in the cache
#endif

#ifndef SHARC_GRID_LEVEL_BIAS
#define SHARC_GRID_LEVEL_BIAS                   0       // LOD bias - positive adds extra magnified levels, negative reduces levels
#endif

#ifndef SHARC_USE_FP16
#define SHARC_USE_FP16                          0       // use native fp16 for sample weights storage; HLSL requires DXC -enable-16bit-types
#endif

#ifndef SHARC_RESPONSIVE_ENTRY_PROBE_RANGE
#define SHARC_RESPONSIVE_ENTRY_PROBE_RANGE      16      // maximum depth for responsive entry search, deeper search is not justified
#endif

#ifndef HASH_GRID_COMPACT
#define HASH_GRID_COMPACT                       0
#endif

#ifndef HASH_GRID_LIMIT_EMPTY_SLOTS
#define HASH_GRID_LIMIT_EMPTY_SLOTS             2
#endif

// Values of SHARC_ENABLE_FILTER_MODE
#define SHARC_FILTER_DISABLED                   0
#define SHARC_STANDARD_DEVIATION_FILTER         1
#define SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER  2

#ifndef SHARC_ENABLE_FILTER_MODE
#define SHARC_ENABLE_FILTER_MODE                SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER // per-voxel luminance statistic and the deposit filter of SharcAddVoxelData built on it (SharcParameters::fireflyWidth): SHARC_FILTER_DISABLED = neither; SHARC_STANDARD_DEVIATION_FILTER = EWMA second moment M2, clamp above a Hoelder mean + width * sigma; SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER = streaming median and MAD of ln(luminance), clamp above the band edge median * exp(MAD)^(1.4826 width)
#endif

#ifdef SHARC_ENABLE_STANDARD_DEVIATION
#error "SHARC_ENABLE_STANDARD_DEVIATION was replaced by SHARC_ENABLE_FILTER_MODE: SHARC_STANDARD_DEVIATION_FILTER (1) for the M2 statistic, SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER (2) for the median / MAD"
#endif

#if SHARC_ENABLE_FILTER_MODE != SHARC_FILTER_DISABLED && SHARC_ENABLE_FADE_ACCELERATION
#error "SHARC_ENABLE_FILTER_MODE keeps its luminance statistic in SharcPackedData::sampleDataExt, the word SHARC_ENABLE_FADE_ACCELERATION uses for its fading bitmask: enable one or the other"
#endif

#define SHARC_LOG_MAD_HALF_WINDOW_MIN           1       // bounds of SharcResolveParameters::logMadHalfWindow N: the streaming median and MAD track with a noise-equivalent window of w = 2N + 1 contributing frames (steps 3.72 / w and 2.33 / w)
#define SHARC_LOG_MAD_HALF_WINDOW_MAX           15

#ifndef SHARC_LOG_MAD_MIN
#define SHARC_LOG_MAD_MIN                       0.02f   // floor on the stored MAD of ln(luminance) (~2% luminance), so a stored MAD of exactly 0 means "no observation yet"
#endif

#ifndef SHARC_LOG_MAD_INITIAL
#define SHARC_LOG_MAD_INITIAL                   0.5f    // MAD seed on a voxel's first observed (non-black) frame, in ln units
#endif

#ifndef SHARC_SIGMA_FLOOR_FRACTION
#define SHARC_SIGMA_FLOOR_FRACTION              0.05f   // the standard deviation filter floors sigma at this fraction of the mean luminance, so a voxel with a near-constant history cannot clamp everything
#endif

#ifndef RW_STRUCTURED_BUFFER
#define RW_STRUCTURED_BUFFER(name, type)        RWStructuredBuffer<type> name
#endif

#ifndef BUFFER_AT_OFFSET
#define BUFFER_AT_OFFSET(name, offset)          name[offset]
#endif

#if SHARC_USE_FP16
#define SharcSampleWeight float16_t3
#else // !SHARC_USE_FP16
#define SharcSampleWeight float3
#endif // SHARC_USE_FP16

#if HASH_GRID_COMPACT
#define HashGridKey uint
#else // !HASH_GRID_COMPACT
#define HashGridKey uint64_t
#endif // !HASH_GRID_COMPACT
#define HASH_GRID_KEY_TYPE HashGridKey
#define HASH_GRID_PREFIX HashGrid
#define HASH_GRID_CONST_PREFIX HASH_GRID
#include "HashGridTypes.h"
#include "HashGridCommon.h"
#undef HASH_GRID_KEY_TYPE
#undef HASH_GRID_PREFIX
#undef HASH_GRID_CONST_PREFIX

// Error checks
#if SHARC_ENABLE_RESPONSIVE_LIGHTING
#if SHARC_RESPONSIVE_ENTRY_PROBE_RANGE < 1
#error SHARC_RESPONSIVE_ENTRY_PROBE_RANGE must be at least 1
#endif

#if SHARC_RESPONSIVE_ENTRY_PROBE_RANGE > HASH_GRID_HASH_MAP_BUCKET_SIZE
#error SHARC_RESPONSIVE_ENTRY_PROBE_RANGE must not exceed HASH_GRID_HASH_MAP_BUCKET_SIZE
#endif

#if HASH_GRID_COMPACT
#error compact hash grid currently doesn't work with responsive lighting
#endif
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING

#include "SharcTypes.h"

struct SharcParameters
{
    HashGridParameters hashGridParameters;
    HashGridData hashGridData;
    float radianceScale;            // quantization factor for atomic radiance accumulation (u32 per channel during SHARC_UPDATE). Start with 1e3f; reduce for large radiance values to prevent overflow
    float fireflyWidth;             // width, in standard deviations of one path's contribution, of the deposit filter selected by SHARC_ENABLE_FILTER_MODE (SharcFireflyLuminanceBound): a deposit above the bound is clamped to it. <= 0 disables
    float holderExponent;           // SHARC_STANDARD_DEVIATION_FILTER: exponent p of the Hoelder mean the bound is centred on, in [-5, 5]; 1 = the arithmetic mean, 0 = the geometric mean
    uint filterMinFrameNum;         // SHARC_STANDARD_DEVIATION_FILTER: the filter acts once this many frames have contributed and the sample weight is at least as large (clamped to [2, SHARC_ACCUMULATED_FRAME_NUM_MAX]); pass the accumulation window

    RW_STRUCTURED_BUFFER(accumulationBuffer, SharcAccumulationData);
    RW_STRUCTURED_BUFFER(resolvedBuffer, SharcPackedData);
};

struct SharcState
{
#if SHARC_UPDATE
    HashGridIndex cacheIndices[SHARC_PROPAGATION_DEPTH];
    SharcSampleWeight sampleWeights[SHARC_PROPAGATION_DEPTH];
#if SHARC_ENABLE_SH_ENCODING
    float3 radianceDirections[SHARC_PROPAGATION_DEPTH];
    float radianceDirectionWeights[SHARC_PROPAGATION_DEPTH];
#endif // SHARC_ENABLE_SH_ENCODING
    uint pathLength;
 #else // !SHARC_UPDATE
    uint placeholder;               // prevents empty-struct compilation issues with GLSL
#endif // SHARC_UPDATE
};

struct SharcHitData
{
    float3 positionWorld;
    float3 normalWorld;             // geometry normal in world space. Shading or object-space normals should work, but are not generally recommended
#if SHARC_ENABLE_SH_ENCODING
    float3 radianceDirectionWorld;  // normalized outgoing direction for cached radiance, pointing from the hit toward the previous path vertex
    float radianceDirectionWeight;  // 0 for diffuse radiance, 1 for strongly directional glossy/specular radiance
#endif // SHARC_ENABLE_SH_ENCODING
#if SHARC_MATERIAL_DEMODULATION
    float3 materialDemodulation;    // demodulation factor used to preserve material details. Use > 0 when active; set to float3(1.0f, 1.0f, 1.0f) when unused
#endif // SHARC_MATERIAL_DEMODULATION
#if SHARC_SEPARATE_EMISSIVE
    float3 emissive;                // separate emissive improves behavior with dynamic lighting. Requires computing material emissive on each(even cached) hit
#endif // SHARC_SEPARATE_EMISSIVE
};

struct SharcRadianceData
{
#if SHARC_ENABLE_SH_ENCODING
    float4 luminanceSH;             // xyz - L1, w - L0
    float2 chromaL0;                // Co/Cg L0 in YCoCg color space
#else // !SHARC_ENABLE_SH_ENCODING
    float3 radiance;
#endif // SHARC_ENABLE_SH_ENCODING
};

float3 SharcGetRadianceDirection(SharcHitData sharcHitData)
{
#if SHARC_ENABLE_SH_ENCODING
    return sharcHitData.radianceDirectionWorld;
#else // !SHARC_ENABLE_SH_ENCODING
    return float3(0.0f, 0.0f, 1.0f);
#endif // SHARC_ENABLE_SH_ENCODING
}

float SharcGetRadianceDirectionWeight(SharcHitData sharcHitData)
{
#if SHARC_ENABLE_SH_ENCODING
    return saturate(sharcHitData.radianceDirectionWeight);
#else // !SHARC_ENABLE_SH_ENCODING
    return 0.0f;
#endif // SHARC_ENABLE_SH_ENCODING
}

// Per-voxel spread statistic of the stored luminance, maintained by SharcResolveEntry (see the helpers before
// SharcAddVoxelData). Stored in SharcPackedData::sampleDataExt, 32 bits in every mode - the word
// SHARC_ENABLE_FADE_ACCELERATION keeps its fading bitmask in, so the two cannot be on together (see the #error above).
struct SharcLuminanceStats
{
#if SHARC_ENABLE_FILTER_MODE == SHARC_STANDARD_DEVIATION_FILTER
    float m2;               // weight-extensive second moment sum w (luma - mean)^2 over the EWMA window; variance = m2 / (K - 1)
#elif SHARC_ENABLE_FILTER_MODE == SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER
    float median;           // streaming median M of the luminance of the frame mean, in the cache's units
    float logMad;           // streaming median of |ln(luminance / M)| (madX), >= SHARC_LOG_MAD_MIN once seeded (exactly 0 = no observation yet); robust sigma = 1.4826 * logMad
#else
    uint unused;            // SHARC_FILTER_DISABLED: no statistic (GLSL has no empty structs)
#endif
};

uint SharcPackLuminanceStats(SharcLuminanceStats stats)
{
#if SHARC_ENABLE_FILTER_MODE == SHARC_STANDARD_DEVIATION_FILTER
    return asuint(stats.m2);
#elif SHARC_ENABLE_FILTER_MODE == SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER
    // The median is kept in the fp16 normal range: above it f32tof16 saturates or overflows to infinity, below it the
    // multiplicative steps of SharcUpdateLogStatistics would fall under the storage spacing (and could never leave 0).
    const float float16MinNormal = 6.103515625e-5f;
    const float float16Max = 65504.0f;
    return (f32tof16(clamp(stats.median, float16MinNormal, float16Max)) & 0xFFFFu) | (f32tof16(stats.logMad) << 16);
#else
    return 0u;
#endif
}

SharcLuminanceStats SharcUnpackLuminanceStats(uint packed)
{
    SharcLuminanceStats stats;
#if SHARC_ENABLE_FILTER_MODE == SHARC_STANDARD_DEVIATION_FILTER
    stats.m2 = asfloat(packed);
#elif SHARC_ENABLE_FILTER_MODE == SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER
    stats.median = f16tof32(packed & 0xFFFFu);
    stats.logMad = f16tof32(packed >> 16);
#else
    stats.unused = packed;
#endif
    return stats;
}

struct SharcVoxelData
{
    SharcRadianceData accumulatedRadiance;
    float accumulatedSampleNum;
    uint accumulatedFrameNum;
    uint staleFrameNum;
    uint sampleDataExt;             // SHARC_ENABLE_FADE_ACCELERATION's fading bitmask, or the packed luminance statistic of SHARC_ENABLE_FILTER_MODE (SharcUnpackLuminanceStats)
};

struct SharcResolveParameters
{
    float3 cameraPositionPrev;      // previous camera position
    uint accumulationFrameNum;      // maximum number of frames for the temporal accumulation window
    uint responsiveFrameNum;        // maximum number of frames for the temporal accumulation window used with responsive signal
    uint staleFrameNumMax;          // maximum number of frames without new samples before the cache entry is evicted
    uint logMadHalfWindow;          // SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER: N, clamped to [SHARC_LOG_MAD_HALF_WINDOW_MIN, SHARC_LOG_MAD_HALF_WINDOW_MAX]; the median and MAD track with a noise-equivalent window of 2N + 1 contributing frames. 10 gives 21, the cached mean's own window at accumulationFrameNum 10
    bool resetLuminanceStats;       // zero every record's luminance statistic this frame (SharcLuminanceStats; radiance and counters are kept): SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER re-seeds from the next frame mean, SHARC_STANDARD_DEVIATION_FILTER restarts M2 (its sigma then reads low for about accumulationFrameNum frames). For a clean comparison after a settings change; the statistics themselves are valid under any parameter
    uint frameIndex;
};

SharcRadianceData SharcZeroRadianceData()
{
    SharcRadianceData radianceData;
#if SHARC_ENABLE_SH_ENCODING
    radianceData.luminanceSH = float4(0, 0, 0, 0);
    radianceData.chromaL0 = float2(0, 0);
#else // !SHARC_ENABLE_SH_ENCODING
    radianceData.radiance = float3(0, 0, 0);
#endif // SHARC_ENABLE_SH_ENCODING

    return radianceData;
}

SharcRadianceData SharcAddRadianceData(SharcRadianceData a, SharcRadianceData b)
{
    SharcRadianceData result;
#if SHARC_ENABLE_SH_ENCODING
    result.luminanceSH = a.luminanceSH + b.luminanceSH;
    result.chromaL0 = a.chromaL0 + b.chromaL0;
#else // !SHARC_ENABLE_SH_ENCODING
    result.radiance = a.radiance + b.radiance;
#endif // SHARC_ENABLE_SH_ENCODING

    return result;
}

SharcRadianceData SharcScaleRadianceData(SharcRadianceData radianceData, float scale)
{
    SharcRadianceData result;
#if SHARC_ENABLE_SH_ENCODING
    result.luminanceSH = radianceData.luminanceSH * scale;
    result.chromaL0 = radianceData.chromaL0 * scale;
#else // !SHARC_ENABLE_SH_ENCODING
    result.radiance = radianceData.radiance * scale;
#endif // SHARC_ENABLE_SH_ENCODING

    return result;
}

float3 SharcRGBToYCoCg(float3 color)
{
    return float3(0.25f * (color.x + 2.0f * color.y + color.z), color.x - color.z, color.y - 0.5f * (color.x + color.z));
}

float3 SharcYCoCgToRGB(float3 color)
{
    return float3(color.x + 0.5f * (color.y - color.z), color.x + 0.5f * color.z, color.x - 0.5f * (color.y + color.z));
}

SharcRadianceData SharcEncodeRadiance(float3 radiance, float3 radianceDirection, float radianceDirectionWeight)
{
    SharcRadianceData radianceData;
#if SHARC_ENABLE_SH_ENCODING
    float3 direction = normalize(radianceDirection);
    float3 ycocg = SharcRGBToYCoCg(radiance);
    radianceData.luminanceSH = float4(ycocg.x * direction * saturate(radianceDirectionWeight), ycocg.x);
    radianceData.chromaL0 = ycocg.yz;
#else // !SHARC_ENABLE_SH_ENCODING
    radianceData.radiance = radiance;
#endif // SHARC_ENABLE_SH_ENCODING

    return radianceData;
}

float3 SharcDecodeRadiance(SharcRadianceData radianceData, float3 radianceDirection)
{
#if SHARC_ENABLE_SH_ENCODING
    float3 direction = normalize(radianceDirection);
    float directionalLuminance = min(length(radianceData.luminanceSH.xyz), max(radianceData.luminanceSH.w, 0.0f));
    float diffuseLuminance = max(radianceData.luminanceSH.w - directionalLuminance, 0.0f);
    float luminance = diffuseLuminance + max(dot(radianceData.luminanceSH.xyz, direction), 0.0f);
    float chromaScale = (radianceData.luminanceSH.w > 1e-6f) ? (luminance / radianceData.luminanceSH.w) : 0.0f;
    float2 chromaL0 = radianceData.chromaL0 * chromaScale;
    return max(SharcYCoCgToRGB(float3(luminance, chromaL0.x, chromaL0.y)), float3(0.0f, 0.0f, 0.0f));
#else // !SHARC_ENABLE_SH_ENCODING
    return radianceData.radiance;
#endif // SHARC_ENABLE_SH_ENCODING
}

uint SharcPackFloat16(float value)
{
    return f32tof16(value) & 0xFFFFu;
}

float SharcUnpackFloat16(uint value)
{
    return f16tof32(value & 0xFFFFu);
}

uint SharcPackFloat16Pair(float2 value)
{
    return SharcPackFloat16(value.x) | (SharcPackFloat16(value.y) << 16);
}

float2 SharcUnpackFloat16Pair(uint value)
{
    return float2(SharcUnpackFloat16(value), SharcUnpackFloat16(value >> 16));
}

SharcPackedData SharcZeroPackedData()
{
    SharcPackedData packedData;
    packedData.radianceData = float16_t4(0, 0, 0, 0);
#if SHARC_ENABLE_SH_ENCODING
    packedData.radianceDataExt = 0;
    packedData.sampleNumData = 0;
#endif // SHARC_ENABLE_SH_ENCODING
    packedData.sampleData = 0;
    packedData.sampleDataExt = 0;

    return packedData;
}

SharcAccumulationData SharcZeroAccumulationData()
{
    SharcAccumulationData accumulatedData;
#if SHARC_ENABLE_SH_ENCODING
    accumulatedData.data = int4(0, 0, 0, 0);
    accumulatedData.dataExt = int4(0, 0, 0, 0);
#else // !SHARC_ENABLE_SH_ENCODING
    accumulatedData.data = uint4(0, 0, 0, 0);
#endif // SHARC_ENABLE_SH_ENCODING

    return accumulatedData;
}

float SharcGetAccumulatedSampleNum(SharcAccumulationData accumulatedData)
{
#if SHARC_ENABLE_SH_ENCODING
    return float(accumulatedData.dataExt.z);
#else // !SHARC_ENABLE_SH_ENCODING
    return float(accumulatedData.data.w);
#endif // SHARC_ENABLE_SH_ENCODING
}

SharcRadianceData SharcGetAccumulatedRadianceData(SharcAccumulationData accumulatedData, float radianceScale, float sampleNum)
{
    SharcRadianceData radianceData;
    float scale = rcp(radianceScale * max(sampleNum, 1e-6f));
#if SHARC_ENABLE_SH_ENCODING
    radianceData.luminanceSH = float4(accumulatedData.data) * scale;
    radianceData.chromaL0 = float2(accumulatedData.dataExt.xy) * scale;
#else // !SHARC_ENABLE_SH_ENCODING
    radianceData.radiance = float3(accumulatedData.data.xyz) * scale;
#endif // SHARC_ENABLE_SH_ENCODING

    return radianceData;
}

SharcPackedData SharcPackVoxelData(SharcRadianceData radianceData, float sampleNum, uint accumulatedFrameNum, uint staleFrameNum, uint sampleDataExt)
{
    const float float16Max = 65504.0f;

    SharcPackedData packedData;
#if SHARC_ENABLE_SH_ENCODING
    packedData.radianceData = float16_t4(clamp(radianceData.luminanceSH, float4(-float16Max, -float16Max, -float16Max, -float16Max), float4(float16Max, float16Max, float16Max, float16Max)));
    packedData.radianceDataExt = SharcPackFloat16Pair(clamp(radianceData.chromaL0, float2(-float16Max, -float16Max), float2(float16Max, float16Max)));
    packedData.sampleNumData = SharcPackFloat16(min(sampleNum, float16Max));
#else // !SHARC_ENABLE_SH_ENCODING
    packedData.radianceData = float16_t4(min(float4(radianceData.radiance, sampleNum), float4(float16Max, float16Max, float16Max, float16Max)));
#endif // SHARC_ENABLE_SH_ENCODING
    packedData.sampleData = accumulatedFrameNum | (staleFrameNum << SHARC_STALE_FRAME_NUM_BIT_OFFSET);
    packedData.sampleDataExt = sampleDataExt;

    return packedData;
}

SharcVoxelData SharcUnpackVoxelData(SharcPackedData packedData)
{
    SharcVoxelData voxelData;
#if SHARC_ENABLE_SH_ENCODING
    voxelData.accumulatedRadiance.luminanceSH = float4(packedData.radianceData);
    voxelData.accumulatedRadiance.chromaL0 = SharcUnpackFloat16Pair(packedData.radianceDataExt);
    voxelData.accumulatedSampleNum = SharcUnpackFloat16(packedData.sampleNumData);
#else // !SHARC_ENABLE_SH_ENCODING
    float4 radianceAndSampleNum = float4(packedData.radianceData);
    voxelData.accumulatedRadiance.radiance = radianceAndSampleNum.xyz;
    voxelData.accumulatedSampleNum = radianceAndSampleNum.w;
#endif // SHARC_ENABLE_SH_ENCODING
    voxelData.accumulatedFrameNum = (packedData.sampleData >> SHARC_ACCUMULATED_FRAME_NUM_BIT_OFFSET) & SHARC_ACCUMULATED_FRAME_NUM_BIT_MASK;
    voxelData.staleFrameNum = (packedData.sampleData >> SHARC_STALE_FRAME_NUM_BIT_OFFSET) & SHARC_STALE_FRAME_NUM_BIT_MASK;
    voxelData.sampleDataExt = packedData.sampleDataExt;

    return voxelData;
}

SharcVoxelData SharcGetVoxelData(RW_STRUCTURED_BUFFER(voxelDataBuffer, SharcPackedData), HashGridIndex hashGridIndex)
{
    SharcPackedData packedData = BUFFER_AT_OFFSET(voxelDataBuffer, hashGridIndex);

    return SharcUnpackVoxelData(packedData);
}

float SharcLuma(float3 color)
{
    const float3 luma = float3(0.213f, 0.715f, 0.072f);

    return dot(color, luma);
}

float SharcRadianceLuma(SharcRadianceData radianceData)
{
#if SHARC_ENABLE_SH_ENCODING
    return max(radianceData.luminanceSH.w, 0.0f);
#else // !SHARC_ENABLE_SH_ENCODING
    return SharcLuma(radianceData.radiance);
#endif // SHARC_ENABLE_SH_ENCODING
}

// Luminance spread support. SharcResolveEntry maintains SharcPackedData::sampleDataExt from the per-frame means
// it sees - only the resolve pass writes it. The UPDATE path reads it for the deposit filter in SharcAddVoxelData
// (SharcParameters::fireflyWidth > 0); the QUERY path is unchanged. The statistic and its filter are selected by
// SHARC_ENABLE_FILTER_MODE:
//   SHARC_STANDARD_DEVIATION_FILTER: a weight-extensive second moment M2 = sum w (luma - mean)^2 built with the
//      Chan-Golub-LeVeque merge and decayed under the same exponential window as the mean, which gives the per-path
//      standard deviation (WorknotesSharcStdDev.md); the filter clamps a deposit above a Hoelder mean of one path's
//      luminance + width * sigma (WorknotesSharcHolderFilter.md).
//   SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER: a streaming median M of the luminance and median absolute deviation madX
//      of ln(luminance) - stochastic gradient trackers, one step per contributing frame (Knowledge.md, section 12) -
//      with 1.4826 * madX estimating the standard deviation of the underlying normal distribution of a lognormal
//      signal; the filter clamps a deposit above the band edge M exp(madX)^k, converted to the scale of one path
//      (WorknotesSharcAcceptanceFilter.md). Medians are far less sensitive than M2 to a spike population, but not
//      immune: each observation is a frame mean of n deposits, so a spike fraction p among deposits reaches
//      1 - (1 - p)^n of the frame means.
//   SHARC_FILTER_DISABLED: no statistic and no filter; sampleDataExt stays with SHARC_ENABLE_FADE_ACCELERATION.

// Luminance is SharcLuma / SharcRadianceLuma above.

#if SHARC_ENABLE_FILTER_MODE == SHARC_STANDARD_DEVIATION_FILTER

// Merges one weighted observation (a frame mean, or a neighbouring voxel's mean) into a running M2.
// The only term added is a squared difference scaled by non-negative weights, so there is no cancellation.
float SharcMergeLuminanceM2(float m2Prev, float lumaPrev, float weightPrev, float lumaNew, float weightNew)
{
    float delta = lumaNew - lumaPrev;
    return m2Prev + delta * delta * weightPrev * weightNew * rcp(weightPrev + weightNew);
}

// Per-sample luminance variance from the stored second moment. Because each merged observation is a frame
// MEAN weighted by its sample count, the expectation of M2 is (frames - 1) * sigma^2 regardless of how many
// samples each frame contributed - so the divisor is the frame count, not the sample weight (dividing by the
// sample weight would give sigma^2 / samplesPerFrame, an update-pass density artefact). Once the window has
// saturated the exact divisor is 2A(A+1)/(2A+1) for window A; K-1 reads ~16% high on variance at A = 10 and
// converges for larger windows. Callers must treat accumulatedFrameNum < 2 as "no estimate".
float SharcLuminanceVariance(float luminanceM2, uint accumulatedFrameNum)
{
    return luminanceM2 * rcp(max(float(accumulatedFrameNum) - 1.0f, 1.0f));
}

// New accessor alongside SharcGetCachedRadiance; that function and the update path are not modified.
bool SharcGetCachedVariance(in SharcParameters sharcParameters, in SharcHitData sharcHitData, out float variance)
{
    variance = 0.0f;

    HashGridKey hashGridKey;
    HashGridIndex cacheIndex = HashGridFindEntry(sharcParameters.hashGridData, sharcHitData.positionWorld, sharcHitData.normalWorld, sharcParameters.hashGridParameters, hashGridKey);
    if (cacheIndex == HASH_GRID_INVALID_CACHE_INDEX)
        return false;

    SharcVoxelData voxelData = SharcGetVoxelData(sharcParameters.resolvedBuffer, cacheIndex);
    SharcLuminanceStats luminance = SharcUnpackLuminanceStats(voxelData.sampleDataExt);
    if (voxelData.accumulatedSampleNum <= SHARC_SAMPLE_NUM_THRESHOLD || voxelData.accumulatedFrameNum < 2)
        return false;

    variance = SharcLuminanceVariance(luminance.m2, voxelData.accumulatedFrameNum);
    return true;
}

// Hoelder (power) mean M_p = E[X^p]^(1/p) of one path's luminance X, from the voxel's mean and per-path standard
// deviation. For lognormal X, ln X ~ N(m, s^2), E[X^p] = exp(p m + p^2 s^2 / 2), so
//     M_p = exp(m + p s^2 / 2) = mean (1 + sigma^2 / mean^2)^((p - 1) / 2):
// p = 0 is the geometric mean (the median), p = 1 the arithmetic mean, p = -1 the harmonic mean. Exact for lognormal
// X, and for any X at p = 1 and p = 2 (sqrt(mean^2 + sigma^2)). The filter tests one path's deposits, hence the
// per-path form: the power mean of the frame means the resolve pass sees is another quantity, which tends to the
// arithmetic mean as the paths per frame grow. The exponent is capped so that the result stays finite for any stored
// mean (fp16, at most 65504).
float SharcHolderMean(float mean, float sigma, float exponent)
{
    float relativeSigma = sigma / mean;
    return mean * exp(min(0.5f * (exponent - 1.0f) * log(1.0f + relativeSigma * relativeSigma), 69.0f));
}

// Deposit bound of the standard deviation filter (Experiment 1's gate, centred on a Hoelder mean):
//     bound = M_p + width * sigma,
// with sigma the per-path standard deviation, floored at SHARC_SIGMA_FLOOR_FRACTION of the mean so that a voxel with a
// near-constant history cannot clamp everything, and M_p = SharcHolderMean with exponent holderExponent. M2 sums frame
// means weighted by their path counts, so divided by frames (not samples) it describes one path and needs no scale
// conversion. The divisor follows the history the record holds. The resolve pass decays M2 and the weight by
// A / (accumulatedFrameNum + 1) on every contributing frame, and idle frames advance accumulatedFrameNum (F) and
// staleFrameNum (S) together, so a voxel visited every S + 1 frames holds N = (F + 1) / (S + 1) frames' worth of
// weight (the duty-cycle reading behind the median / MAD filter's path count), and at steady state
// E[M2] = 2N (N - 1) / (2N - 1) sigma^2. The divisor N - 1 = (F - S) / (S + 1) is slightly smaller; that offsets the
// right skew of an M2 built from a few frames, so the median sigma reads 0.98-0.99 of the truth without a filter
// (simulated). Dividing by F - S - 1 = A - 1 instead reads sigma low by about 1 / sqrt(S + 1) (simulated at a visit
// every 5th frame: sigma 0.32-0.37 of the truth and 6-8% of the ordinary energy clamped, against 0.95-1.04 and
// 0.8-1.2%). The filter acts only on statistics it can trust - at least filterMinFrameNum contributing frames
// (clamped like the resolve pass's window, and to 2 or more), about one path per frame over them (weight >= the same
// count), a non-zero spread and a non-zero mean - and returns 0 otherwise (the filter then passes everything).
float SharcFireflyLuminanceBound(in SharcParameters sharcParameters, SharcVoxelData voxelData)
{
    SharcLuminanceStats luminance = SharcUnpackLuminanceStats(voxelData.sampleDataExt);
    float minFrameNum = float(clamp(sharcParameters.filterMinFrameNum, 2u, uint(SHARC_ACCUMULATED_FRAME_NUM_MAX)));
    uint contributingFrameNum = voxelData.accumulatedFrameNum - voxelData.staleFrameNum;
    float luminanceMean = SharcRadianceLuma(voxelData.accumulatedRadiance);
    if (voxelData.accumulatedSampleNum <= SHARC_SAMPLE_NUM_THRESHOLD || voxelData.accumulatedSampleNum < minFrameNum || float(contributingFrameNum) < minFrameNum
        || !(luminance.m2 > 0.0f) || !(luminanceMean > 0.0f))
        return 0.0f;

    float m2FrameNum = float(contributingFrameNum) * rcp(float(voxelData.staleFrameNum) + 1.0f);
    float sigma = max(sqrt(luminance.m2 * rcp(m2FrameNum)), SHARC_SIGMA_FLOOR_FRACTION * luminanceMean);
    return SharcHolderMean(luminanceMean, sigma, sharcParameters.holderExponent) + sharcParameters.fireflyWidth * sigma;
}

#elif SHARC_ENABLE_FILTER_MODE == SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER

// One step of the streaming median M of the frame-mean luminance L and of madX, the median of |ln(L / M)|
// (Knowledge.md, section 12): stochastic gradient descent on the mean absolute error, one observation per
// contributing frame. Both tests use the previous state and L itself, so no logarithm is taken: s = sign(L - M), and
// L lies outside the band [M / D, M D], D = exp(madX), exactly when |ln(L / M)| > madX. Then
//     M <- M exp(eta_m madX s),   madX <- madX exp(eta_u r),   r = +1 outside the band, -1 strictly inside, 0 on an edge,
// so each estimate settles where half of its observations fall on either side (the median, by definition), and the
// madX factors exp(+-eta_u) cancel exactly. eta_m = 3.72 / w and eta_u = 2.33 / w, w = 2 logMadHalfWindow + 1
// (SharcResolveParameters), give the noise of an exact median over w frames when ln L is normal. The stored madX is
// floored at SHARC_LOG_MAD_MIN, so exactly 0 - a fresh or evicted record, or one that has only seen black frames -
// means "no observation yet", and the first observed frame seeds M with L and madX with a prior. Black frames
// (luminance 0) carry no information about the spread of a lognormal signal and are skipped. The trackers see the
// frame means of the clamped deposits: section 12 feeds every sample unfiltered so that a filter which rejects cannot
// lock out a change of level, but a clamped deposit still lands and counts, and the unclamped stream widens the band
// (simulated at 64 paths per frame: 24% of the firefly energy removed against 54%).
void SharcUpdateLogStatistics(inout SharcLuminanceStats stats, float lumaFrame, float window)
{
    if (!(lumaFrame > 0.0f))
        return;

    if (stats.logMad == 0.0f)
    {
        stats.median = lumaFrame;
        stats.logMad = SHARC_LOG_MAD_INITIAL;
        return;
    }

    float medianStep = 3.72f / window;
    float madStep = 2.33f / window;
    float band = exp(stats.logMad);
    float aboveMedian = float(sign(lumaFrame - stats.median));
    float outsideBand = (lumaFrame > stats.median * band || lumaFrame * band < stats.median) ? 1.0f
        : ((lumaFrame < stats.median * band && lumaFrame * band > stats.median) ? -1.0f 
        : 0.0f);
    stats.median *= exp(medianStep * stats.logMad * aboveMedian);
    stats.logMad = max(stats.logMad * exp(madStep * outsideBand), SHARC_LOG_MAD_MIN);
}

// Standard deviation of ln(luminance) implied by the MAD, assuming the underlying distribution is normal
// (i.e. the luminance is lognormal): 1 / PHI^-1(0.75) = 1.4826. Dimensionless.
float SharcRobustLogSigma(SharcLuminanceStats stats)
{
    return 1.4826f * stats.logMad;
}

// Deposit bound of the median absolute deviation filter (Knowledge.md, section 12, Use 3). Section 12 accepts a
// sample x inside the band [M D^-k, M D^k], D = exp(madX), k = 1.4826 width: a robust Z-score
// |ln(x / M)| / (1.4826 madX) of at most width. Three changes adapt it to SHaRC:
//   - one-sided: fireflies are bright, and a dark deposit raised to a bound would add energy;
//   - a deposit above the edge is clamped to it, not rejected (see SharcAddVoxelData);
//   - the band is converted to the scale of one path. M and madX describe the voxel's FRAME MEANS, each the mean over
//     the n paths counted that frame (SharcUpdateHit counts a path's own deposit; the pieces it back-propagates and
//     the sky radiance of SharcUpdateMiss add radiance but no count), so a frame mean is a mean of per-path totals,
//     and the legitimate spread of one path is much wider than that of a mean of n (in simulation, 64 paths per
//     frame tested on the frame scale lose 28% of their ordinary energy). Under the lognormal assumption the mean of
//     n lognormals is again close to lognormal (Fenton-Wilkinson) with its squared coefficient of variation divided
//     by n, which converts the frame scale (sigma_f = 1.4826 madX) to the scale of one path:
//         sigma_p^2 = ln(1 + n (exp(sigma_f^2) - 1)),   ln M_p = ln M + (sigma_f^2 - sigma_p^2) / 2,
//     and the band edge becomes M_p exp(width sigma_p).
// Every SharcAddVoxelData call - own deposit, back-propagated piece or miss - is tested on its own against this
// per-path bound, which is conservative: a piece never exceeds its path's total.
// n is read back from the record. The resolve pass keeps accumulatedSampleNum as an EWMA: a contributing resolve
// scales it by A / (accumulatedFrameNum + 1) before adding the frame's count (A = accumulationFrameNum; idle frames
// advance accumulatedFrameNum and staleFrameNum without clamping), so a voxel visited every S + 1 frames settles at
// W = n (F + 1) / (S + 1) and n = W (S + 1) / (F + 1) with F = accumulatedFrameNum, S = staleFrameNum. The raw ratio
// W / F would read n low by the duty cycle of a sparsely visited voxel and pull the bound back towards the frame
// scale (simulated at 16 paths every 5th frame: 10% of the ordinary energy clamped against 1.1%). What the record
// cannot tell: before the window saturates n reads low by K / (K + 1) after K contributing frames (while the MAD is
// still near its seed), the first frame back from a long idle run reads it high (that frame passes unfiltered), and
// for about A frames after that it reads low because the resolve pass has decayed W.
// The arithmetic is arranged so that nothing overflows for any stored madX: ln(1 + n (e^v - 1)) = v +
// ln(n - (n - 1) e^-v), whose log argument lies in [1, n], and the exponent is capped at 69, which keeps M e^69 below
// FLT_MAX for any stored M (fp16, at most 65504), so the bound is always finite. Returns the luminance a deposit may
// reach before it is clamped, or 0 when the voxel has no estimate yet (the filter then passes everything).
float SharcFireflyLuminanceBound(in SharcParameters sharcParameters, SharcVoxelData voxelData)
{
    SharcLuminanceStats luminance = SharcUnpackLuminanceStats(voxelData.sampleDataExt);
    if (luminance.logMad == 0.0f || voxelData.accumulatedSampleNum <= SHARC_SAMPLE_NUM_THRESHOLD)
        return 0.0f;

    float sigmaFrame = SharcRobustLogSigma(luminance);
    float pathsPerFrame = max(voxelData.accumulatedSampleNum * (float(voxelData.staleFrameNum) + 1.0f) * rcp(float(voxelData.accumulatedFrameNum) + 1.0f), 1.0f);
    float varFrame = sigmaFrame * sigmaFrame;
    float varPath = varFrame + log(pathsPerFrame - (pathsPerFrame - 1.0f) * exp(-varFrame));
    return luminance.median * exp(min(0.5f * (varFrame - varPath) + sharcParameters.fireflyWidth * sqrt(varPath), 69.0f));
}

// New accessor alongside SharcGetCachedRadiance; that function is not modified. Returns
// the voxel's ln-luminance median and robust sigma, or false (zeros) when there is no entry, no samples, or no
// observed frame yet (MAD still 0). Early values are dominated by the prior: the first observed frame returns
// sigma = 1.4826 * SHARC_LOG_MAD_INITIAL, and the trackers need about a window (2 logMadHalfWindow + 1) of observed
// frames to settle on the voxel's own spread.
bool SharcGetCachedLogStatistics(in SharcParameters sharcParameters, in SharcHitData sharcHitData, out float logMedian, out float robustSigma)
{
    logMedian = 0.0f;
    robustSigma = 0.0f;

    HashGridKey hashGridKey;
    HashGridIndex cacheIndex = HashGridFindEntry(sharcParameters.hashGridData, sharcHitData.positionWorld, sharcHitData.normalWorld, sharcParameters.hashGridParameters, hashGridKey);
    if (cacheIndex == HASH_GRID_INVALID_CACHE_INDEX)
        return false;

    SharcVoxelData voxelData = SharcGetVoxelData(sharcParameters.resolvedBuffer, cacheIndex);
    SharcLuminanceStats luminance = SharcUnpackLuminanceStats(voxelData.sampleDataExt);
    if (voxelData.accumulatedSampleNum <= SHARC_SAMPLE_NUM_THRESHOLD || luminance.logMad == 0.0f)
        return false;

    logMedian = log(luminance.median);
    robustSigma = SharcRobustLogSigma(luminance);
    return true;
}

#endif // SHARC_ENABLE_FILTER_MODE

void SharcAddVoxelData(in SharcParameters sharcParameters, HashGridIndex hashGridIndex, float3 sampleValue, float3 sampleWeight, float3 sampleDirection, float sampleDirectionWeight, uint sampleData)
{
#if SHARC_ENABLE_FILTER_MODE != SHARC_FILTER_DISABLED
    // Deposit filter of SHARC_ENABLE_FILTER_MODE. A deposit above the voxel's bound is scaled down to it and still
    // deposited and counted, never dropped: a dropped deposit cannot be told apart from an absent one, so the voxel
    // could neither be starved into eviction nor follow a real brightening (Experiment 1). Costs one 16-byte read of
    // the resolved record per deposit while enabled.
    if (sharcParameters.fireflyWidth > 0.0f)
    {
        float bound = SharcFireflyLuminanceBound(sharcParameters, SharcGetVoxelData(sharcParameters.resolvedBuffer, hashGridIndex));
        float luminanceCur = SharcLuma(sampleValue * sampleWeight);
        if (bound > 0.0f && luminanceCur > bound)
            sampleWeight *= bound / luminanceCur;
    }
#endif // SHARC_ENABLE_FILTER_MODE != SHARC_FILTER_DISABLED

#if SHARC_ENABLE_SH_ENCODING
    SharcRadianceData scaledSample = SharcEncodeRadiance(sampleValue * sampleWeight, sampleDirection, sampleDirectionWeight);
    int4 scaledLuminanceSH = int4(scaledSample.luminanceSH * sharcParameters.radianceScale);
    int2 scaledChromaL0 = int2(scaledSample.chromaL0 * sharcParameters.radianceScale);

    if (scaledLuminanceSH.x != 0) InterlockedAdd(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, hashGridIndex).data.x, scaledLuminanceSH.x);
    if (scaledLuminanceSH.y != 0) InterlockedAdd(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, hashGridIndex).data.y, scaledLuminanceSH.y);
    if (scaledLuminanceSH.z != 0) InterlockedAdd(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, hashGridIndex).data.z, scaledLuminanceSH.z);
    if (scaledLuminanceSH.w != 0) InterlockedAdd(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, hashGridIndex).data.w, scaledLuminanceSH.w);
    if (scaledChromaL0.x != 0) InterlockedAdd(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, hashGridIndex).dataExt.x, scaledChromaL0.x);
    if (scaledChromaL0.y != 0) InterlockedAdd(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, hashGridIndex).dataExt.y, scaledChromaL0.y);
    if (sampleData != 0) InterlockedAdd(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, hashGridIndex).dataExt.z, int(sampleData));
#else // !SHARC_ENABLE_SH_ENCODING
    uint3 scaledRadiance = uint3(sampleValue * sampleWeight * sharcParameters.radianceScale);

    if (scaledRadiance.x != 0) InterlockedAdd(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, hashGridIndex).data.x, scaledRadiance.x);
    if (scaledRadiance.y != 0) InterlockedAdd(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, hashGridIndex).data.y, scaledRadiance.y);
    if (scaledRadiance.z != 0) InterlockedAdd(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, hashGridIndex).data.z, scaledRadiance.z);
    if (sampleData != 0) InterlockedAdd(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, hashGridIndex).data.w, sampleData);
#endif // SHARC_ENABLE_SH_ENCODING
}

void SharcInit(inout SharcState sharcState)
{
#if SHARC_UPDATE
    sharcState.pathLength = 0;
#endif // SHARC_UPDATE
}

#if SHARC_ENABLE_RESPONSIVE_LIGHTING
uint SharcPackCacheIndexWithOffset(uint baseIndex, int offset)
{
    uint offsetBits = (uint(offset)) & SHARC_RESPONSIVE_INDEX_OFFSET_BIT_MASK;

    return (baseIndex & SHARC_CACHE_INDEX_BIT_MASK) | (offsetBits << SHARC_CACHE_INDEX_BIT_NUM);
}

int SharcUnpackOffset(uint packedIndex)
{
    return (int(packedIndex)) >> SHARC_CACHE_INDEX_BIT_NUM;
}

bool SharcGetResponsiveIndexOffset(in SharcParameters sharcParameters, HashGridIndex hashGridIndex, out int responsiveIndexOffset, out bool isNewSample)
{
    responsiveIndexOffset = SharcUnpackOffset(hashGridIndex);
    isNewSample = false;
    if (responsiveIndexOffset == 0)
    {
        HashGridKey hashGridKey = BUFFER_AT_OFFSET(sharcParameters.hashGridData.hashEntriesBuffer, hashGridIndex);
        uint baseSlot = HashGridGetBaseSlot(hashGridKey, sharcParameters.hashGridData.capacity);
        hashGridKey |= HashGridKey(1) << (HASH_GRID_KEY_BIT_NUM - 1);
        HashGridIndex responsiveIndex;
        uint bucketOffset;
        if (!HashGridInsert(sharcParameters.hashGridData, hashGridKey, baseSlot, SHARC_RESPONSIVE_ENTRY_PROBE_RANGE, responsiveIndex, bucketOffset))
            return false;

        responsiveIndexOffset = int(responsiveIndex) - int(hashGridIndex);
        isNewSample = true;
    }

    return true;
}
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING

void SharcUpdateMiss(in SharcParameters sharcParameters, in SharcState sharcState, float3 radiance
#if SHARC_ENABLE_RESPONSIVE_LIGHTING
    , bool isResponsiveLighting
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING
    )
{
#if SHARC_UPDATE
    for (int i = 0; i < sharcState.pathLength; ++i)
    {
        HashGridIndex hashGridIndex = sharcState.cacheIndices[i];
        bool isNewSample = false;

#if SHARC_ENABLE_RESPONSIVE_LIGHTING
        if (isResponsiveLighting)
        {
            int responsiveIndexOffset;
            if (!SharcGetResponsiveIndexOffset(sharcParameters, sharcState.cacheIndices[i], responsiveIndexOffset, isNewSample))
                return;

            hashGridIndex += responsiveIndexOffset;
        }

        hashGridIndex &= SHARC_CACHE_INDEX_BIT_MASK;
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING
#if SHARC_ENABLE_SH_ENCODING
        float3 radianceDirection = sharcState.radianceDirections[i];
        float radianceDirectionWeight = sharcState.radianceDirectionWeights[i];
#else // !SHARC_ENABLE_SH_ENCODING
        float3 radianceDirection = float3(0.0f, 0.0f, 1.0f);
        float radianceDirectionWeight = 0.0f;
#endif // SHARC_ENABLE_SH_ENCODING
        SharcAddVoxelData(sharcParameters, hashGridIndex, radiance, sharcState.sampleWeights[i], radianceDirection, radianceDirectionWeight, isNewSample ? 1 : 0);
    }
#endif // SHARC_UPDATE
}

bool SharcUpdateHit(in SharcParameters sharcParameters, inout SharcState sharcState, SharcHitData sharcHitData, float3 directLighting, float random
#if SHARC_ENABLE_RESPONSIVE_LIGHTING
    , bool isResponsiveLighting
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING
    )
{
    bool continueTracing = true;
#if SHARC_UPDATE
    HashGridKey hashGridKey;
    HashGridIndex hashGridIndex;
    if (!HashGridInsertEntry(sharcParameters.hashGridData, sharcHitData.positionWorld, sharcHitData.normalWorld, sharcParameters.hashGridParameters, hashGridKey, hashGridIndex))
        return false;

#if SHARC_ENABLE_RESPONSIVE_LIGHTING
    HashGridIndex responsiveCacheIndex = HASH_GRID_INVALID_CACHE_INDEX;
    if (isResponsiveLighting)
    {
        uint baseSlot = HashGridGetBaseSlot(hashGridKey, sharcParameters.hashGridData.capacity);
        hashGridKey |= HashGridKey(1) << (HASH_GRID_KEY_BIT_NUM - 1);
        uint bucketOffset;
        if (!HashGridInsert(sharcParameters.hashGridData, hashGridKey, baseSlot, SHARC_RESPONSIVE_ENTRY_PROBE_RANGE, responsiveCacheIndex, bucketOffset))
            return false;
    }
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING

    float3 sharcRadiance = directLighting;
    float3 materialDemodulation = float3(1.0f, 1.0f, 1.0f);
    float3 sharcRadianceDirection = SharcGetRadianceDirection(sharcHitData);
    float sharcRadianceDirectionWeight = SharcGetRadianceDirectionWeight(sharcHitData);
#if SHARC_MATERIAL_DEMODULATION
    materialDemodulation = sharcHitData.materialDemodulation;
#endif // SHARC_MATERIAL_DEMODULATION
    float3 inverseMaterialDemodulation = rcp(materialDemodulation);

#if SHARC_ENABLE_CACHE_RESAMPLING
    uint resamplingDepth = uint(round(lerp(SHARC_RESAMPLING_DEPTH_MIN, SHARC_PROPAGATION_DEPTH, random)));
    if (resamplingDepth <= sharcState.pathLength)
    {
        SharcVoxelData voxelData = SharcGetVoxelData(sharcParameters.resolvedBuffer, hashGridIndex);
        if (voxelData.accumulatedSampleNum > SHARC_SAMPLE_NUM_THRESHOLD)
        {
            sharcRadiance = SharcDecodeRadiance(voxelData.accumulatedRadiance, sharcRadianceDirection);
#if SHARC_ENABLE_RESPONSIVE_LIGHTING
            if (responsiveCacheIndex != HASH_GRID_INVALID_CACHE_INDEX)
            {
                SharcVoxelData responsiveVoxelData = SharcGetVoxelData(sharcParameters.resolvedBuffer, responsiveCacheIndex);
                if (responsiveVoxelData.accumulatedSampleNum > SHARC_SAMPLE_NUM_THRESHOLD)
                    sharcRadiance += SharcDecodeRadiance(responsiveVoxelData.accumulatedRadiance, sharcRadianceDirection);
            }
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING
            sharcRadiance *= materialDemodulation;
            continueTracing = false;
        }
    }
#endif // SHARC_ENABLE_CACHE_RESAMPLING

    if (continueTracing)
    {
#if SHARC_ENABLE_RESPONSIVE_LIGHTING
        if (responsiveCacheIndex != HASH_GRID_INVALID_CACHE_INDEX)
        {
            SharcAddVoxelData(sharcParameters, responsiveCacheIndex, directLighting * inverseMaterialDemodulation, float3(1.0f, 1.0f, 1.0f), sharcRadianceDirection, sharcRadianceDirectionWeight, 1);
            directLighting = float3(0.0f, 0.0f, 0.0f); // avoid adding the direct lighting contribution twice
        }
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING
        SharcAddVoxelData(sharcParameters, hashGridIndex, directLighting * inverseMaterialDemodulation, float3(1.0f, 1.0f, 1.0f), sharcRadianceDirection, sharcRadianceDirectionWeight, 1);
    }

#if SHARC_SEPARATE_EMISSIVE
    sharcRadiance += sharcHitData.emissive;
#endif // SHARC_SEPARATE_EMISSIVE

    uint i;
    for (i = 0; i < sharcState.pathLength; ++i)
    {
        HashGridIndex tempHashGridIndex = sharcState.cacheIndices[i];
        if (tempHashGridIndex == HASH_GRID_INVALID_CACHE_INDEX)
            continue;

        bool isNewSample = false;
#if SHARC_ENABLE_RESPONSIVE_LIGHTING
        if (responsiveCacheIndex != HASH_GRID_INVALID_CACHE_INDEX)
        {
            int responsiveIndexOffset;
            if (!SharcGetResponsiveIndexOffset(sharcParameters, sharcState.cacheIndices[i], responsiveIndexOffset, isNewSample))
                return false;

            tempHashGridIndex += responsiveIndexOffset;

            if (isNewSample)
                sharcState.cacheIndices[i] = SharcPackCacheIndexWithOffset(sharcState.cacheIndices[i], responsiveIndexOffset);
        }

        tempHashGridIndex &= SHARC_CACHE_INDEX_BIT_MASK;
        isNewSample = isNewSample && isResponsiveLighting;
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING
#if SHARC_ENABLE_SH_ENCODING
        float3 radianceDirection = sharcState.radianceDirections[i];
        float radianceDirectionWeight = sharcState.radianceDirectionWeights[i];
#else // !SHARC_ENABLE_SH_ENCODING
        float3 radianceDirection = float3(0.0f, 0.0f, 1.0f);
        float radianceDirectionWeight = 0.0f;
#endif // SHARC_ENABLE_SH_ENCODING
        SharcAddVoxelData(sharcParameters, tempHashGridIndex, sharcRadiance, sharcState.sampleWeights[i], radianceDirection, radianceDirectionWeight, isNewSample ? 1 : 0);
    }

    for (i = min(sharcState.pathLength, SHARC_PROPAGATION_DEPTH - 1); i > 0; --i)
    {
        sharcState.cacheIndices[i] = sharcState.cacheIndices[i - 1];
        sharcState.sampleWeights[i] = sharcState.sampleWeights[i - 1];
#if SHARC_ENABLE_SH_ENCODING
        sharcState.radianceDirections[i] = sharcState.radianceDirections[i - 1];
        sharcState.radianceDirectionWeights[i] = sharcState.radianceDirectionWeights[i - 1];
#endif // SHARC_ENABLE_SH_ENCODING
    }

#if SHARC_ENABLE_RESPONSIVE_LIGHTING
    if (responsiveCacheIndex != HASH_GRID_INVALID_CACHE_INDEX)
    {
        int responsiveIndexOffset = int(responsiveCacheIndex) - int(hashGridIndex);
        hashGridIndex = SharcPackCacheIndexWithOffset(hashGridIndex, responsiveIndexOffset);
    }
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING

    sharcState.cacheIndices[0] = hashGridIndex;
    sharcState.sampleWeights[0] = SharcSampleWeight(inverseMaterialDemodulation);
#if SHARC_ENABLE_SH_ENCODING
    sharcState.radianceDirections[0] = sharcRadianceDirection;
    sharcState.radianceDirectionWeights[0] = sharcRadianceDirectionWeight;
#endif // SHARC_ENABLE_SH_ENCODING
    sharcState.pathLength = min(++sharcState.pathLength, SHARC_PROPAGATION_DEPTH);
#endif // SHARC_UPDATE
    return continueTracing;
}

void SharcSetThroughput(inout SharcState sharcState, float3 throughput)
{
#if SHARC_UPDATE
    for (uint i = 0; i < sharcState.pathLength; ++i)
        sharcState.sampleWeights[i] *= SharcSampleWeight(throughput);
#endif // SHARC_UPDATE
}

void SharcSetRadianceDirectionWeight(inout SharcState sharcState, float radianceDirectionWeight)
{
#if SHARC_UPDATE && SHARC_ENABLE_SH_ENCODING
    if (sharcState.pathLength > 0)
        sharcState.radianceDirectionWeights[0] = saturate(radianceDirectionWeight);
#endif // SHARC_UPDATE && SHARC_ENABLE_SH_ENCODING
}

bool SharcGetCachedRadianceFromHash(in SharcParameters sharcParameters, in SharcHitData sharcHitData, HashGridKey hashGridKey, out float3 radiance, bool skipResponsiveLighting)
{
    HashGridIndex hashGridIndex;
    uint baseSlot = HashGridGetBaseSlot(hashGridKey, sharcParameters.hashGridData.capacity);
    uint bucketOffset;
    if (!HashGridFind(sharcParameters.hashGridData, hashGridKey, baseSlot, HASH_GRID_HASH_MAP_BUCKET_SIZE, hashGridIndex, bucketOffset))
        return false;

    SharcVoxelData voxelData = SharcGetVoxelData(sharcParameters.resolvedBuffer, hashGridIndex);
    if (voxelData.accumulatedSampleNum > SHARC_SAMPLE_NUM_THRESHOLD)
    {
        float3 sharcRadianceDirection = SharcGetRadianceDirection(sharcHitData);
        radiance = SharcDecodeRadiance(voxelData.accumulatedRadiance, sharcRadianceDirection);

#if SHARC_ENABLE_RESPONSIVE_LIGHTING
        uint temp;
        hashGridKey |= HashGridKey(1) << (HASH_GRID_KEY_BIT_NUM - 1);
        if (!skipResponsiveLighting && HashGridFind(sharcParameters.hashGridData, hashGridKey, baseSlot, SHARC_RESPONSIVE_ENTRY_PROBE_RANGE, hashGridIndex, temp))
        {
            SharcVoxelData responsiveVoxelData = SharcGetVoxelData(sharcParameters.resolvedBuffer, hashGridIndex);
            if (responsiveVoxelData.accumulatedSampleNum > SHARC_SAMPLE_NUM_THRESHOLD)
                radiance += SharcDecodeRadiance(responsiveVoxelData.accumulatedRadiance, sharcRadianceDirection);
        }
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING

#if SHARC_MATERIAL_DEMODULATION
        radiance *= sharcHitData.materialDemodulation;
#endif // SHARC_MATERIAL_DEMODULATION
#if SHARC_SEPARATE_EMISSIVE
        radiance += sharcHitData.emissive;
#endif // SHARC_SEPARATE_EMISSIVE

        return true;
    }

    return false;
}

bool SharcGetCachedRadiance(in SharcParameters sharcParameters, in SharcHitData sharcHitData, out float3 radiance, bool skipResponsiveLighting)
{
    HashGridKey hashGridKey = HashGridComputeSpatialHash(sharcHitData.positionWorld, sharcHitData.normalWorld, sharcParameters.hashGridParameters);
    return SharcGetCachedRadianceFromHash(sharcParameters, sharcHitData, hashGridKey, radiance, skipResponsiveLighting);
}

int SharcGetGridDistance2(int3 position)
{
    return position.x * position.x + position.y * position.y + position.z * position.z;
}

HashGridKey SharcGetAdjacentLevelHashKey(HashGridKey hashGridKey, HashGridParameters gridParameters, float3 cameraPositionPrev)
{
    int3 gridPosition;
    gridPosition.x = int((hashGridKey >> HASH_GRID_POSITION_BIT_NUM * 0) & HASH_GRID_POSITION_BIT_MASK);
    gridPosition.y = int((hashGridKey >> HASH_GRID_POSITION_BIT_NUM * 1) & HASH_GRID_POSITION_BIT_MASK);
    gridPosition.z = int((hashGridKey >> HASH_GRID_POSITION_BIT_NUM * 2) & HASH_GRID_POSITION_BIT_MASK);

    // Sign-extend packed coordinates without divergent branches.
    gridPosition = (gridPosition << (32 - HASH_GRID_POSITION_BIT_NUM)) >> (32 - HASH_GRID_POSITION_BIT_NUM);

    int level = int((hashGridKey >> HASH_GRID_LEVEL_BIT_OFFSET) & HASH_GRID_LEVEL_BIT_MASK);

    float voxelSize = HashGridGetVoxelSize(uint(level), gridParameters);
    float inverseVoxelSize = rcp(voxelSize);
    int3 cameraGridPosition = int3(floor(gridParameters.cameraPosition * inverseVoxelSize));
    int3 cameraVector = cameraGridPosition - gridPosition;
    int cameraDistance = SharcGetGridDistance2(cameraVector);

    int3 cameraGridPositionPrev = int3(floor(cameraPositionPrev * inverseVoxelSize));
    int3 cameraVectorPrev = cameraGridPositionPrev - gridPosition;
    int cameraDistancePrev = SharcGetGridDistance2(cameraVectorPrev);

    if (cameraDistance < cameraDistancePrev)
    {
        gridPosition = int3(floor(gridPosition / gridParameters.logarithmBase));
        level = min(level + 1, int(HASH_GRID_LEVEL_BIT_MASK));
    }
    else // this may be inaccurate
    {
        gridPosition = int3(floor(gridPosition * gridParameters.logarithmBase));
        level = max(level - 1, 1);
    }

    HashGridKey modifiedHashGridKey = ((HashGridKey(gridPosition.x) & HASH_GRID_POSITION_BIT_MASK) << (HASH_GRID_POSITION_BIT_NUM * 0))
        | ((HashGridKey(gridPosition.y) & HASH_GRID_POSITION_BIT_MASK) << (HASH_GRID_POSITION_BIT_NUM * 1))
        | ((HashGridKey(gridPosition.z) & HASH_GRID_POSITION_BIT_MASK) << (HASH_GRID_POSITION_BIT_NUM * 2))
        | ((HashGridKey(level) & HASH_GRID_LEVEL_BIT_MASK) << HASH_GRID_LEVEL_BIT_OFFSET);

#if HASH_GRID_USE_NORMALS
    modifiedHashGridKey |= hashGridKey & (HashGridKey(HASH_GRID_NORMAL_BIT_MASK) << HASH_GRID_NORMAL_BIT_OFFSET);
#endif // HASH_GRID_USE_NORMALS

    return modifiedHashGridKey;
}

void SharcResolveEntry(uint entryIndex, SharcParameters sharcParameters, SharcResolveParameters resolveParameters)
{
    if (entryIndex >= sharcParameters.hashGridData.capacity)
        return;

    HashGridKey hashGridKey = BUFFER_AT_OFFSET(sharcParameters.hashGridData.hashEntriesBuffer, entryIndex);
    if (hashGridKey == HASH_GRID_INVALID_HASH_KEY)
        return;

    bool isResponsiveSignal = false;
#if SHARC_ENABLE_RESPONSIVE_LIGHTING
    isResponsiveSignal = (hashGridKey >> (HASH_GRID_KEY_BIT_NUM - 1)) != 0;
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING

    SharcAccumulationData accumulatedData = BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, entryIndex);
    SharcPackedData resolvedData = BUFFER_AT_OFFSET(sharcParameters.resolvedBuffer, entryIndex);
    SharcVoxelData sharcVoxelData = SharcUnpackVoxelData(resolvedData);

    float sampleNum = SharcGetAccumulatedSampleNum(accumulatedData);
    float sampleNumPrev = sharcVoxelData.accumulatedSampleNum;
    uint accumulatedFrameNum = sharcVoxelData.accumulatedFrameNum + 1;
    uint staleFrameNum = sharcVoxelData.staleFrameNum;

    staleFrameNum = (sampleNum != 0) ? 0 : staleFrameNum + 1;
    uint staleFrameNumMax = clamp(resolveParameters.staleFrameNumMax, SHARC_STALE_FRAME_NUM_MIN, SHARC_STALE_FRAME_NUM_MAX);

    if (isResponsiveSignal)
        staleFrameNumMax = resolveParameters.responsiveFrameNum;

    bool isValidElement = (staleFrameNum < staleFrameNumMax) ? true : false;
    if (!isValidElement)
    {
        SharcAccumulationData zeroAccumulationData = SharcZeroAccumulationData();
        SharcPackedData zeroPackedData = SharcZeroPackedData();

        BUFFER_AT_OFFSET(sharcParameters.hashGridData.hashEntriesBuffer, entryIndex) = HASH_GRID_INVALID_HASH_KEY;
        BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, entryIndex) = zeroAccumulationData;
        BUFFER_AT_OFFSET(sharcParameters.resolvedBuffer, entryIndex) = zeroPackedData;
        return;
    }
    else if (sampleNum == 0 && !isResponsiveSignal)
    {
        // Resolve dispatches exactly one thread per entry, so no atomic is needed.
        BUFFER_AT_OFFSET(sharcParameters.resolvedBuffer, entryIndex).sampleData = resolvedData.sampleData + ((1 << SHARC_ACCUMULATED_FRAME_NUM_BIT_OFFSET) | (1 << SHARC_STALE_FRAME_NUM_BIT_OFFSET));
#if SHARC_ENABLE_FADE_ACCELERATION
        uint bitOffset = resolveParameters.frameIndex % 32u;
        uint bit = 1u << bitOffset;
        BUFFER_AT_OFFSET(sharcParameters.resolvedBuffer, entryIndex).sampleDataExt = resolvedData.sampleDataExt | bit;
#endif // SHARC_ENABLE_FADE_ACCELERATION
#if SHARC_ENABLE_FILTER_MODE != SHARC_FILTER_DISABLED
        // an idle record is not rewritten below, so the reset must reach it here
        if (resolveParameters.resetLuminanceStats)
            BUFFER_AT_OFFSET(sharcParameters.resolvedBuffer, entryIndex).sampleDataExt = 0;
#endif
        return;
    }

#if SHARC_ENABLE_RESPONSIVE_LIGHTING
    if (isResponsiveSignal)
    {
        HashGridKey targetHashGridKey = hashGridKey & ~(HashGridKey(1) << (HASH_GRID_KEY_BIT_NUM - 1)); // clear responsive bit to find the main entry
        uint baseSlot = HashGridGetBaseSlot(targetHashGridKey, sharcParameters.hashGridData.capacity);
        for (uint i = 0; i < SHARC_RESPONSIVE_ENTRY_PROBE_RANGE; ++i)
        {
            uint searchIndex = baseSlot + i;
            HashGridKey searchHashGridKey = BUFFER_AT_OFFSET(sharcParameters.hashGridData.hashEntriesBuffer, searchIndex);
            if (searchHashGridKey == targetHashGridKey)
            {
                SharcAccumulationData targetAccumulatedData = BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, searchIndex);
                sampleNum = SharcGetAccumulatedSampleNum(targetAccumulatedData);
                break;
            }
        }
    }
#endif // SHARC_ENABLE_RESPONSIVE_LIGHTING

    // Performs hash map lookup to find existing entries in case previous insertions
    // encountered collisions and a different slot was assigned.
    // Uses a fixed-size linear probe window
    if (sampleNumPrev == 0)
    {
        uint searchEnd = min(entryIndex + 1 + SHARC_LINEAR_PROBE_WINDOW_SIZE, sharcParameters.hashGridData.capacity);
        for (uint i = entryIndex + 1; i < searchEnd; ++i)
        {
            HashGridKey hashKeyOld = BUFFER_AT_OFFSET(sharcParameters.hashGridData.hashEntriesBuffer, i);
            if (hashKeyOld == hashGridKey)
            {
                resolvedData = BUFFER_AT_OFFSET(sharcParameters.resolvedBuffer, i);
                sharcVoxelData = SharcUnpackVoxelData(resolvedData);
                sampleNumPrev = sharcVoxelData.accumulatedSampleNum;
                accumulatedFrameNum = sharcVoxelData.accumulatedFrameNum + 1;
                staleFrameNum = 0;
                break;
            }
        }
    }

    SharcRadianceData accumulatedRadiance = SharcGetAccumulatedRadianceData(accumulatedData, sharcParameters.radianceScale, sampleNum);
    SharcRadianceData accumulatedRadiancePrev = sharcVoxelData.accumulatedRadiance;
    uint accumulationFrameNum = clamp(isResponsiveSignal ? resolveParameters.responsiveFrameNum : resolveParameters.accumulationFrameNum, SHARC_ACCUMULATED_FRAME_NUM_MIN, SHARC_ACCUMULATED_FRAME_NUM_MAX);
#if SHARC_ENABLE_FILTER_MODE != SHARC_FILTER_DISABLED
    // after the probe above, so that a recovered record's statistics are reset too; the unpack of 0 is every mode's
    // "no statistic yet" state, so this frame's mean then seeds (mode 2) or restarts (mode 1) the statistic below
    SharcLuminanceStats luminance = SharcUnpackLuminanceStats(resolveParameters.resetLuminanceStats ? 0u : sharcVoxelData.sampleDataExt);
#endif
    if (accumulatedFrameNum > accumulationFrameNum)
    {
        float normalizationScale = float(accumulationFrameNum) / float(accumulatedFrameNum);
        accumulatedFrameNum = accumulationFrameNum;
        sampleNumPrev *= normalizationScale;
#if SHARC_ENABLE_FILTER_MODE == SHARC_STANDARD_DEVIATION_FILTER
        // M2 is extensive in the same weight, so it must decay with it or it accumulates without bound
        luminance.m2 *= normalizationScale;
#endif
    }

#if SHARC_ENABLE_FADE_ACCELERATION
    {
        uint bitOffset = resolveParameters.frameIndex % 32u;
        uint bit = 1u << bitOffset;

        float lumaCur = SharcRadianceLuma(accumulatedRadiance);
        float lumaPrev = SharcRadianceLuma(accumulatedRadiancePrev);
        bool fading = lumaCur < lumaPrev;

        sharcVoxelData.sampleDataExt = (sharcVoxelData.sampleDataExt & ~bit) | (fading ? bit : 0u);
        uint fadingFrameNum = countbits(sharcVoxelData.sampleDataExt);
        if (fadingFrameNum == 32)
            sampleNumPrev = sampleNum;
    }
#endif // SHARC_ENABLE_FADE_ACCELERATION
    float sampleTotalInv = rcp(sampleNumPrev + sampleNum);

#if SHARC_ENABLE_FILTER_MODE == SHARC_STANDARD_DEVIATION_FILTER
    // This frame's mean enters the variance as one observation of weight sampleNum, with the same weights
    // the mean blend below uses. Only per-frame sums reach the resolve pass, so this is the spread of frame
    // means, not of individual paths; SharcLuminanceVariance converts it back to a per-sample variance.
    luminance.m2 = SharcMergeLuminanceM2(luminance.m2, SharcRadianceLuma(accumulatedRadiancePrev), sampleNumPrev, SharcRadianceLuma(accumulatedRadiance), sampleNum);
#elif SHARC_ENABLE_FILTER_MODE == SHARC_MEDIAN_ABSOLUTE_DEVIATION_FILTER
    // This frame's mean is one observation of the luminance stream; only per-frame sums reach the resolve
    // pass, so the median and MAD describe the frame means, not individual paths. A fresh record (MAD 0) is
    // seeded here; a record recovered by the linear probe above carries its statistics and is not.
    uint logMadHalfWindow = clamp(resolveParameters.logMadHalfWindow, SHARC_LOG_MAD_HALF_WINDOW_MIN, SHARC_LOG_MAD_HALF_WINDOW_MAX);
    SharcUpdateLogStatistics(luminance, SharcRadianceLuma(accumulatedRadiance), 2.0f * float(logMadHalfWindow) + 1.0f);
#endif

    accumulatedRadiance = SharcAddRadianceData(SharcScaleRadianceData(accumulatedRadiancePrev, sampleNumPrev * sampleTotalInv), SharcScaleRadianceData(accumulatedRadiance, sampleNum * sampleTotalInv));
    float accumulatedSampleNum = sampleNumPrev + sampleNum;

#if SHARC_BLEND_ADJACENT_LEVELS
    // Reproject sample from adjacent level
    float3 cameraOffset = sharcParameters.hashGridParameters.cameraPosition.xyz - resolveParameters.cameraPositionPrev.xyz;
    if (!isResponsiveSignal && (dot(cameraOffset, cameraOffset) > 1e-6f) && (accumulatedFrameNum <= 2))
    {
        HashGridKey adjacentLevelHashKey = SharcGetAdjacentLevelHashKey(hashGridKey, sharcParameters.hashGridParameters, resolveParameters.cameraPositionPrev);

        HashGridIndex hashGridIndex;
        uint baseSlot = HashGridGetBaseSlot(adjacentLevelHashKey, sharcParameters.hashGridData.capacity);
        uint hashCollisionsNum;
        if (HashGridFind(sharcParameters.hashGridData, adjacentLevelHashKey, baseSlot, HASH_GRID_HASH_MAP_BUCKET_SIZE, hashGridIndex, hashCollisionsNum))
        {
            SharcPackedData adjacentPackedDataPrev = BUFFER_AT_OFFSET(sharcParameters.resolvedBuffer, hashGridIndex);
            SharcVoxelData adjacentVoxelDataPrev = SharcUnpackVoxelData(adjacentPackedDataPrev);
            float adjacentSampleNum = adjacentVoxelDataPrev.accumulatedSampleNum;
            if (adjacentSampleNum > SHARC_SAMPLE_NUM_THRESHOLD)
            {
                float blendWeight = rcp(adjacentSampleNum + accumulatedSampleNum);

#if SHARC_ENABLE_FILTER_MODE == SHARC_STANDARD_DEVIATION_FILTER
                // The neighbour enters the variance as one observation of weight adjacentSampleNum, like a
                // frame. Its own M2 is deliberately not imported: this record's frame count is not increased
                // by the blend, so pooling a converged neighbour's spread into a young voxel would inflate
                // the per-frame variance by the ratio of their frame counts.
                luminance.m2 = SharcMergeLuminanceM2(luminance.m2, SharcRadianceLuma(accumulatedRadiance), accumulatedSampleNum, SharcRadianceLuma(adjacentVoxelDataPrev.accumulatedRadiance), adjacentSampleNum);
#endif
                // (median / MAD path: order statistics have no pooled form; the neighbour's mean is not an
                // observation of this voxel's stream, so the statistics are left as they are.)

                accumulatedRadiance = SharcAddRadianceData(
                    SharcScaleRadianceData(adjacentVoxelDataPrev.accumulatedRadiance, adjacentSampleNum * blendWeight),
                    SharcScaleRadianceData(accumulatedRadiance, accumulatedSampleNum * blendWeight));
                accumulatedSampleNum += adjacentSampleNum;
            }
        }
    }
#endif // SHARC_BLEND_ADJACENT_LEVELS

#if SHARC_ENABLE_FILTER_MODE == SHARC_STANDARD_DEVIATION_FILTER
    // Every term added above is >= 0; this exists to map a NaN to 0 (DXIL fmax drops the NaN operand) so a
    // single corrupt frame cannot poison the recursion permanently.
    luminance.m2 = max(luminance.m2, 0.0f);
#endif
#if SHARC_ENABLE_FILTER_MODE != SHARC_FILTER_DISABLED
    sharcVoxelData.sampleDataExt = SharcPackLuminanceStats(luminance);   // the statistic takes the word; fade acceleration is compiled out with a filter mode (see the #error at the top)
#endif
    BUFFER_AT_OFFSET(sharcParameters.resolvedBuffer, entryIndex) = SharcPackVoxelData(accumulatedRadiance, accumulatedSampleNum, accumulatedFrameNum, staleFrameNum, sharcVoxelData.sampleDataExt);

#if !SHARC_ENABLE_RESPONSIVE_LIGHTING
    // Clear buffer entry for the next frame
    SharcAccumulationData zeroAccumulationData = SharcZeroAccumulationData();
    BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, entryIndex) = zeroAccumulationData;
#endif // !SHARC_ENABLE_RESPONSIVE_LIGHTING
}
