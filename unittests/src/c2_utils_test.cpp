/********************************************************************************

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or nondisclosure
agreement with Intel Corporation and may not be copied or disclosed except in
accordance with the terms of that agreement
Copyright(c) 2017 Intel Corporation. All Rights Reserved.

*********************************************************************************/

#include "gtest_emulation.h"
#include "mfx_cmd_queue.h"
#include "mfx_pool.h"
#include "mfx_gralloc_allocator.h"
#include "mfx_va_allocator.h"
#include "mfx_frame_pool_allocator.h"
#include "C2BlockAllocator.h"
#include "mfx_c2_utils.h"
#include <map>
#include <set>

#ifdef LIBVA_SUPPORT
#include "mfx_dev_va.h"
#include <va/va_android.h>
#else
#include "mfx_dev_android.h"
#endif

using namespace android;

static const size_t CMD_COUNT = 10;

// Tests abstract command queue processed all supplied tasks in correct order.
TEST(MfxCmdQueue, ProcessAll)
{
    MfxCmdQueue queue;
    queue.Start();

    std::vector<size_t> result;

    for(size_t i = 0; i < CMD_COUNT; ++i) {

        std::unique_ptr<int> ptr_i = std::make_unique<int>(i);

        // lambda mutable and not copy-assignable to assert MfxCmdQueue supports it
        auto task = [ ptr_i = std::move(ptr_i), &result] () mutable {
            result.push_back(*ptr_i);
            ptr_i = nullptr;
        };

        queue.Push(std::move(task));
    }

    queue.Stop();

    EXPECT_EQ(result.size(), CMD_COUNT);
    for(size_t i = 0; i < result.size(); ++i) {
        EXPECT_EQ(result[i], i);
    }
}

// Tests that MfxCmdQueue::Stop is waiting for the end of all pushed tasks.
TEST(MfxCmdQueue, Stop)
{
    MfxCmdQueue queue;
    queue.Start();

    std::vector<size_t> result;

    auto timeout = std::chrono::milliseconds(1);
    for(size_t i = 0; i < CMD_COUNT; ++i) {

        queue.Push( [i, timeout, &result] {

            std::this_thread::sleep_for(timeout);
            result.push_back(i);
        } );

        // progressively increase timeout to be sure some of them will not be processed
        // by moment of Stop
        timeout *= 2;
    }

    queue.Stop();

    EXPECT_EQ(result.size(), CMD_COUNT); // all commands should be executed
    for(size_t i = 0; i < result.size(); ++i) {
        EXPECT_EQ(result[i], i);
    }
}

// Tests that MfxCmdQueue::Abort is not waiting for the end of all pushed tasks.
// At least some tasks should not be processed.
TEST(MfxCmdQueue, Abort)
{
    MfxCmdQueue queue;
    queue.Start();

    std::vector<size_t> result;

    auto timeout = std::chrono::milliseconds(1);
    for(size_t i = 0; i < CMD_COUNT; ++i) {

        queue.Push( [i, timeout, &result] {

            std::this_thread::sleep_for(timeout);
            result.push_back(i);
        } );

        // progressively increase timeout to be sure some of them will not be processed
        timeout *= 2;
    }

    queue.Abort();

    EXPECT_EQ(result.size() < CMD_COUNT, true); // some commands must be dropped
    for(size_t i = 0; i < result.size(); ++i) {
        EXPECT_EQ(result[i], i);
    }
}

// Tests that MfxPool allocates values among appended
// and if no resources available, correctly waits for freeing resources.
// Also checks allocated values are valid after pool destruction.
TEST(MfxPool, Alloc)
{
    const int COUNT = 10;
    std::shared_ptr<int> allocated_again[COUNT];

    {
        MfxPool<int> pool;
        // append range of numbers
        for (int i = 0; i < COUNT; ++i) {
            pool.Append(std::make_shared<int>(i));
        }

        std::shared_ptr<int> allocated[COUNT];
        for (int i = 0; i < COUNT; ++i) {
            allocated[i] = pool.Alloc();
            EXPECT_EQ(*allocated[i], i); // check values are those appended
        }

        std::thread free_thread([&] {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            for (int i = 0; i < COUNT; ++i) {
                allocated[i].reset();
            }
        });

        auto start = std::chrono::system_clock::now();
        for (int i = 0; i < COUNT; ++i) {
            allocated_again[i] = pool.Alloc(); // this Alloc should wait for free in free_thread
            EXPECT_EQ(*allocated_again[i], i); // and got the same value
        }
        auto end = std::chrono::system_clock::now();
        std::chrono::duration<double> elapsed_seconds = end - start;
        // elapsed time is around 1 second
        EXPECT_TRUE((0.9 < elapsed_seconds.count()) && (elapsed_seconds.count() < 1.1));

        free_thread.join();
    }
    // check allocated_again have correct values after pool destruction
    for (int i = 0; i < COUNT; ++i) {
        EXPECT_EQ(*allocated_again[i], i);
    }
}

// Tests MfxDev could be created and released significant amount of times.
// For pure build this tests MfxDevAndroid, for VA - MfxDevVa.
TEST(MfxDev, InitCloseNoLeaks)
{
    const int COUNT = 1500;

    for (int i = 0; i < COUNT; ++i)
    {
        std::unique_ptr<MfxDev> device;
        mfxStatus sts = MfxDev::Create(MfxDev::Usage::Decoder, &device);

        EXPECT_EQ(MFX_ERR_NONE, sts);
        EXPECT_NE(device, nullptr);

        sts = device->Close();
        EXPECT_EQ(MFX_ERR_NONE, sts);
    }
}

static void CheckNV12PlaneLayout(uint16_t width, uint16_t height, const C2PlaneLayout& layout)
{
    using Layout = C2PlaneLayout;
    using Info = C2PlaneInfo;

    EXPECT_EQ(layout.mType, Layout::MEDIA_IMAGE_TYPE_YUV);
    EXPECT_EQ(layout.mNumPlanes, 3);

    std::map<Layout::PlaneIndex, Info::Channel> expected_channels = {
        {  Layout::Y, Info::Y },
        {  Layout::U, Info::Cb },
        {  Layout::V, Info::Cr },
    };

    for (Layout::PlaneIndex index : { Layout::Y, Layout::U, Layout::V }) {
        EXPECT_EQ(layout.mPlanes[index].mChannel, expected_channels[index]);
        EXPECT_EQ(layout.mPlanes[index].mColInc, index == Layout::Y ? 1 : 2);
        EXPECT_TRUE(layout.mPlanes[index].mRowInc >= width);
        EXPECT_EQ(layout.mPlanes[index].mHorizSubsampling, index == Layout::Y ? 1 : 2);
        EXPECT_EQ(layout.mPlanes[index].mVertSubsampling, index == Layout::Y ? 1 : 2);
        EXPECT_EQ(layout.mPlanes[index].mBitDepth, 8);
        EXPECT_EQ(layout.mPlanes[index].mAllocatedDepth, 8);

        if (index != Layout::Y) EXPECT_TRUE(layout.mPlanes[index].mOffset >= width * height);
    }
    EXPECT_EQ(layout.mPlanes[Layout::Y].mOffset, 0);
    EXPECT_EQ(layout.mPlanes[Layout::U].mOffset + 1, layout.mPlanes[Layout::V].mOffset);
}

static void CheckMfxFrameData(mfxU32 fourcc, uint16_t width, uint16_t height,
    bool hw_memory, bool locked, const mfxFrameData& frame_data)
{
    EXPECT_EQ(frame_data.PitchHigh, 0);
    uint32_t pitch = MakeUint32(frame_data.PitchHigh, frame_data.PitchLow);

    if (fourcc != MFX_FOURCC_P8) {
        EXPECT_TRUE(pitch >= width);
    }
    EXPECT_EQ(frame_data.MemId != nullptr, hw_memory);

    bool pointers_expected = locked || !hw_memory;
    bool color = (fourcc != MFX_FOURCC_P8);

    EXPECT_EQ(pointers_expected, frame_data.Y != nullptr);
    EXPECT_EQ(pointers_expected && color, frame_data.UV != nullptr);
    EXPECT_EQ(pointers_expected && color, frame_data.V != nullptr);

    if(pointers_expected && color) {
        EXPECT_TRUE(frame_data.Y + pitch * height <= frame_data.UV);
        EXPECT_EQ(frame_data.UV + 1, frame_data.V);
    }
    EXPECT_EQ(frame_data.A, nullptr);
}

static uint8_t PlanePixelValue(uint16_t x, uint16_t y, uint32_t plane_index, int frame_index)
{
    return (uint8_t)(x + y + plane_index + frame_index);
}

typedef std::function<void(uint16_t x, uint16_t y, uint32_t plane_index, uint8_t* plane_pixel)> ProcessPlanePixel;

static void ForEveryPlanePixel(uint16_t width, uint16_t height, const C2PlaneLayout& layout,
    const ProcessPlanePixel& process_function, uint8_t* data)
{
    for (uint32_t i = 0; i < layout.mNumPlanes; ++i) {
        const C2PlaneInfo& plane = layout.mPlanes[i];

        uint8_t* row = data + plane.mOffset;
        for (uint16_t y = 0; y < height; y += plane.mVertSubsampling) {
            uint8_t* pixel = row;
            for (uint16_t x = 0; x < width; x += plane.mHorizSubsampling) {
                process_function(x, y, i, pixel);
                pixel += plane.mColInc;
            }
            row += plane.mRowInc;
        }
    }
}

static void ForEveryPlanePixel(uint16_t width, uint16_t height, const mfxFrameInfo& frame_info,
    const ProcessPlanePixel& process_function, const mfxFrameData& frame_data)
{
    const int planes_count_max = 3;
    uint8_t* planes_data[planes_count_max] = { frame_data.Y, frame_data.UV, frame_data.UV + 1 };
    const uint16_t planes_vert_subsampling[planes_count_max] = { 1, 2, 2 };
    const uint16_t planes_horz_subsampling[planes_count_max] = { 1, 2, 2 };
    const uint16_t planes_col_inc[planes_count_max] = { 1, 2, 2 };

    int planes_count = -1;

    switch (frame_info.FourCC) {
        case MFX_FOURCC_NV12:
            EXPECT_EQ(frame_info.ChromaFormat, MFX_CHROMAFORMAT_YUV420);
            planes_count = 3;
            break;
        case MFX_FOURCC_P8:
            EXPECT_EQ(frame_info.ChromaFormat, MFX_CHROMAFORMAT_MONOCHROME);
            planes_count = 1;
            // buffer is linear, set up width and height to one line
            width = EstimatedEncodedFrameLen(width, height);
            height = 1;
            break;
        default:
            EXPECT_TRUE(false) << "unsupported color format";
    }

    uint32_t pitch = MakeUint32(frame_data.PitchHigh, frame_data.PitchLow);

    for (int i = 0; i < planes_count; ++i) {

        uint8_t* row = planes_data[i];
        for (uint16_t y = 0; y < height; y += planes_vert_subsampling[i]) {
            uint8_t* pixel = row;
            for (uint16_t x = 0; x < width; x += planes_horz_subsampling[i]) {
                process_function(x, y, i, pixel);
                pixel += planes_col_inc[i];
            }
            row += pitch;
        }
    }
}

// Fills frame planes with PlanePixelValue pattern.
// Value should depend on plane index, frame index, x and y.
template<typename FrameInfo, typename FrameData>
static void FillFrameContents(uint16_t width, uint16_t height, int frame_index,
    const FrameInfo& frame_info, const FrameData& frame_data)
{
    ProcessPlanePixel process = [frame_index] (uint16_t x, uint16_t y, uint32_t plane_index, uint8_t* plane_pixel) {
        *plane_pixel = PlanePixelValue(x, y, plane_index, frame_index);
    };
    ForEveryPlanePixel(width, height, frame_info, process, frame_data);
}

template<typename FrameInfo, typename FrameData>
static void CheckFrameContents(uint16_t width, uint16_t height, int frame_index,
    const FrameInfo& frame_info, const FrameData& frame_data)
{
    int fails_count = 0;

    ProcessPlanePixel process = [frame_index, &fails_count] (uint16_t x, uint16_t y, uint32_t plane_index, uint8_t* plane_pixel) {
        if (fails_count < 10) { // to not overflood output
            uint8_t actual = *plane_pixel;
            uint8_t expected = PlanePixelValue(x, y, plane_index, frame_index);
            bool match = (actual == expected);
            if (!match) ++fails_count;
            EXPECT_TRUE(match) << NAMED(x) << NAMED(y) << NAMED(plane_index)
                << NAMED((int)actual) << NAMED((int)expected);
        }
    };
    ForEveryPlanePixel(width, height, frame_info, process, frame_data);
}

// Tests gralloc allocator ability to alloc and free buffers.
// The allocated buffer is locked, filled memory with some pattern,
// unlocked, then locked again, memory pattern should the same.
TEST(MfxGrallocAllocator, BufferKeepsContents)
{
    std::unique_ptr<MfxGrallocAllocator> allocator;

    status_t res = MfxGrallocAllocator::Create(&allocator);
    EXPECT_EQ(res, C2_OK);
    EXPECT_NE(allocator, nullptr);

    const int WIDTH = 600;
    const int HEIGHT = 400;
    const int FRAME_COUNT = 3;

    if (allocator) {

        buffer_handle_t handle[FRAME_COUNT] {};
        status_t res;

        for (int i = 0; i < FRAME_COUNT; ++i) {
            res = allocator->Alloc(WIDTH, HEIGHT, &handle[i]);
            EXPECT_EQ(res, C2_OK);
            EXPECT_NE(handle, nullptr);
        }

        for (int i = 0; i < FRAME_COUNT; ++i) {
            uint8_t* data {};
            android::C2PlaneLayout layout {};
            res = allocator->LockFrame(handle[i], &data, &layout);
            EXPECT_EQ(res, C2_OK);
            EXPECT_NE(data, nullptr);

            CheckNV12PlaneLayout(WIDTH, HEIGHT, layout);

            FillFrameContents(WIDTH, HEIGHT, i, layout, data);

            res = allocator->UnlockFrame(handle[i]);
            EXPECT_EQ(res, C2_OK);
        }

        for (int i = 0; i < FRAME_COUNT; ++i) {
            uint8_t* data {};
            android::C2PlaneLayout layout {};
            res = allocator->LockFrame(handle[i], &data, &layout);
            EXPECT_EQ(res, C2_OK);
            EXPECT_NE(data, nullptr);

            CheckNV12PlaneLayout(WIDTH, HEIGHT, layout);

            CheckFrameContents(WIDTH, HEIGHT, i, layout, data);

            res = allocator->UnlockFrame(handle[i]);
            EXPECT_EQ(res, C2_OK);
        }

        for (int i = 0; i < FRAME_COUNT; ++i) {
            res = allocator->Free(handle[i]);
            EXPECT_EQ(res, C2_OK);
        }
    }
}

#ifdef LIBVA_SUPPORT

static void InitFrameInfo(mfxU32 fourcc, uint16_t width, uint16_t height, mfxFrameInfo* frame_info)
{
    *frame_info = mfxFrameInfo {};
    frame_info->BitDepthLuma = 8;
    frame_info->BitDepthChroma = 8;
    frame_info->FourCC = fourcc;

    switch (fourcc) {
        case MFX_FOURCC_NV12:
            frame_info->ChromaFormat = MFX_CHROMAFORMAT_YUV420;
            break;
        case MFX_FOURCC_P8:
            frame_info->ChromaFormat = MFX_CHROMAFORMAT_MONOCHROME;
            break;
        default:
            ASSERT_TRUE(false) << std::hex << fourcc << " format is not supported";
    }

    frame_info->Width = width;
    frame_info->Height = height;
    frame_info->CropX = 0;
    frame_info->CropY = 0;
    frame_info->CropW = width;
    frame_info->CropH = height;
    frame_info->FrameRateExtN = 30;
    frame_info->FrameRateExtD = 1;
    frame_info->PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
}

class UtilsVaContext
{
private:
    VAConfigID va_config_ { VA_INVALID_ID };

    VAContextID va_context_ { VA_INVALID_ID };

    VADisplay va_display_ { nullptr };

public:
    UtilsVaContext(VADisplay va_display, int width, int height)
        : va_display_(va_display)
    {
        VAConfigAttrib attrib[2];
        mfxI32 numAttrib = MFX_GET_ARRAY_SIZE(attrib);
        attrib[0].type = VAConfigAttribRTFormat;
        attrib[0].value = VA_RT_FORMAT_YUV420;
        attrib[1].type = VAConfigAttribRateControl;
        attrib[1].value = VA_RC_CQP;

        mfxU32 flag = VA_PROGRESSIVE;

        VAProfile va_profile = VAProfileH264ConstrainedBaseline;
        VAEntrypoint entrypoint = VAEntrypointEncSlice;
        VAStatus sts = vaCreateConfig(va_display_, va_profile, entrypoint, attrib, numAttrib, &va_config_);
        EXPECT_EQ(sts, VA_STATUS_SUCCESS);
        EXPECT_NE(va_config_, VA_INVALID_ID);

        if (VA_INVALID_ID != va_config_) {
            sts = vaCreateContext(va_display_, va_config_, width, height, flag, nullptr, 0, &va_context_);
            EXPECT_EQ(sts, VA_STATUS_SUCCESS);
            EXPECT_NE(va_context_, VA_INVALID_ID);
        }
    }

    ~UtilsVaContext()
    {
        if (va_config_ != VA_INVALID_ID) vaDestroyConfig(va_display_, va_config_);
        if (va_context_ != VA_INVALID_ID) vaDestroyContext(va_display_, va_context_);
    }

    VAContextID GetVaContext() { return va_context_; }
};

struct MfxAllocTestRun {
    int width;
    int height;
    int frame_count;
    mfxU32 fourcc;
};

typedef std::function<void (const MfxAllocTestRun& run, MfxFrameAllocator* allocator,
    mfxFrameAllocRequest& request, mfxFrameAllocResponse& response)> MfxVaAllocatorTestStep;

static void MfxVaAllocatorTest(const std::vector<MfxVaAllocatorTestStep>& steps, int repeat_count = 1)
{
    MfxDevVa* dev_va = new MfxDevVa(MfxDev::Usage::Encoder);
    std::unique_ptr<MfxDev> dev { dev_va };

    mfxStatus sts = dev->Init();
    EXPECT_EQ(MFX_ERR_NONE, sts);

    MfxFrameAllocator* allocator = dev->GetFrameAllocator();
    EXPECT_NE(allocator, nullptr);

    if (allocator) {

        MfxAllocTestRun test_allocations[] {
            { 600, 400, 3, MFX_FOURCC_NV12 },
            { 320, 240, 2, MFX_FOURCC_NV12 },
            { 1920, 1080, 3, MFX_FOURCC_NV12 },
            { 1280, 720, 3, MFX_FOURCC_P8 },
        };

        mfxFrameAllocResponse responses[MFX_GET_ARRAY_SIZE(test_allocations)] {};
        mfxFrameAllocRequest requests[MFX_GET_ARRAY_SIZE(test_allocations)] {};
        std::unique_ptr<UtilsVaContext> va_contexts[MFX_GET_ARRAY_SIZE(test_allocations)];

        for (MfxAllocTestRun& run : test_allocations) {
            if (run.fourcc == MFX_FOURCC_P8) {
                va_contexts[&run - test_allocations] =
                    std::make_unique<UtilsVaContext>(dev_va->GetVaDisplay(), run.width, run.height);
            }
        }

        for (int i = 0; i < repeat_count; ++i) {
            for (auto& step : steps) {
                for (const MfxAllocTestRun& run : test_allocations) {
                    const int index = &run - test_allocations;
                    mfxFrameAllocResponse& response = responses[index];
                    mfxFrameAllocRequest& request = requests[index];

                    if (va_contexts[index]) {

                        if (va_contexts[index]->GetVaContext() == VA_INVALID_ID) continue;

                        request.AllocId = va_contexts[index]->GetVaContext();
                    }

                    step(run, allocator, request, response);
                }
            }
        }
    }
    dev->Close();
}

static void MfxFrameAlloc(const MfxAllocTestRun& run, MfxFrameAllocator* allocator,
    mfxFrameAllocRequest& request, mfxFrameAllocResponse& response)
{
    request.Type = MFX_MEMTYPE_FROM_ENCODE;
    request.NumFrameMin = run.frame_count;
    request.NumFrameSuggested = run.frame_count;
    InitFrameInfo(run.fourcc, run.width, run.height, &request.Info);

    mfxStatus sts = allocator->AllocFrames(&request, &response);
    EXPECT_EQ(sts, MFX_ERR_NONE);
    EXPECT_EQ(response.NumFrameActual, request.NumFrameMin);

    EXPECT_NE(response.mids, nullptr);
}

static void MfxFrameFree(const MfxAllocTestRun&, MfxFrameAllocator* allocator,
    mfxFrameAllocRequest&, mfxFrameAllocResponse& response)
{
    mfxStatus sts = allocator->FreeFrames(&response);
    EXPECT_EQ(MFX_ERR_NONE, sts);
}

// Tests mfxFrameAllocator implementation on VA.
// Checks Alloc and Free don't return any errors.
// Repeated many times to check possible memory leaks.
TEST(MfxVaAllocator, AllocFreeNoLeaks)
{
    const int COUNT = 1000;
    MfxVaAllocatorTest( { MfxFrameAlloc, MfxFrameFree }, COUNT );
}

// Tests mfxFrameAllocator implementation on VA.
// Executes GetFrameHDL on every allocated mem_id and assures all returned handles are different.
TEST(MfxVaAllocator, GetFrameHDL)
{
    std::set<mfxHDL> all_frame_handles;
    auto get_frame_hdl_test = [&all_frame_handles] (const MfxAllocTestRun& run, MfxFrameAllocator* allocator,
        mfxFrameAllocRequest&, mfxFrameAllocResponse& response) {

        EXPECT_NE(response.mids, nullptr);
        if (response.mids) {
            for (int i = 0; i < run.frame_count; ++i) {
                EXPECT_NE(response.mids[i], nullptr);

                mfxHDL frame_handle {};
                mfxStatus sts = allocator->GetFrameHDL(response.mids[i], &frame_handle);
                EXPECT_EQ(MFX_ERR_NONE, sts);
                EXPECT_NE(frame_handle, nullptr);

                EXPECT_EQ(all_frame_handles.find(frame_handle), all_frame_handles.end()); // test uniqueness
                all_frame_handles.insert(frame_handle);
            }
        }
    };

    MfxVaAllocatorTest( { MfxFrameAlloc, get_frame_hdl_test, MfxFrameFree } );
}

// Tests mfxFrameAllocator implementation on VA.
// The allocated buffer is locked, memory filled with some pattern,
// unlocked, then locked again, memory pattern should the same.
TEST(MfxVaAllocator, BufferKeepsContents)
{
    const bool hw_memory = true;
    const bool locked = true;

    auto lock_frame = [] (const MfxAllocTestRun& run, MfxFrameAllocator* allocator,
        mfxFrameAllocRequest& request, mfxFrameAllocResponse& response) {

        for (int i = 0; i < run.frame_count; ++i) {
            mfxFrameData frame_data {};
            mfxStatus sts = allocator->LockFrame(response.mids[i], &frame_data);
            EXPECT_EQ(MFX_ERR_NONE, sts);

            CheckMfxFrameData(run.fourcc, run.width, run.height, hw_memory, locked, frame_data);

            FillFrameContents(run.width, run.height, i, request.Info, frame_data);

            sts = allocator->UnlockFrame(response.mids[i], &frame_data);
            EXPECT_EQ(MFX_ERR_NONE, sts);
        }
    };

    auto unlock_frame = [] (const MfxAllocTestRun& run, MfxFrameAllocator* allocator,
        mfxFrameAllocRequest& request, mfxFrameAllocResponse& response) {

        for (int i = 0; i < run.frame_count; ++i) {
            mfxFrameData frame_data {};
            mfxStatus sts = allocator->LockFrame(response.mids[i], &frame_data);
            EXPECT_EQ(MFX_ERR_NONE, sts);

            CheckMfxFrameData(run.fourcc, run.width, run.height, hw_memory, locked, frame_data);

            CheckFrameContents(run.width, run.height, i, request.Info, frame_data);

            sts = allocator->UnlockFrame(response.mids[i], &frame_data);
            EXPECT_EQ(MFX_ERR_NONE, sts);
        }
    };

    MfxVaAllocatorTest( { MfxFrameAlloc, lock_frame, unlock_frame, MfxFrameFree } );
}

typedef std::function<void (MfxGrallocAllocator* gr_allocator, MfxFrameAllocator* allocator,
    MfxFrameConverter* converter)> MfxFrameConverterTestStep;

static void MfxFrameConverterTest(const std::vector<MfxFrameConverterTestStep>& steps, int repeat_count = 1)
{
    std::unique_ptr<MfxGrallocAllocator> gr_allocator;

    status_t res = MfxGrallocAllocator::Create(&gr_allocator);
    EXPECT_EQ(res, C2_OK);
    EXPECT_NE(gr_allocator, nullptr);

    std::unique_ptr<MfxDev> dev { new MfxDevVa(MfxDev::Usage::Encoder) };

    mfxStatus sts = dev->Init();
    EXPECT_EQ(MFX_ERR_NONE, sts);

    MfxFrameAllocator* allocator = dev->GetFrameAllocator();
    EXPECT_NE(allocator, nullptr);

    MfxFrameConverter* converter = dev->GetFrameConverter();
    EXPECT_NE(converter, nullptr);

    if (gr_allocator && allocator && converter) {
        for (int i = 0; i < repeat_count; ++i) {
            for (auto& step : steps) {
                step(gr_allocator.get(), allocator, converter);
            }
        }
    }

    dev->Close();
}

// Allocates some gralloc frames, fills them with pattern,
// wires them up with mfxMemID (VA surface inside),
// locks mfxFrames and checks a pattern is the same.
// Then locks mfxFrames again, fills them with different pattern
// and checks original gralloc buffers get updated pattern.
// These steps prove modifications go from gralloc to VA and back.
TEST(MfxFrameConverter, GrallocContentsMappedToVa)
{
    const int WIDTH = 600;
    const int HEIGHT = 400;
    const int FRAME_COUNT = 3;

    buffer_handle_t handles[FRAME_COUNT] {};
    mfxMemId mfx_mem_ids[FRAME_COUNT] {};
    status_t res = C2_OK;
    mfxStatus sts = MFX_ERR_NONE;

    // gralloc allocation step
    auto gr_alloc = [&] (MfxGrallocAllocator* gr_allocator, MfxFrameAllocator*, MfxFrameConverter*) {

        for (int i = 0; i < FRAME_COUNT; ++i) {
            res = gr_allocator->Alloc(WIDTH, HEIGHT, &handles[i]);
            EXPECT_EQ(res, C2_OK);
            EXPECT_NE(handles, nullptr);
        }
    };

    // gralloc allocation step
    auto gr_free = [&] (MfxGrallocAllocator* gr_allocator, MfxFrameAllocator*, MfxFrameConverter*) {

        for (int i = 0; i < FRAME_COUNT; ++i) {
            res = gr_allocator->Free(handles[i]);
            EXPECT_EQ(res, C2_OK);
        }
    };

    // operation on frame mapped from gralloc to system memory
    typedef std::function<void (int frame_index, const C2PlaneLayout& layout, uint8_t* data)> GrMemOperation;
    // lambda constructing test step doing: gralloc Lock, some specific work on locked memory, gralloc unlock
    auto do_gr_mem_operation = [&] (GrMemOperation gr_mem_operation) {
        return [&] (MfxGrallocAllocator* gr_allocator, MfxFrameAllocator*, MfxFrameConverter*) {

            for (int i = 0; i < FRAME_COUNT; ++i) {
                uint8_t* data {};
                android::C2PlaneLayout layout {};
                res = gr_allocator->LockFrame(handles[i], &data, &layout);
                EXPECT_EQ(res, C2_OK);
                EXPECT_NE(data, nullptr);

                CheckNV12PlaneLayout(WIDTH, HEIGHT, layout);

                gr_mem_operation(i, layout, data);

                res = gr_allocator->UnlockFrame(handles[i]);
                EXPECT_EQ(res, C2_OK);
            };
        };
    };

    // gralloc to va wiring step
    auto gr_convert_to_va = [&] (MfxGrallocAllocator*, MfxFrameAllocator*, MfxFrameConverter* converter) {

        for (int i = 0; i < FRAME_COUNT; ++i) {
            bool decode_target { false };
            mfxStatus mfx_sts = converter->ConvertGrallocToVa(handles[i], decode_target, &mfx_mem_ids[i]);
            EXPECT_EQ(MFX_ERR_NONE, mfx_sts);
            EXPECT_NE(mfx_mem_ids[i], nullptr);
        }
    };

    // operation on frame mapped from va to system memory
    typedef std::function<void (int frame_index,
        const mfxFrameInfo& frame_info, mfxFrameData& frame_data)> VaMemOperation;
    // lambda constructing test step doing: va Lock, some specific work on locked memory, va unlock
    auto do_va_mem_operation = [&] (VaMemOperation va_mem_operation) {
        return [&] (MfxGrallocAllocator*, MfxFrameAllocator* allocator, MfxFrameConverter*) {

            const bool hw_memory = true;
            const bool locked = true;

            mfxFrameInfo frame_info {};
            InitFrameInfo(MFX_FOURCC_NV12, WIDTH, HEIGHT, &frame_info);

            for (int i = 0; i < FRAME_COUNT; ++i) {
                mfxFrameData frame_data {};
                sts = allocator->LockFrame(mfx_mem_ids[i], &frame_data);
                EXPECT_EQ(MFX_ERR_NONE, sts);

                CheckMfxFrameData(MFX_FOURCC_NV12, WIDTH, HEIGHT, hw_memory, locked, frame_data);

                va_mem_operation(i, frame_info, frame_data);

                sts = allocator->UnlockFrame(mfx_mem_ids[i], &frame_data);
                EXPECT_EQ(MFX_ERR_NONE, sts);
            }
        };
    };

    // all test steps together
    MfxFrameConverterTest( {
        gr_alloc,
        do_gr_mem_operation([&] (int frame_index, const C2PlaneLayout& layout, uint8_t* data) {
            FillFrameContents(WIDTH, HEIGHT, frame_index, layout, data); // fill gralloc with pattern #1
        }),
        gr_convert_to_va,
        do_va_mem_operation([&] (int frame_index, const mfxFrameInfo& frame_info, mfxFrameData& frame_data) {
            CheckFrameContents(WIDTH, HEIGHT, frame_index, frame_info, frame_data); // check pattern #1 in va
        }),
        do_va_mem_operation([&] (int frame_index, const mfxFrameInfo& frame_info, mfxFrameData& frame_data) {
            // fill va with pattern #2
            FillFrameContents(WIDTH, HEIGHT, FRAME_COUNT - frame_index, frame_info, frame_data);
        }),
        do_gr_mem_operation([&] (int frame_index, const C2PlaneLayout& layout, uint8_t* data) {
            // check pattern #2 in gralloc
            CheckFrameContents(WIDTH, HEIGHT, FRAME_COUNT - frame_index, layout, data);
        }),
        [] (MfxGrallocAllocator*, MfxFrameAllocator*, MfxFrameConverter* converter) {
            converter->FreeAllMappings();
        },
        gr_free
    } );
}

// Allocates and maps gralloc handles to VA.
// Then frees resources in different ways, checks it works
// significant amount of times.
TEST(MfxFrameConverter, NoLeaks)
{
    const int WIDTH = 1920;
    const int HEIGHT = 1080;
    const int REPEAT_COUNT = 500;

    buffer_handle_t handle {};
    mfxMemId mfx_mem_id {};

    auto alloc_and_map = [&] (MfxGrallocAllocator* gr_allocator, MfxFrameAllocator*,
        MfxFrameConverter* converter) {

        status_t res = gr_allocator->Alloc(WIDTH, HEIGHT, &handle);
        EXPECT_EQ(res, C2_OK);
        EXPECT_NE(handle, nullptr);

        bool decode_target { false };
        mfxStatus mfx_sts = converter->ConvertGrallocToVa(handle, decode_target, &mfx_mem_id);
        EXPECT_EQ(MFX_ERR_NONE, mfx_sts);
        EXPECT_NE(mfx_mem_id, nullptr);
    };

    auto gr_free = [&] (MfxGrallocAllocator* gr_allocator, MfxFrameAllocator*, MfxFrameConverter*) {
        status_t res = gr_allocator->Free(handle);
        EXPECT_EQ(res, C2_OK);
    };

    auto free_all = [] (MfxGrallocAllocator*, MfxFrameAllocator*, MfxFrameConverter* converter) {
        converter->FreeAllMappings();
    };

    MfxFrameConverterTest( { alloc_and_map, free_all, gr_free }, REPEAT_COUNT );

    auto free_by_handles = [&] (MfxGrallocAllocator*, MfxFrameAllocator*,
        MfxFrameConverter* converter) {

        converter->FreeGrallocToVaMapping(handle);
    };

    MfxFrameConverterTest( { alloc_and_map, free_by_handles, gr_free }, REPEAT_COUNT );

    auto free_by_mids = [&] (MfxGrallocAllocator*, MfxFrameAllocator*,
        MfxFrameConverter* converter) {

        converter->FreeGrallocToVaMapping(mfx_mem_id);
    };

    MfxFrameConverterTest( { alloc_and_map, free_by_mids, gr_free }, REPEAT_COUNT );
}

// Checks converter returns the same mem_id for the same gralloc handle.
TEST(MfxFrameConverter, CacheResources)
{
    const int WIDTH = 1920;
    const int HEIGHT = 1080;
    const int REPEAT_COUNT = 10;

    auto test_cache = [&] (MfxGrallocAllocator* gr_allocator, MfxFrameAllocator*,
        MfxFrameConverter* converter) {

        buffer_handle_t handle {};

        status_t res = gr_allocator->Alloc(WIDTH, HEIGHT, &handle);
        EXPECT_EQ(res, C2_OK);
        EXPECT_NE(handle, nullptr);

        mfxMemId mfx_mem_ids[REPEAT_COUNT] {};

        for (int i = 0; i < REPEAT_COUNT; ++i) {
            bool decode_target { false };
            mfxStatus mfx_sts = converter->ConvertGrallocToVa(handle, decode_target, &mfx_mem_ids[i]);
            EXPECT_EQ(MFX_ERR_NONE, mfx_sts);
            EXPECT_NE(mfx_mem_ids[i], nullptr);
        }

        ASSERT_TRUE(REPEAT_COUNT > 1);
        for (int i = 1; i < REPEAT_COUNT; ++i) {
            EXPECT_EQ(mfx_mem_ids[0], mfx_mem_ids[i]);
        }
    };

    MfxFrameConverterTest( { test_cache } );
}

typedef std::function<void (MfxFrameAllocator* allocator, MfxFramePoolAllocator* pool_allocator)> MfxFramePoolAllocatorTestStep;

static void MfxFramePoolAllocatorTest(const std::vector<MfxFramePoolAllocatorTestStep>& steps, int repeat_count = 1)
{
    std::shared_ptr<C2BlockAllocator> c2_allocator;
    status_t res = GetC2BlockAllocator(&c2_allocator);
    EXPECT_EQ(res, C2_OK);
    EXPECT_NE(c2_allocator, nullptr);

    std::unique_ptr<MfxDev> dev { new MfxDevVa(MfxDev::Usage::Decoder) };

    mfxStatus sts = dev->Init();
    EXPECT_EQ(MFX_ERR_NONE, sts);

    if (c2_allocator) {
        MfxFrameAllocator* allocator = dev->GetFrameAllocator();
        EXPECT_NE(allocator, nullptr);
        MfxFramePoolAllocator* pool_allocator = dev->GetFramePoolAllocator();
        EXPECT_NE(pool_allocator, nullptr);
        if (pool_allocator) {

            pool_allocator->SetC2Allocator(c2_allocator);

            for (int i = 0; i < repeat_count; ++i) {
                for (auto& step : steps) {
                    step(allocator, pool_allocator);
                }
            }
        }
    }

    dev->Close();
}

// Tests a typical use sequence for MfxFramePoolAllocator.
// 1) Preallocate pool of frames through MfxFrameAllocator::AllocFrames.
// 2) Acquire C2 Graphic Blocks from the allocator, saves C2 handles and
// their wired MFX Mem IDs for future comparison.
// 3) Free C2 Graphic Blocks by releasing their shared_ptrs.
// 4) Acquire C2 Graphic Blocks again, check C2 handles and
// their wired MFX Mem IDs are the same as saved on step 2.
// 5) Reset allocator - release ownership of allocated C2 handles (no allocated any more).
// 6) Allocate again.
// 7) Check all handles are new.
TEST(MfxFramePoolAllocator, RetainHandles)
{
    const int FRAME_COUNT = 10;
    const int WIDTH = 1920;
    const int HEIGHT = 1080;
    const mfxU32 FOURCC = MFX_FOURCC_NV12;
    std::shared_ptr<C2GraphicBlock> c2_blocks[FRAME_COUNT];

    std::map<const C2Handle*, mfxHDL> handleC2ToMfx;

    mfxFrameAllocResponse response {};

    auto mfx_alloc = [&] (MfxFrameAllocator* allocator, MfxFramePoolAllocator*) {

        mfxFrameAllocRequest request {};
        request.Type = MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;
        request.NumFrameMin = FRAME_COUNT;
        request.NumFrameSuggested = FRAME_COUNT;
        InitFrameInfo(FOURCC, WIDTH, HEIGHT, &request.Info);

        mfxStatus sts = allocator->AllocFrames(&request, &response);
        EXPECT_EQ(sts, MFX_ERR_NONE);
        EXPECT_EQ(response.NumFrameActual, request.NumFrameMin);

        EXPECT_NE(response.mids, nullptr);
    };

    auto pool_alloc = [&] (MfxFrameAllocator* allocator, MfxFramePoolAllocator* pool_allocator) {
        for (int i = 0; i < FRAME_COUNT; ++i) {
            c2_blocks[i] = pool_allocator->Alloc();
            EXPECT_NE(c2_blocks[i], nullptr);

            const C2Handle* c2_handle = c2_blocks[i]->handle();
            mfxHDL mfx_handle {};
            mfxStatus sts = allocator->GetFrameHDL(response.mids[i], &mfx_handle);
            EXPECT_EQ(sts, MFX_ERR_NONE);
            handleC2ToMfx[c2_handle] = mfx_handle;
        }
        EXPECT_EQ(handleC2ToMfx.size(), FRAME_COUNT);
    };

    auto pool_free = [&] (MfxFrameAllocator*, MfxFramePoolAllocator*) {
        for (int i = 0; i < FRAME_COUNT; ++i) {
            c2_blocks[i].reset();
        }
    };

    auto pool_reset = [&] (MfxFrameAllocator*, MfxFramePoolAllocator* pool_allocator) {
        pool_allocator->Reset();
    };

    auto alloc_retains_handles = [&] (MfxFrameAllocator* allocator, MfxFramePoolAllocator* pool_allocator) {
        for (int i = 0; i < FRAME_COUNT; ++i) {
            c2_blocks[i] = pool_allocator->Alloc();
            EXPECT_NE(c2_blocks[i], nullptr);

            const C2Handle* c2_handle = c2_blocks[i]->handle();
            mfxHDL mfx_handle {};
            mfxStatus sts = allocator->GetFrameHDL(response.mids[i], &mfx_handle);
            EXPECT_EQ(sts, MFX_ERR_NONE);

            EXPECT_EQ(handleC2ToMfx[c2_handle], mfx_handle);
        }
    };

    auto alloc_another_handles = [&] (MfxFrameAllocator*, MfxFramePoolAllocator* pool_allocator) {
        std::shared_ptr<C2GraphicBlock> c2_blocks_2[FRAME_COUNT];
        for (int i = 0; i < FRAME_COUNT; ++i) {
            c2_blocks_2[i] = pool_allocator->Alloc();
            EXPECT_NE(c2_blocks_2[i], nullptr);

            const C2Handle* c2_handle = c2_blocks_2[i]->handle();
            EXPECT_EQ(handleC2ToMfx.find(c2_handle), handleC2ToMfx.end());
        }
    };

    MfxFramePoolAllocatorTest( { mfx_alloc, pool_alloc, pool_free, alloc_retains_handles,
        pool_reset, mfx_alloc, alloc_another_handles } );
}

#endif
