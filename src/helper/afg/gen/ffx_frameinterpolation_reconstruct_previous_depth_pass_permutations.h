#include "ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb.h"
#include "ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a.h"

typedef union ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationKey {
    struct {
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS : 1;
        uint32_t FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS : 1;
    };
    uint32_t index;
} ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationKey;

typedef struct ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationInfo {
    const uint32_t       blobSize;
    const unsigned char* blobData;


    const uint32_t  numConstantBuffers;
    const char**    constantBufferNames;
    const uint32_t* constantBufferBindings;
    const uint32_t* constantBufferCounts;
    const uint32_t* constantBufferSpaces;

    const uint32_t  numSRVTextures;
    const char**    srvTextureNames;
    const uint32_t* srvTextureBindings;
    const uint32_t* srvTextureCounts;
    const uint32_t* srvTextureSpaces;

    const uint32_t  numUAVTextures;
    const char**    uavTextureNames;
    const uint32_t* uavTextureBindings;
    const uint32_t* uavTextureCounts;
    const uint32_t* uavTextureSpaces;

    const uint32_t  numSRVBuffers;
    const char**    srvBufferNames;
    const uint32_t* srvBufferBindings;
    const uint32_t* srvBufferCounts;
    const uint32_t* srvBufferSpaces;

    const uint32_t  numUAVBuffers;
    const char**    uavBufferNames;
    const uint32_t* uavBufferBindings;
    const uint32_t* uavBufferCounts;
    const uint32_t* uavBufferSpaces;

    const uint32_t  numSamplers;
    const char**    samplerNames;
    const uint32_t* samplerBindings;
    const uint32_t* samplerCounts;
    const uint32_t* samplerSpaces;

    const uint32_t  numRTAccelerationStructures;
    const char**    rtAccelerationStructureNames;
    const uint32_t* rtAccelerationStructureBindings;
    const uint32_t* rtAccelerationStructureCounts;
    const uint32_t* rtAccelerationStructureSpaces;
} ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationInfo;

static const uint32_t g_ffx_frameinterpolation_reconstruct_previous_depth_pass_IndirectionTable[] = {
    1,
    0,
    1,
    0,
    1,
    0,
    1,
    0,
};

static const ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationInfo g_ffx_frameinterpolation_reconstruct_previous_depth_pass_PermutationInfo[] = {
    { g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_size, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_data, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_CBVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_CBVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_CBVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_CBVResourceSets, 3, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_TextureSRVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_TextureSRVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_TextureSRVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_TextureSRVResourceSets, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_TextureUAVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_TextureUAVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_TextureUAVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_TextureUAVResourceSets, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_SamplerResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_SamplerResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_SamplerResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_9e7db1e4cf361fd821b0f05246e09fbb_SamplerResourceSets, 0, 0, 0, 0, 0, },
    { g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_size, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_data, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_CBVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_CBVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_CBVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_CBVResourceSets, 3, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_TextureSRVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_TextureSRVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_TextureSRVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_TextureSRVResourceSets, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_TextureUAVResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_TextureUAVResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_TextureUAVResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_TextureUAVResourceSets, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_SamplerResourceNames, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_SamplerResourceBindings, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_SamplerResourceCounts, g_ffx_frameinterpolation_reconstruct_previous_depth_pass_90f2c6678f12a74d65bd5aa3c9cffd4a_SamplerResourceSets, 0, 0, 0, 0, 0, },
};

