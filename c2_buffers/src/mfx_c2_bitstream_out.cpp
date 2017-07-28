/********************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or nondisclosure
agreement with Intel Corporation and may not be copied or disclosed except in
accordance with the terms of that agreement
Copyright(c) 2017 Intel Corporation. All Rights Reserved.

*********************************************************************************/

#include "mfx_c2_bitstream_out.h"
#include "mfx_debug.h"
#include "mfx_c2_utils.h"
#include "mfx_c2_debug.h"

using namespace android;

static void InitMfxBitstream(
    uint8_t* raw, uint32_t capacity, mfxBitstream* mfx_bitstream)
{
    MFX_DEBUG_TRACE_FUNC;

    memset(mfx_bitstream, 0, sizeof(mfxBitstream));

    mfx_bitstream->Data = raw;
    mfx_bitstream->MaxLength = capacity;
}

status_t MfxBitstreamWrapper::Create(
    std::shared_ptr<C2LinearBlock> block, nsecs_t timeout,
    MfxBitstreamWrapper* wrapper)
{
    MFX_DEBUG_TRACE_FUNC;

    C2Error res = C2_OK;

    do {

        if (nullptr == wrapper) {
            res = C2_BAD_VALUE;
            break;
        }

        uint8_t* raw = nullptr;

        res = MapLinearBlock(*block, timeout, &raw);
        if (C2_OK != res) break;

        wrapper->mfx_bitstream_ = std::make_unique<mfxBitstream>();
        wrapper->c2_linear_block_ = block;

        InitMfxBitstream(raw, block->capacity(), wrapper->mfx_bitstream_.get());

    } while(false);

    MFX_DEBUG_TRACE_android_status_t(res);
    return res;
}
