/*
 * Copyright (c) 2012-2022 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/RootDomain.h>

#include <IOKit/stream/IOStream.h>
#include <IOKit/stream/IOStreamShared.h>

#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSet.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSDebug.h>

#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/kauth.h>

#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/host.h>

#include <machine/machine_routines.h>

/*==============================================================================
 * AppleH264bpd - H.264 Bitstream Parser and Decoder Driver
 * 
 * This driver provides hardware-accelerated H.264 bitstream parsing and
 * decoding capabilities for Apple's graphics hardware.
 *==============================================================================*/

/*==============================================================================
 * Constants and Definitions
 *==============================================================================*/

#define APPLE_H264BPD_VERSION           0x00030001  /* Version 3.1 */
#define APPLE_H264BPD_REVISION          0x00000002

/* Maximum supported resolution (4K) */
#define H264_MAX_WIDTH                  4096
#define H264_MAX_HEIGHT                 2304
#define H264_MAX_MB_PER_FRAME           8192        /* Max macroblocks per frame */

/* H.264 NAL Unit Types */
enum {
    H264_NAL_UNSPECIFIED        = 0,
    H264_NAL_SLICE_NON_IDR      = 1,
    H264_NAL_SLICE_A            = 2,
    H264_NAL_SLICE_B            = 3,
    H264_NAL_SLICE_C            = 4,
    H264_NAL_SLICE_IDR          = 5,
    H264_NAL_SEI                = 6,
    H264_NAL_SPS                = 7,
    H264_NAL_PPS                = 8,
    H264_NAL_AUD                = 9,
    H264_NAL_END_OF_SEQUENCE    = 10,
    H264_NAL_END_OF_STREAM      = 11,
    H264_NAL_FILLER_DATA        = 12,
    H264_NAL_SPS_EXT            = 13,
    H264_NAL_PREFIX             = 14,
    H264_NAL_SUBSET_SPS         = 15,
    H264_NAL_DEPTH_SPS          = 16,
    H264_NAL_SLICE_AUX          = 19,
    H264_NAL_SLICE_EXT          = 20,
    H264_NAL_SLICE_EXT_DEPTH    = 21
};

/* H.264 Profile IDs */
enum {
    H264_PROFILE_BASELINE       = 66,
    H264_PROFILE_MAIN           = 77,
    H264_PROFILE_EXTENDED       = 88,
    H264_PROFILE_HIGH           = 100,
    H264_PROFILE_HIGH10         = 110,
    H264_PROFILE_HIGH422        = 122,
    H264_PROFILE_HIGH444        = 144,
    H264_PROFILE_CAVLC444       = 244
};

/* H.264 Level IDs */
enum {
    H264_LEVEL_1                = 10,
    H264_LEVEL_1b               = 9,
    H264_LEVEL_1_1              = 11,
    H264_LEVEL_1_2              = 12,
    H264_LEVEL_1_3              = 13,
    H264_LEVEL_2                = 20,
    H264_LEVEL_2_1              = 21,
    H264_LEVEL_2_2              = 22,
    H264_LEVEL_3                = 30,
    H264_LEVEL_3_1              = 31,
    H264_LEVEL_3_2              = 32,
    H264_LEVEL_4                = 40,
    H264_LEVEL_4_1              = 41,
    H264_LEVEL_4_2              = 42,
    H264_LEVEL_5                = 50,
    H264_LEVEL_5_1              = 51,
    H264_LEVEL_5_2              = 52
};

/* NAL Unit Start Codes */
#define H264_START_CODE_3         { 0x00, 0x00, 0x01 }
#define H264_START_CODE_4         { 0x00, 0x00, 0x00, 0x01 }

/* Hardware decoder states */
enum {
    H264_STATE_IDLE             = 0,
    H264_STATE_INITIALIZED      = 1,
    H264_STATE_CONFIGURED       = 2,
    H264_STATE_DECODING         = 3,
    H264_STATE_PAUSED           = 4,
    H264_STATE_ERROR            = 5,
    H264_STATE_RESET            = 6
};

/* Error codes */
#define H264_SUCCESS              0
#define H264_ERR_GENERAL          -1
#define H264_ERR_NO_MEMORY        -2
#define H264_ERR_INVALID_PARAM    -3
#define H264_ERR_NOT_READY        -4
#define H264_ERR_BUSY             -5
#define H264_ERR_TIMEOUT          -6
#define H264_ERR_UNSUPPORTED      -7
#define H264_ERR_MALFORMED        -8
#define H264_ERR_HARDWARE         -9
#define H264_ERR_BUFFER_OVERFLOW  -10
#define H264_ERR_BUFFER_UNDERFLOW -11
#define H264_ERR_RESOLUTION       -12
#define H264_ERR_PROFILE          -13
#define H264_ERR_LEVEL            -14
#define H264_ERR_REFERENCE        -15
#define H264_ERR_SLICE            -16

/* Buffer flags */
#define H264_BUF_FLAG_FRAME       (1 << 0)
#define H264_BUF_FLAG_FIELD       (1 << 1)
#define H264_BUF_FLAG_TOP_FIELD   (1 << 2)
#define H264_BUF_FLAG_BOTTOM_FIELD (1 << 3)
#define H264_BUF_FLAG_IDR          (1 << 4)
#define H264_BUF_FLAG_REFERENCE    (1 << 5)
#define H264_BUF_FLAG_CORRUPT      (1 << 6)
#define H264_BUF_FLAG_EOS          (1 << 7)

/* Maximum number of reference frames */
#define H264_MAX_REF_FRAMES        16
#define H264_MAX_DPB_SIZE          16

/* Slice types */
enum {
    H264_SLICE_P                 = 0,
    H264_SLICE_B                 = 1,
    H264_SLICE_I                 = 2,
    H264_SLICE_SP                = 3,
    H264_SLICE_SI                = 4
};

/* Chroma formats */
enum {
    H264_CHROMA_MONO             = 0,
    H264_CHROMA_420              = 1,
    H264_CHROMA_422              = 2,
    H264_CHROMA_444              = 3
};

/* Entropy coding modes */
enum {
    H264_ENTROPY_CAVLC           = 0,
    H264_ENTROPY_CABAC           = 1
};

/*==============================================================================
 * Data Structures
 *==============================================================================*/

/* NAL Unit Header */
typedef struct {
    uint8_t     forbidden_zero_bit;        /* Should be 0 */
    uint8_t     nal_ref_idc;               /* NAL reference IDC (0-3) */
    uint8_t     nal_unit_type;              /* NAL unit type */
} __attribute__((packed)) h264_nal_header_t;

/* Sequence Parameter Set (SPS) */
typedef struct {
    uint8_t     profile_idc;
    uint8_t     constraint_set0_flag : 1;
    uint8_t     constraint_set1_flag : 1;
    uint8_t     constraint_set2_flag : 1;
    uint8_t     constraint_set3_flag : 1;
    uint8_t     constraint_set4_flag : 1;
    uint8_t     constraint_set5_flag : 1;
    uint8_t     reserved_zero_2bits  : 2;
    uint8_t     level_idc;
    uint32_t    seq_parameter_set_id;
    
    /* Chroma */
    uint32_t    chroma_format_idc;
    uint8_t     separate_colour_plane_flag;
    uint32_t    bit_depth_luma_minus8;
    uint32_t    bit_depth_chroma_minus8;
    uint8_t     qpprime_y_zero_transform_bypass_flag;
    
    /* Picture size */
    uint32_t    pic_width_in_mbs_minus1;
    uint32_t    pic_height_in_map_units_minus1;
    uint8_t     frame_mbs_only_flag;
    uint8_t     mb_adaptive_frame_field_flag;
    uint8_t     direct_8x8_inference_flag;
    
    /* Crop */
    uint8_t     frame_cropping_flag;
    uint32_t    frame_crop_left_offset;
    uint32_t    frame_crop_right_offset;
    uint32_t    frame_crop_top_offset;
    uint32_t    frame_crop_bottom_offset;
    
    /* VUI parameters */
    uint8_t     vui_parameters_present_flag;
    
    /* Calculated values */
    uint32_t    width;
    uint32_t    height;
    uint32_t    mb_width;
    uint32_t    mb_height;
    uint32_t    sar_width;
    uint32_t    sar_height;
    uint8_t     timing_info_present_flag;
    uint32_t    num_units_in_tick;
    uint32_t    time_scale;
    uint8_t     fixed_frame_rate_flag;
    
    uint32_t    reserved[4];
} h264_sps_t;

/* Picture Parameter Set (PPS) */
typedef struct {
    uint32_t    pic_parameter_set_id;
    uint32_t    seq_parameter_set_id;
    uint8_t     entropy_coding_mode_flag;
    uint8_t     bottom_field_pic_order_in_frame_present_flag;
    uint32_t    num_slice_groups_minus1;
    uint32_t    slice_group_map_type;
    uint32_t    num_ref_idx_l0_default_active_minus1;
    uint32_t    num_ref_idx_l1_default_active_minus1;
    uint8_t     weighted_pred_flag;
    uint8_t     weighted_bipred_idc;
    int32_t     pic_init_qp_minus26;
    int32_t     pic_init_qs_minus26;
    int32_t     chroma_qp_index_offset;
    uint8_t     deblocking_filter_control_present_flag;
    uint8_t     constrained_intra_pred_flag;
    uint8_t     redundant_pic_cnt_present_flag;
    uint8_t     transform_8x8_mode_flag;
    uint8_t     pic_scaling_matrix_present_flag;
    int32_t     second_chroma_qp_index_offset;
    
    uint32_t    reserved[4];
} h264_pps_t;

/* Slice Header */
typedef struct {
    uint32_t    first_mb_in_slice;
    uint32_t    slice_type;
    uint32_t    pic_parameter_set_id;
    uint32_t    frame_num;
    uint8_t     field_pic_flag;
    uint8_t     bottom_field_flag;
    uint32_t    idr_pic_id;
    uint32_t    pic_order_cnt_lsb;
    int32_t     delta_pic_order_cnt_bottom;
    int32_t     delta_pic_order_cnt[2];
    uint8_t     redundant_pic_cnt;
    uint8_t     direct_spatial_mv_pred_flag;
    uint8_t     num_ref_idx_active_override_flag;
    uint32_t    num_ref_idx_l0_active_minus1;
    uint32_t    num_ref_idx_l1_active_minus1;
    uint8_t     cabac_init_idc;
    int32_t     slice_qp_delta;
    int32_t     slice_qs_delta;
    uint32_t    disable_deblocking_filter_idc;
    int32_t     slice_alpha_c0_offset_div2;
    int32_t     slice_beta_offset_div2;
    uint32_t    slice_group_change_cycle;
    
    uint32_t    reserved[4];
} h264_slice_header_t;

/* Decoded Picture Buffer Entry */
typedef struct {
    uint64_t    frame_id;
    uint32_t    pic_order_cnt;
    uint32_t    frame_num;
    uint8_t     is_reference;
    uint8_t     is_long_term;
    uint8_t     is_idr;
    uint8_t     field;
    uint32_t    width;
    uint32_t    height;
    uint64_t    surface_id;
    IOMemoryDescriptor* surface_mem;
    uint64_t    timestamp;
    uint32_t    flags;
    uint8_t*     data;
    uint32_t    data_size;
} h264_dpb_entry_t;

/* Decoder Context */
typedef struct {
    uint32_t    context_id;
    uint32_t    state;
    
    /* SPS/PPS */
    h264_sps_t  active_sps;
    h264_pps_t  active_pps;
    uint8_t     sps_valid;
    uint8_t     pps_valid;
    
    /* Picture parameters */
    uint32_t    width;
    uint32_t    height;
    uint32_t    chroma_format;
    uint32_t    bit_depth_luma;
    uint32_t    bit_depth_chroma;
    uint32_t    profile;
    uint32_t    level;
    uint32_t    entropy_mode;
    
    /* Decoding state */
    uint32_t    frame_num;
    uint32_t    pic_order_cnt;
    uint32_t    prev_frame_num;
    int32_t     prev_pic_order_cnt_msb;
    int32_t     prev_pic_order_cnt_lsb;
    uint8_t     memory_management_control_operation;
    
    /* Reference picture lists */
    h264_dpb_entry_t dpb[H264_MAX_DPB_SIZE];
    uint32_t    dpb_size;
    uint32_t    dpb_count;
    
    /* Active references */
    h264_dpb_entry_t* ref_list0[H264_MAX_REF_FRAMES];
    h264_dpb_entry_t* ref_list1[H264_MAX_REF_FRAMES];
    uint32_t    num_ref_idx_l0_active;
    uint32_t    num_ref_idx_l1_active;
    
    /* Output queue */
    h264_dpb_entry_t* output_queue[H264_MAX_DPB_SIZE];
    uint32_t    output_count;
    
    /* Statistics */
    uint64_t    frames_decoded;
    uint64_t    idr_frames;
    uint64_t    p_frames;
    uint64_t    b_frames;
    uint64_t    errors;
    uint64_t    bytes_processed;
    
    /* Hardware specific */
    void*       hw_context;
    uint64_t    hw_handle;
} h264_decoder_ctx_t;

/* Bitstream Buffer */
typedef struct {
    uint8_t*    data;
    uint32_t    size;
    uint32_t    capacity;
    uint32_t    pos;
    uint32_t    end;
    uint64_t    pts;
    uint64_t    dts;
    uint32_t    flags;
} h264_bitstream_buf_t;

/* Frame Output */
typedef struct {
    uint64_t    frame_id;
    uint32_t    width;
    uint32_t    height;
    uint32_t    chroma_format;
    uint32_t    bit_depth;
    uint64_t    surface_id;
    uint64_t    timestamp;
    uint32_t    flags;
    uint8_t     is_idr;
    uint8_t     is_reference;
    uint8_t     is_corrupt;
    uint32_t    data_size;
    uint8_t*    data[3];           /* Y, Cb, Cr planes */
    uint32_t    strides[3];        /* Strides for each plane */
} h264_frame_output_t;

/*==============================================================================
 * AppleH264bpd Main Class
 *==============================================================================*/

class AppleH264bpd : public IOService
{
    OSDeclareDefaultStructors(AppleH264bpd)
    
private:
    /* IOKit resources */
    IOWorkLoop*                 fWorkLoop;
    IOCommandGate*              fCommandGate;
    IOTimerEventSource*         fTimerSource;
    
    /* Version information */
    uint32_t                    fDriverVersion;
    uint32_t                    fDriverRevision;
    char                        fDriverBuild[32];
    
    /* Hardware capabilities */
    uint32_t                    fMaxWidth;
    uint32_t                    fMaxHeight;
    uint32_t                    fMaxMBPerFrame;
    uint32_t                    fMaxBitrate;
    uint32_t                    fMaxDPBSize;
    uint32_t                    fMaxRefFrames;
    uint32_t                    fSupportsProfiles[16];
    uint32_t                    fNumProfiles;
    uint32_t                    fSupportsLevels[16];
    uint32_t                    fNumLevels;
    uint8_t                     fSupports444;
    uint8_t                     fSupports422;
    uint8_t                     fSupports420;
    uint8_t                     fSupportsMBAFF;
    uint8_t                     fSupportsPAFF;
    uint8_t                     fSupportsCABAC;
    uint8_t                     fSupports8x8Transform;
    
    /* Decoder contexts */
    h264_decoder_ctx_t*         fContexts[8];
    uint32_t                    fNumContexts;
    lck_mtx_t*                  fContextLock;
    
    /* Bitstream buffers */
    h264_bitstream_buf_t*       fBitstreamBuffers[32];
    uint32_t                    fNumBitstreamBuffers;
    lck_mtx_t*                  fBufferLock;
    
    /* Output queue */
    h264_frame_output_t*        fOutputQueue[32];
    uint32_t                    fOutputHead;
    uint32_t                    fOutputTail;
    uint32_t                    fOutputCount;
    lck_mtx_t*                  fOutputLock;
    
    /* Hardware interface */
    void*                       fHardwareBase;
    uint64_t                    fHardwarePhys;
    uint64_t                    fHardwareSize;
    uint32_t                    fHardwareRevision;
    uint32_t                    fHardwareFeatures;
    
    /* Statistics */
    uint64_t                    fTotalFrames;
    uint64_t                    fTotalIDR;
    uint64_t                    fTotalP;
    uint64_t                    fTotalB;
    uint64_t                    fTotalBytes;
    uint64_t                    fTotalErrors;
    uint64_t                    fTotalTimeouts;
    
    /* Lock group */
    lck_grp_t*                  fLockGroup;
    lck_attr_t*                 fLockAttr;
    
    /* Power management */
    uint32_t                    fPowerState;
    uint64_t                    fLastActive;
    
    /*==============================================================================
     * Private Methods
     *==============================================================================*/
    
    bool                        initializeHardware(void);
    void                        shutdownHardware(void);
    
    bool                        createWorkLoop(void);
    void                        destroyWorkLoop(void);
    
    void                        timerFired(void);
    static void                 timerFired(OSObject* owner, IOTimerEventSource* sender);
    
    IOReturn                    handleCommand(void* arg0, void* arg1, void* arg2, void* arg3);
    
    /* NAL Unit parsing */
    int                         findNextNALUnit(const uint8_t* data, uint32_t size, 
                                                uint32_t* start, uint32_t* end);
    int                         parseNALHeader(const uint8_t* data, uint32_t size,
                                               h264_nal_header_t* header);
    int                         parseSPS(const uint8_t* data, uint32_t size,
                                         h264_sps_t* sps);
    int                         parsePPS(const uint8_t* data, uint32_t size,
                                         h264_pps_t* pps);
    int                         parseSliceHeader(const uint8_t* data, uint32_t size,
                                                h264_slice_header_t* slice,
                                                h264_sps_t* sps, h264_pps_t* pps);
    
    /* Bitstream parsing utilities */
    uint32_t                    readUE(const uint8_t** buffer, uint32_t* bits_left);
    int32_t                     readSE(const uint8_t** buffer, uint32_t* bits_left);
    uint32_t                    readBits(const uint8_t** buffer, uint32_t* bits_left, uint32_t n);
    void                        skipBits(const uint8_t** buffer, uint32_t* bits_left, uint32_t n);
    uint8_t                     moreRBSPData(const uint8_t* buffer, uint32_t bits_left);
    uint32_t                    countLeadingZeros(const uint8_t* buffer, uint32_t bits_left);
    
    /* Decoder management */
    int                         createDecoderContext(uint32_t* context_id);
    int                         destroyDecoderContext(uint32_t context_id);
    h264_decoder_ctx_t*         findDecoderContext(uint32_t context_id);
    
    int                         initializeDecoderContext(h264_decoder_ctx_t* ctx);
    int                         resetDecoderContext(h264_decoder_ctx_t* ctx);
    
    /* Decoding */
    int                         decodeNALUnit(h264_decoder_ctx_t* ctx,
                                              const uint8_t* data, uint32_t size,
                                              uint64_t pts, uint64_t dts);
    int                         decodeSlice(h264_decoder_ctx_t* ctx,
                                           h264_slice_header_t* slice,
                                           const uint8_t* data, uint32_t size);
    
    /* Picture management */
    int                         referencePictureMarking(h264_decoder_ctx_t* ctx,
                                                        h264_slice_header_t* slice);
    int                         buildReferencePictureLists(h264_decoder_ctx_t* ctx,
                                                           h264_slice_header_t* slice);
    int                         outputPicture(h264_decoder_ctx_t* ctx,
                                             h264_dpb_entry_t* picture);
    
    /* DPB management */
    int                         dpbAddPicture(h264_decoder_ctx_t* ctx,
                                             h264_dpb_entry_t* picture);
    int                         dpbRemovePicture(h264_decoder_ctx_t* ctx,
                                                uint32_t index);
    h264_dpb_entry_t*           dpbFindPicture(h264_decoder_ctx_t* ctx,
                                               uint32_t frame_num, uint8_t field);
    int                         dpbSlideWindow(h264_decoder_ctx_t* ctx);
    int                         dpbBump(h264_decoder_ctx_t* ctx);
    
    /* Hardware acceleration */
    int                         submitToHardware(h264_decoder_ctx_t* ctx,
                                                 const uint8_t* data, uint32_t size,
                                                 h264_slice_header_t* slice);
    int                         waitForHardware(h264_decoder_ctx_t* ctx,
                                                uint32_t timeout_ms);
    
    /* Utility */
    uint64_t                    getCurrentTimestamp(void);
    uint32_t                    getProfileString(uint32_t profile, char* buf, uint32_t len);
    uint32_t                    getLevelString(uint32_t level, char* buf, uint32_t len);
    
protected:
    bool                        init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                        free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                        start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                        stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    /* Power management */
    IOReturn                    setPowerState(unsigned long powerState, IOService* device) APPLE_KEXT_OVERRIDE;
    
    /* User client access */
    IOReturn                    newUserClient(task_t owningTask,
                                              void* securityID,
                                              UInt32 type,
                                              OSDictionary* properties,
                                              IOUserClient** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleH264bpd, IOService)

/*==============================================================================
 * AppleH264bpdUserClient - User client for H.264 decoding
 *==============================================================================*/

class AppleH264bpdUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(AppleH264bpdUserClient)
    
private:
    task_t                      fTask;
    pid_t                       fPid;
    uint32_t                    fClientId;
    AppleH264bpd*               fProvider;
    uint32_t                    fState;
    
    IOMemoryDescriptor*         fInputBuffer;
    IOMemoryDescriptor*         fOutputBuffer;
    uint8_t*                    fInputMapping;
    uint8_t*                    fOutputMapping;
    uint32_t                    fInputSize;
    uint32_t                    fOutputSize;
    
    uint32_t                    fDecoderContextId;
    bool                        fValid;
    
    /* External method dispatch */
    static const IOExternalMethodDispatch sMethods[];
    
protected:
    bool                        init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                        free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                        start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                        stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    IOReturn                    clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn                    clientDied(void) APPLE_KEXT_OVERRIDE;
    
    IOReturn                    registerNotificationPort(mach_port_t port, UInt32 type,
                                                          UInt32 refCon) APPLE_KEXT_OVERRIDE;
    
    IOReturn                    externalMethod(uint32_t selector,
                                                IOExternalMethodArguments* arguments,
                                                IOExternalMethodDispatch* dispatch,
                                                OSObject* target,
                                                void* reference) APPLE_KEXT_OVERRIDE;
    
    /* External methods */
    static IOReturn             sCreateDecoder(AppleH264bpdUserClient* target,
                                               void* reference,
                                               IOExternalMethodArguments* args);
    
    static IOReturn             sDestroyDecoder(AppleH264bpdUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args);
    
    static IOReturn             sConfigureDecoder(AppleH264bpdUserClient* target,
                                                  void* reference,
                                                  IOExternalMethodArguments* args);
    
    static IOReturn             sDecodeNAL(AppleH264bpdUserClient* target,
                                           void* reference,
                                           IOExternalMethodArguments* args);
    
    static IOReturn             sDecodeFrame(AppleH264bpdUserClient* target,
                                             void* reference,
                                             IOExternalMethodArguments* args);
    
    static IOReturn             sGetOutput(AppleH264bpdUserClient* target,
                                           void* reference,
                                           IOExternalMethodArguments* args);
    
    static IOReturn             sFlushDecoder(AppleH264bpdUserClient* target,
                                              void* reference,
                                              IOExternalMethodArguments* args);
    
    static IOReturn             sResetDecoder(AppleH264bpdUserClient* target,
                                              void* reference,
                                              IOExternalMethodArguments* args);
    
    static IOReturn             sGetCapabilities(AppleH264bpdUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args);
    
    static IOReturn             sGetStatistics(AppleH264bpdUserClient* target,
                                               void* reference,
                                               IOExternalMethodArguments* args);
};

OSDefineMetaClassAndStructors(AppleH264bpdUserClient, IOUserClient)

/*==============================================================================
 * External Method Dispatch Table
 *==============================================================================*/

enum {
    kMethodCreateDecoder,
    kMethodDestroyDecoder,
    kMethodConfigureDecoder,
    kMethodDecodeNAL,
    kMethodDecodeFrame,
    kMethodGetOutput,
    kMethodFlushDecoder,
    kMethodResetDecoder,
    kMethodGetCapabilities,
    kMethodGetStatistics,
    kMethodCount
};

const IOExternalMethodDispatch AppleH264bpdUserClient::sMethods[kMethodCount] = {
    {   /* kMethodCreateDecoder */
        (IOExternalMethodAction)&AppleH264bpdUserClient::sCreateDecoder,
        0,                          /* Number of scalar inputs */
        0,                          /* Number of struct inputs */
        1,                          /* Number of scalar outputs */
        0                           /* Number of struct outputs */
    },
    {   /* kMethodDestroyDecoder */
        (IOExternalMethodAction)&AppleH264bpdUserClient::sDestroyDecoder,
        1, 0, 0, 0
    },
    {   /* kMethodConfigureDecoder */
        (IOExternalMethodAction)&AppleH264bpdUserClient::sConfigureDecoder,
        3, 0, 0, 0
    },
    {   /* kMethodDecodeNAL */
        (IOExternalMethodAction)&AppleH264bpdUserClient::sDecodeNAL,
        2, 1, 1, 0
    },
    {   /* kMethodDecodeFrame */
        (IOExternalMethodAction)&AppleH264bpdUserClient::sDecodeFrame,
        2, 1, 1, 0
    },
    {   /* kMethodGetOutput */
        (IOExternalMethodAction)&AppleH264bpdUserClient::sGetOutput,
        0, 0, 0, 1
    },
    {   /* kMethodFlushDecoder */
        (IOExternalMethodAction)&AppleH264bpdUserClient::sFlushDecoder,
        1, 0, 0, 0
    },
    {   /* kMethodResetDecoder */
        (IOExternalMethodAction)&AppleH264bpdUserClient::sResetDecoder,
        1, 0, 0, 0
    },
    {   /* kMethodGetCapabilities */
        (IOExternalMethodAction)&AppleH264bpdUserClient::sGetCapabilities,
        0, 0, 0, 1
    },
    {   /* kMethodGetStatistics */
        (IOExternalMethodAction)&AppleH264bpdUserClient::sGetStatistics,
        1, 0, 0, 1
    }
};

/*==============================================================================
 * Bitstream Parsing Utilities
 *==============================================================================*/

uint32_t AppleH264bpd::readBits(const uint8_t** buffer, uint32_t* bits_left, uint32_t n)
{
    uint32_t value = 0;
    uint32_t bits = n;
    const uint8_t* buf = *buffer;
    uint32_t left = *bits_left;
    
    while (bits > 0) {
        uint32_t byte_pos = (left - bits) >> 3;
        uint32_t bit_pos = 7 - (((left - bits) & 7));
        uint32_t bits_this_byte = bits;
        
        if (bits_this_byte > (bit_pos + 1)) {
            bits_this_byte = bit_pos + 1;
        }
        
        uint32_t byte_val = buf[byte_pos];
        uint32_t mask = (1 << bits_this_byte) - 1;
        
        value = (value << bits_this_byte) | 
                ((byte_val >> (bit_pos + 1 - bits_this_byte)) & mask);
        
        bits -= bits_this_byte;
    }
    
    *buffer = buf;
    *bits_left = left - n;
    
    return value;
}

void AppleH264bpd::skipBits(const uint8_t** buffer, uint32_t* bits_left, uint32_t n)
{
    *bits_left -= n;
    /* Note: buffer pointer not updated for simplicity */
}

uint32_t AppleH264bpd::readUE(const uint8_t** buffer, uint32_t* bits_left)
{
    uint32_t leading_zeros = 0;
    
    while (!readBits(buffer, bits_left, 1) && *bits_left > 0) {
        leading_zeros++;
    }
    
    if (leading_zeros >= 32) {
        return 0xFFFFFFFF;
    }
    
    return (1 << leading_zeros) - 1 + readBits(buffer, bits_left, leading_zeros);
}

int32_t AppleH264bpd::readSE(const uint8_t** buffer, uint32_t* bits_left)
{
    uint32_t ue = readUE(buffer, bits_left);
    
    if (ue == 0xFFFFFFFF) {
        return 0x80000000;
    }
    
    int32_t se = (ue + 1) >> 1;
    if (!(ue & 1)) {
        se = -se;
    }
    
    return se;
}

uint8_t AppleH264bpd::moreRBSPData(const uint8_t* buffer, uint32_t bits_left)
{
    /* Check if there's more data after rbsp_stop_one_bit */
    if (bits_left <= 1) {
        return 0;
    }
    
    /* Check for trailing bits */
    uint32_t byte_pos = (bits_left - 1) >> 3;
    uint8_t last_byte = buffer[byte_pos];
    uint8_t mask = 0xFF >> (7 - ((bits_left - 1) & 7));
    
    if ((last_byte & mask) != 0) {
        return 1;
    }
    
    return 0;
}

uint32_t AppleH264bpd::countLeadingZeros(const uint8_t* buffer, uint32_t bits_left)
{
    uint32_t zeros = 0;
    uint32_t byte_pos = 0;
    uint32_t bit_pos = 7;
    uint32_t bits = bits_left;
    
    while (bits > 0) {
        if ((buffer[byte_pos] >> bit_pos) & 1) {
            break;
        }
        zeros++;
        bits--;
        
        if (bit_pos == 0) {
            byte_pos++;
            bit_pos = 7;
        } else {
            bit_pos--;
        }
    }
    
    return zeros;
}

/*==============================================================================
 * NAL Unit Parsing
 *==============================================================================*/

int AppleH264bpd::findNextNALUnit(const uint8_t* data, uint32_t size,
                                   uint32_t* start, uint32_t* end)
{
    uint32_t i = 0;
    uint32_t nal_start = 0;
    uint32_t nal_end = size;
    
    /* Find start code */
    while (i < size - 3) {
        if (data[i] == 0 && data[i+1] == 0) {
            if (data[i+2] == 1) {
                /* 3-byte start code */
                nal_start = i + 3;
                i += 3;
                break;
            } else if (i < size - 4 && data[i+2] == 0 && data[i+3] == 1) {
                /* 4-byte start code */
                nal_start = i + 4;
                i += 4;
                break;
            }
        }
        i++;
    }
    
    if (nal_start == 0) {
        return H264_ERR_MALFORMED;
    }
    
    /* Find next start code */
    while (i < size - 3) {
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            nal_end = i;
            break;
        } else if (i < size - 4 && data[i] == 0 && data[i+1] == 0 && 
                   data[i+2] == 0 && data[i+3] == 1) {
            nal_end = i;
            break;
        }
        i++;
    }
    
    *start = nal_start;
    *end = nal_end;
    
    return H264_SUCCESS;
}

int AppleH264bpd::parseNALHeader(const uint8_t* data, uint32_t size,
                                  h264_nal_header_t* header)
{
    if (!data || !header || size < 1) {
        return H264_ERR_INVALID_PARAM;
    }
    
    uint8_t nal_byte = data[0];
    
    header->forbidden_zero_bit = (nal_byte >> 7) & 1;
    header->nal_ref_idc = (nal_byte >> 5) & 3;
    header->nal_unit_type = nal_byte & 0x1F;
    
    return H264_SUCCESS;
}

int AppleH264bpd::parseSPS(const uint8_t* data, uint32_t size, h264_sps_t* sps)
{
    const uint8_t* buf = data;
    uint32_t bits_left = size * 8;
    uint32_t i;
    
    if (!data || !sps || size < 3) {
        return H264_ERR_INVALID_PARAM;
    }
    
    /* Skip NAL header (already parsed) */
    skipBits(&buf, &bits_left, 8);
    
    sps->profile_idc = readBits(&buf, &bits_left, 8);
    
    /* constraint flags */
    uint8_t flags = readBits(&buf, &bits_left, 8);
    sps->constraint_set0_flag = (flags >> 7) & 1;
    sps->constraint_set1_flag = (flags >> 6) & 1;
    sps->constraint_set2_flag = (flags >> 5) & 1;
    sps->constraint_set3_flag = (flags >> 4) & 1;
    sps->constraint_set4_flag = (flags >> 3) & 1;
    sps->constraint_set5_flag = (flags >> 2) & 1;
    sps->reserved_zero_2bits = flags & 3;
    
    sps->level_idc = readBits(&buf, &bits_left, 8);
    
    sps->seq_parameter_set_id = readUE(&buf, &bits_left);
    
    if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
        sps->profile_idc == 122 || sps->profile_idc == 144 ||
        sps->profile_idc == 244) {
        
        sps->chroma_format_idc = readUE(&buf, &bits_left);
        
        if (sps->chroma_format_idc == 3) {
            sps->separate_colour_plane_flag = readBits(&buf, &bits_left, 1);
        }
        
        sps->bit_depth_luma_minus8 = readUE(&buf, &bits_left);
        sps->bit_depth_chroma_minus8 = readUE(&buf, &bits_left);
        
        sps->qpprime_y_zero_transform_bypass_flag = readBits(&buf, &bits_left, 1);
        
        uint8_t seq_scaling_matrix_present_flag = readBits(&buf, &bits_left, 1);
        if (seq_scaling_matrix_present_flag) {
            /* Skip scaling lists */
            uint32_t num_lists = (sps->chroma_format_idc != 3) ? 8 : 12;
            for (i = 0; i < num_lists; i++) {
                uint8_t present = readBits(&buf, &bits_left, 1);
                if (present) {
                    /* Skip scaling list (simplified) */
                    uint32_t list_size = (i < 6) ? 16 : 64;
                    for (uint32_t j = 0; j < list_size; j++) {
                        readSE(&buf, &bits_left);
                    }
                }
            }
        }
    } else {
        sps->chroma_format_idc = 1;  /* 4:2:0 default */
        sps->bit_depth_luma_minus8 = 0;
        sps->bit_depth_chroma_minus8 = 0;
    }
    
    sps->pic_width_in_mbs_minus1 = readUE(&buf, &bits_left);
    sps->pic_height_in_map_units_minus1 = readUE(&buf, &bits_left);
    sps->frame_mbs_only_flag = readBits(&buf, &bits_left, 1);
    
    if (!sps->frame_mbs_only_flag) {
        sps->mb_adaptive_frame_field_flag = readBits(&buf, &bits_left, 1);
    }
    
    sps->direct_8x8_inference_flag = readBits(&buf, &bits_left, 1);
    
    sps->frame_cropping_flag = readBits(&buf, &bits_left, 1);
    if (sps->frame_cropping_flag) {
        sps->frame_crop_left_offset = readUE(&buf, &bits_left);
        sps->frame_crop_right_offset = readUE(&buf, &bits_left);
        sps->frame_crop_top_offset = readUE(&buf, &bits_left);
        sps->frame_crop_bottom_offset = readUE(&buf, &bits_left);
    }
    
    sps->vui_parameters_present_flag = readBits(&buf, &bits_left, 1);
    if (sps->vui_parameters_present_flag) {
        /* Parse VUI (simplified) */
        uint8_t aspect_ratio_info_present_flag = readBits(&buf, &bits_left, 1);
        if (aspect_ratio_info_present_flag) {
            uint8_t aspect_ratio_idc = readBits(&buf, &bits_left, 8);
            /* Map aspect_ratio_idc to SAR */
            if (aspect_ratio_idc == 0xFF) {
                sps->sar_width = readBits(&buf, &bits_left, 16);
                sps->sar_height = readBits(&buf, &bits_left, 16);
            } else {
                /* Use standard SAR values */
                sps->sar_width = 1;
                sps->sar_height = 1;
            }
        }
        
        sps->timing_info_present_flag = readBits(&buf, &bits_left, 1);
        if (sps->timing_info_present_flag) {
            sps->num_units_in_tick = readBits(&buf, &bits_left, 32);
            sps->time_scale = readBits(&buf, &bits_left, 32);
            sps->fixed_frame_rate_flag = readBits(&buf, &bits_left, 1);
        }
        
        /* Skip remaining VUI for simplicity */
    }
    
    /* Calculate actual dimensions */
    sps->mb_width = sps->pic_width_in_mbs_minus1 + 1;
    sps->mb_height = (2 - sps->frame_mbs_only_flag) * 
                     (sps->pic_height_in_map_units_minus1 + 1);
    
    sps->width = sps->mb_width * 16;
    sps->height = sps->mb_height * 16;
    
    /* Apply cropping */
    if (sps->frame_cropping_flag) {
        uint32_t crop_unit_x = (sps->chroma_format_idc == 3) ? 1 : 2;
        uint32_t crop_unit_y = (2 - sps->frame_mbs_only_flag) * 
                               ((sps->chroma_format_idc == 3) ? 1 : 2);
        
        sps->width -= (sps->frame_crop_left_offset + sps->frame_crop_right_offset) * crop_unit_x;
        sps->height -= (sps->frame_crop_top_offset + sps->frame_crop_bottom_offset) * crop_unit_y;
    }
    
    return H264_SUCCESS;
}

int AppleH264bpd::parsePPS(const uint8_t* data, uint32_t size, h264_pps_t* pps)
{
    const uint8_t* buf = data;
    uint32_t bits_left = size * 8;
    
    if (!data || !pps || size < 1) {
        return H264_ERR_INVALID_PARAM;
    }
    
    /* Skip NAL header */
    skipBits(&buf, &bits_left, 8);
    
    pps->pic_parameter_set_id = readUE(&buf, &bits_left);
    pps->seq_parameter_set_id = readUE(&buf, &bits_left);
    pps->entropy_coding_mode_flag = readBits(&buf, &bits_left, 1);
    pps->bottom_field_pic_order_in_frame_present_flag = readBits(&buf, &bits_left, 1);
    pps->num_slice_groups_minus1 = readUE(&buf, &bits_left);
    
    if (pps->num_slice_groups_minus1 > 0) {
        pps->slice_group_map_type = readUE(&buf, &bits_left);
        
        if (pps->slice_group_map_type >= 3 && pps->slice_group_map_type <= 5) {
            /* Skip slice group mapping for simplicity */
        }
    }
    
    pps->num_ref_idx_l0_default_active_minus1 = readUE(&buf, &bits_left);
    pps->num_ref_idx_l1_default_active_minus1 = readUE(&buf, &bits_left);
    pps->weighted_pred_flag = readBits(&buf, &bits_left, 1);
    pps->weighted_bipred_idc = readBits(&buf, &bits_left, 2);
    pps->pic_init_qp_minus26 = readSE(&buf, &bits_left);
    pps->pic_init_qs_minus26 = readSE(&buf, &bits_left);
    pps->chroma_qp_index_offset = readSE(&buf, &bits_left);
    pps->deblocking_filter_control_present_flag = readBits(&buf, &bits_left, 1);
    pps->constrained_intra_pred_flag = readBits(&buf, &bits_left, 1);
    pps->redundant_pic_cnt_present_flag = readBits(&buf, &bits_left, 1);
    
    if (moreRBSPData(buf, bits_left)) {
        pps->transform_8x8_mode_flag = readBits(&buf, &bits_left, 1);
        pps->pic_scaling_matrix_present_flag = readBits(&buf, &bits_left, 1);
        
        if (pps->pic_scaling_matrix_present_flag) {
            /* Skip scaling lists */
        }
        
        pps->second_chroma_qp_index_offset = readSE(&buf, &bits_left);
    } else {
        pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
    }
    
    return H264_SUCCESS;
}

int AppleH264bpd::parseSliceHeader(const uint8_t* data, uint32_t size,
                                    h264_slice_header_t* slice,
                                    h264_sps_t* sps, h264_pps_t* pps)
{
    const uint8_t* buf = data;
    uint32_t bits_left = size * 8;
    uint32_t i;
    
    if (!data || !slice || !sps || !pps) {
        return H264_ERR_INVALID_PARAM;
    }
    
    /* Skip NAL header */
    skipBits(&buf, &bits_left, 8);
    
    slice->first_mb_in_slice = readUE(&buf, &bits_left);
    slice->slice_type = readUE(&buf, &bits_left);
    slice->pic_parameter_set_id = readUE(&buf, &bits_left);
    
    slice->frame_num = readBits(&buf, &bits_left, 
                                 (sps->frame_mbs_only_flag ? 4 : 5));
    
    if (!sps->frame_mbs_only_flag) {
        slice->field_pic_flag = readBits(&buf, &bits_left, 1);
        if (slice->field_pic_flag) {
            slice->bottom_field_flag = readBits(&buf, &bits_left, 1);
        }
    }
    
    if (slice->slice_type == H264_SLICE_I || slice->slice_type == H264_SLICE_SI) {
        slice->idr_pic_id = readUE(&buf, &bits_left);
    }
    
    if (sps->pic_order_cnt_type == 0) {
        slice->pic_order_cnt_lsb = readBits(&buf, &bits_left,
                                             sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
        if (pps->bottom_field_pic_order_in_frame_present_flag && !slice->field_pic_flag) {
            slice->delta_pic_order_cnt_bottom = readSE(&buf, &bits_left);
        }
    }
    
    if (sps->pic_order_cnt_type == 1 && !sps->delta_pic_order_always_zero_flag) {
        slice->delta_pic_order_cnt[0] = readSE(&buf, &bits_left);
        if (pps->bottom_field_pic_order_in_frame_present_flag && !slice->field_pic_flag) {
            slice->delta_pic_order_cnt[1] = readSE(&buf, &bits_left);
        }
    }
    
    if (pps->redundant_pic_cnt_present_flag) {
        slice->redundant_pic_cnt = readUE(&buf, &bits_left);
    }
    
    if (slice->slice_type == H264_SLICE_B) {
        slice->direct_spatial_mv_pred_flag = readBits(&buf, &bits_left, 1);
    }
    
    if (slice->slice_type == H264_SLICE_P || slice->slice_type == H264_SLICE_SP ||
        slice->slice_type == H264_SLICE_B) {
        
        slice->num_ref_idx_active_override_flag = readBits(&buf, &bits_left, 1);
        
        if (slice->num_ref_idx_active_override_flag) {
            slice->num_ref_idx_l0_active_minus1 = readUE(&buf, &bits_left);
            if (slice->slice_type == H264_SLICE_B) {
                slice->num_ref_idx_l1_active_minus1 = readUE(&buf, &bits_left);
            }
        } else {
            slice->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
            slice->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;
        }
    }
    
    if (pps->entropy_coding_mode_flag && slice->slice_type != H264_SLICE_I &&
        slice->slice_type != H264_SLICE_SI) {
        slice->cabac_init_idc = readUE(&buf, &bits_left);
    }
    
    slice->slice_qp_delta = readSE(&buf, &bits_left);
    
    if (slice->slice_type == H264_SLICE_SP || slice->slice_type == H264_SLICE_SI) {
        if (slice->slice_type == H264_SLICE_SP) {
            slice->slice_qs_delta = readSE(&buf, &bits_left);
        }
    }
    
    if (pps->deblocking_filter_control_present_flag) {
        slice->disable_deblocking_filter_idc = readUE(&buf, &bits_left);
        
        if (slice->disable_deblocking_filter_idc != 1) {
            slice->slice_alpha_c0_offset_div2 = readSE(&buf, &bits_left);
            slice->slice_beta_offset_div2 = readSE(&buf, &bits_left);
        }
    }
    
    if (pps->num_slice_groups_minus1 > 0 && 
        pps->slice_group_map_type >= 3 && 
        pps->slice_group_map_type <= 5) {
        slice->slice_group_change_cycle = readBits(&buf, &bits_left,
                                                    (sps->pic_width_in_mbs_minus1 + 1) *
                                                    (sps->pic_height_in_map_units_minus1 + 1));
    }
    
    return H264_SUCCESS;
}

/*==============================================================================
 * Decoder Context Management
 *==============================================================================*/

int AppleH264bpd::createDecoderContext(uint32_t* context_id)
{
    h264_decoder_ctx_t* ctx;
    static uint32_t next_id = 1;
    int ret = H264_ERR_GENERAL;
    
    lck_mtx_lock(fContextLock);
    
    /* Find free slot */
    for (int i = 0; i < 8; i++) {
        if (fContexts[i] == NULL) {
            ctx = (h264_decoder_ctx_t*)IOMalloc(sizeof(h264_decoder_ctx_t));
            if (!ctx) {
                ret = H264_ERR_NO_MEMORY;
                goto exit;
            }
            
            bzero(ctx, sizeof(h264_decoder_ctx_t));
            
            ctx->context_id = next_id++;
            ctx->state = H264_STATE_IDLE;
            
            fContexts[i] = ctx;
            fNumContexts++;
            *context_id = ctx->context_id;
            
            ret = H264_SUCCESS;
            break;
        }
    }
    
exit:
    lck_mtx_unlock(fContextLock);
    return ret;
}

int AppleH264bpd::destroyDecoderContext(uint32_t context_id)
{
    int ret = H264_ERR_INVALID_PARAM;
    
    lck_mtx_lock(fContextLock);
    
    for (int i = 0; i < 8; i++) {
        if (fContexts[i] && fContexts[i]->context_id == context_id) {
            /* Free DPB entries */
            for (int j = 0; j < fContexts[i]->dpb_count; j++) {
                if (fContexts[i]->dpb[j].surface_mem) {
                    fContexts[i]->dpb[j].surface_mem->release();
                }
                if (fContexts[i]->dpb[j].data) {
                    IOFree(fContexts[i]->dpb[j].data, fContexts[i]->dpb[j].data_size);
                }
            }
            
            IOFree(fContexts[i], sizeof(h264_decoder_ctx_t));
            fContexts[i] = NULL;
            fNumContexts--;
            ret = H264_SUCCESS;
            break;
        }
    }
    
    lck_mtx_unlock(fContextLock);
    return ret;
}

h264_decoder_ctx_t* AppleH264bpd::findDecoderContext(uint32_t context_id)
{
    h264_decoder_ctx_t* ctx = NULL;
    
    lck_mtx_lock(fContextLock);
    
    for (int i = 0; i < 8; i++) {
        if (fContexts[i] && fContexts[i]->context_id == context_id) {
            ctx = fContexts[i];
            break;
        }
    }
    
    lck_mtx_unlock(fContextLock);
    return ctx;
}

int AppleH264bpd::initializeDecoderContext(h264_decoder_ctx_t* ctx)
{
    if (!ctx) {
        return H264_ERR_INVALID_PARAM;
    }
    
    ctx->state = H264_STATE_INITIALIZED;
    ctx->dpb_size = fMaxDPBSize;
    ctx->num_ref_idx_l0_active = 0;
    ctx->num_ref_idx_l1_active = 0;
    ctx->frame_num = 0;
    ctx->pic_order_cnt = 0;
    ctx->prev_frame_num = 0;
    ctx->prev_pic_order_cnt_msb = 0;
    ctx->prev_pic_order_cnt_lsb = 0;
    
    return H264_SUCCESS;
}

int AppleH264bpd::resetDecoderContext(h264_decoder_ctx_t* ctx)
{
    if (!ctx) {
        return H264_ERR_INVALID_PARAM;
    }
    
    ctx->state = H264_STATE_RESET;
    
    /* Clear DPB */
    for (int i = 0; i < ctx->dpb_count; i++) {
        if (ctx->dpb[i].surface_mem) {
            ctx->dpb[i].surface_mem->release();
            ctx->dpb[i].surface_mem = NULL;
        }
        if (ctx->dpb[i].data) {
            IOFree(ctx->dpb[i].data, ctx->dpb[i].data_size);
            ctx->dpb[i].data = NULL;
        }
    }
    
    ctx->dpb_count = 0;
    ctx->output_count = 0;
    ctx->frame_num = 0;
    ctx->pic_order_cnt = 0;
    ctx->prev_frame_num = 0;
    ctx->prev_pic_order_cnt_msb = 0;
    ctx->prev_pic_order_cnt_lsb = 0;
    
    ctx->state = H264_STATE_INITIALIZED;
    
    return H264_SUCCESS;
}

/*==============================================================================
 * Reference Picture Management
 *==============================================================================*/

int AppleH264bpd::referencePictureMarking(h264_decoder_ctx_t* ctx,
                                          h264_slice_header_t* slice)
{
    /* Simplified reference picture marking */
    /* In a real implementation, this would handle sliding window and MMCO */
    
    return H264_SUCCESS;
}

int AppleH264bpd::buildReferencePictureLists(h264_decoder_ctx_t* ctx,
                                              h264_slice_header_t* slice)
{
    /* Build reference picture list 0 */
    uint32_t num_ref = slice->num_ref_idx_l0_active_minus1 + 1;
    uint32_t count = 0;
    
    for (int i = 0; i < ctx->dpb_count && count < num_ref; i++) {
        if (ctx->dpb[i].is_reference) {
            ctx->ref_list0[count++] = &ctx->dpb[i];
        }
    }
    
    ctx->num_ref_idx_l0_active = count;
    
    /* Build reference picture list 1 if B slice */
    if (slice->slice_type == H264_SLICE_B) {
        count = 0;
        num_ref = slice->num_ref_idx_l1_active_minus1 + 1;
        
        /* Simplified list 1 construction */
        for (int i = ctx->dpb_count - 1; i >= 0 && count < num_ref; i--) {
            if (ctx->dpb[i].is_reference && &ctx->dpb[i] != ctx->ref_list0[0]) {
                ctx->ref_list1[count++] = &ctx->dpb[i];
            }
        }
        
        ctx->num_ref_idx_l1_active = count;
    }
    
    return H264_SUCCESS;
}

int AppleH264bpd::dpbAddPicture(h264_decoder_ctx_t* ctx, h264_dpb_entry_t* picture)
{
    if (ctx->dpb_count >= ctx->dpb_size) {
        /* Need to bump a picture */
        dpbBump(ctx);
    }
    
    ctx->dpb[ctx->dpb_count] = *picture;
    ctx->dpb_count++;
    
    return H264_SUCCESS;
}

int AppleH264bpd::dpbRemovePicture(h264_decoder_ctx_t* ctx, uint32_t index)
{
    if (index >= ctx->dpb_count) {
        return H264_ERR_INVALID_PARAM;
    }
    
    /* Free resources */
    if (ctx->dpb[index].surface_mem) {
        ctx->dpb[index].surface_mem->release();
        ctx->dpb[index].surface_mem = NULL;
    }
    
    if (ctx->dpb[index].data) {
        IOFree(ctx->dpb[index].data, ctx->dpb[index].data_size);
        ctx->dpb[index].data = NULL;
    }
    
    /* Shift remaining entries */
    for (uint32_t i = index; i < ctx->dpb_count - 1; i++) {
        ctx->dpb[i] = ctx->dpb[i + 1];
    }
    
    ctx->dpb_count--;
    
    return H264_SUCCESS;
}

h264_dpb_entry_t* AppleH264bpd::dpbFindPicture(h264_decoder_ctx_t* ctx,
                                                uint32_t frame_num, uint8_t field)
{
    for (uint32_t i = 0; i < ctx->dpb_count; i++) {
        if (ctx->dpb[i].frame_num == frame_num) {
            if (field == 0 || ctx->dpb[i].field == field) {
                return &ctx->dpb[i];
            }
        }
    }
    
    return NULL;
}

int AppleH264bpd::dpbSlideWindow(h264_decoder_ctx_t* ctx)
{
    /* Implement sliding window reference picture marking */
    if (ctx->dpb_count > 0) {
        /* Mark the oldest reference picture as unused */
        for (uint32_t i = 0; i < ctx->dpb_count; i++) {
            if (ctx->dpb[i].is_reference) {
                ctx->dpb[i].is_reference = 0;
                break;
            }
        }
    }
    
    return H264_SUCCESS;
}

int AppleH264bpd::dpbBump(h264_decoder_ctx_t* ctx)
{
    /* Output the oldest picture and remove from DPB */
    if (ctx->dpb_count == 0) {
        return H264_SUCCESS;
    }
    
    /* Find oldest picture */
    uint32_t oldest_idx = 0;
    uint32_t oldest_poc = ctx->dpb[0].pic_order_cnt;
    
    for (uint32_t i = 1; i < ctx->dpb_count; i++) {
        if (ctx->dpb[i].pic_order_cnt < oldest_poc) {
            oldest_poc = ctx->dpb[i].pic_order_cnt;
            oldest_idx = i;
        }
    }
    
    /* Output picture */
    outputPicture(ctx, &ctx->dpb[oldest_idx]);
    
    /* Remove from DPB */
    dpbRemovePicture(ctx, oldest_idx);
    
    return H264_SUCCESS;
}

int AppleH264bpd::outputPicture(h264_decoder_ctx_t* ctx, h264_dpb_entry_t* picture)
{
    h264_frame_output_t* output;
    
    if (!ctx || !picture) {
        return H264_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(fOutputLock);
    
    /* Check if output queue is full */
    if (fOutputCount >= 32) {
        lck_mtx_unlock(fOutputLock);
        return H264_ERR_BUSY;
    }
    
    /* Allocate output entry */
    output = (h264_frame_output_t*)IOMalloc(sizeof(h264_frame_output_t));
    if (!output) {
        lck_mtx_unlock(fOutputLock);
        return H264_ERR_NO_MEMORY;
    }
    
    bzero(output, sizeof(h264_frame_output_t));
    
    /* Fill output data */
    output->frame_id = picture->frame_id;
    output->width = picture->width;
    output->height = picture->height;
    output->surface_id = picture->surface_id;
    output->timestamp = picture->timestamp;
    output->flags = picture->flags;
    output->is_idr = picture->is_idr;
    output->is_reference = picture->is_reference;
    
    /* Copy picture data (simplified) */
    if (picture->data && picture->data_size > 0) {
        output->data_size = picture->data_size;
        /* In real implementation, would set up plane pointers */
    }
    
    /* Add to output queue */
    fOutputQueue[fOutputTail] = output;
    fOutputTail = (fOutputTail + 1) % 32;
    fOutputCount++;
    
    ctx->output_queue[ctx->output_count++] = picture;
    
    lck_mtx_unlock(fOutputLock);
    
    return H264_SUCCESS;
}

/*==============================================================================
 * Decoding
 *==============================================================================*/

int AppleH264bpd::decodeNALUnit(h264_decoder_ctx_t* ctx,
                                 const uint8_t* data, uint32_t size,
                                 uint64_t pts, uint64_t dts)
{
    h264_nal_header_t nal_header;
    int ret;
    
    if (!ctx || !data || size == 0) {
        return H264_ERR_INVALID_PARAM;
    }
    
    /* Parse NAL header */
    ret = parseNALHeader(data, size, &nal_header);
    if (ret != H264_SUCCESS) {
        ctx->errors++;
        return ret;
    }
    
    ctx->bytes_processed += size;
    
    /* Process based on NAL type */
    switch (nal_header.nal_unit_type) {
        case H264_NAL_SPS:
            ret = parseSPS(data, size, &ctx->active_sps);
            if (ret == H264_SUCCESS) {
                ctx->sps_valid = 1;
                ctx->width = ctx->active_sps.width;
                ctx->height = ctx->active_sps.height;
                ctx->chroma_format = ctx->active_sps.chroma_format_idc;
                ctx->bit_depth_luma = ctx->active_sps.bit_depth_luma_minus8 + 8;
                ctx->bit_depth_chroma = ctx->active_sps.bit_depth_chroma_minus8 + 8;
                ctx->profile = ctx->active_sps.profile_idc;
                ctx->level = ctx->active_sps.level_idc;
            }
            break;
            
        case H264_NAL_PPS:
            ret = parsePPS(data, size, &ctx->active_pps);
            if (ret == H264_SUCCESS) {
                ctx->pps_valid = 1;
                ctx->entropy_mode = ctx->active_pps.entropy_coding_mode_flag ?
                                    H264_ENTROPY_CABAC : H264_ENTROPY_CAVLC;
            }
            break;
            
        case H264_NAL_SLICE_IDR:
        case H264_NAL_SLICE_NON_IDR:
            if (!ctx->sps_valid || !ctx->pps_valid) {
                return H264_ERR_NOT_READY;
            }
            
            /* Parse slice header */
            h264_slice_header_t slice_header;
            ret = parseSliceHeader(data, size, &slice_header,
                                   &ctx->active_sps, &ctx->active_pps);
            if (ret != H264_SUCCESS) {
                ctx->errors++;
                return ret;
            }
            
            /* Decode slice */
            ret = decodeSlice(ctx, &slice_header, data, size);
            if (ret == H264_SUCCESS) {
                if (nal_header.nal_unit_type == H264_NAL_SLICE_IDR) {
                    ctx->idr_frames++;
                    ctx->frame_num = 0;
                }
                
                /* Update statistics based on slice type */
                switch (slice_header.slice_type) {
                    case H264_SLICE_I:
                        ctx->frames_decoded++;
                        break;
                    case H264_SLICE_P:
                        ctx->p_frames++;
                        ctx->frames_decoded++;
                        break;
                    case H264_SLICE_B:
                        ctx->b_frames++;
                        ctx->frames_decoded++;
                        break;
                }
            }
            break;
            
        case H264_NAL_SEI:
            /* Process SEI messages */
            break;
            
        case H264_NAL_AUD:
            /* Access unit delimiter */
            break;
            
        default:
            /* Unsupported NAL type */
            break;
    }
    
    return ret;
}

int AppleH264bpd::decodeSlice(h264_decoder_ctx_t* ctx,
                               h264_slice_header_t* slice,
                               const uint8_t* data, uint32_t size)
{
    int ret;
    
    if (!ctx || !slice || !data) {
        return H264_ERR_INVALID_PARAM;
    }
    
    /* Build reference picture lists */
    ret = buildReferencePictureLists(ctx, slice);
    if (ret != H264_SUCCESS) {
        return ret;
    }
    
    /* Reference picture marking */
    ret = referencePictureMarking(ctx, slice);
    if (ret != H264_SUCCESS) {
        return ret;
    }
    
    /* Submit to hardware */
    ret = submitToHardware(ctx, data, size, slice);
    if (ret != H264_SUCCESS) {
        return ret;
    }
    
    /* Wait for hardware completion */
    ret = waitForHardware(ctx, 1000);  /* 1 second timeout */
    if (ret != H264_SUCCESS) {
        ctx->errors++;
        fTotalTimeouts++;
        return ret;
    }
    
    /* Create output picture */
    h264_dpb_entry_t picture;
    bzero(&picture, sizeof(picture));
    
    picture.frame_id = ctx->frames_decoded + 1;
    picture.frame_num = slice->frame_num;
    picture.pic_order_cnt = ctx->pic_order_cnt;
    picture.width = ctx->width;
    picture.height = ctx->height;
    picture.timestamp = getCurrentTimestamp();
    picture.is_idr = (slice->slice_type == H264_SLICE_I);
    picture.is_reference = (data[0] & 0x60) != 0;  /* nal_ref_idc > 0 */
    picture.flags = 0;
    
    /* Add to DPB */
    dpbAddPicture(ctx, &picture);
    
    /* Update POC for next frame */
    ctx->prev_frame_num = slice->frame_num;
    
    return H264_SUCCESS;
}

/*==============================================================================
 * Hardware Acceleration
 *==============================================================================*/

int AppleH264bpd::submitToHardware(h264_decoder_ctx_t* ctx,
                                    const uint8_t* data, uint32_t size,
                                    h264_slice_header_t* slice)
{
    /* In real implementation, this would program the hardware decoder */
    /* For simulation, we just return success */
    
    return H264_SUCCESS;
}

int AppleH264bpd::waitForHardware(h264_decoder_ctx_t* ctx, uint32_t timeout_ms)
{
    /* In real implementation, this would wait for hardware completion */
    /* For simulation, we just return success */
    
    return H264_SUCCESS;
}

/*==============================================================================
 * Timer and Utilities
 *==============================================================================*/

void AppleH264bpd::timerFired(void)
{
    /* Periodic tasks */
    
    /* Process any pending hardware events */
    /* Update statistics */
    
    fTimerSource->setTimeoutMS(100);  /* Fire every 100ms */
}

void AppleH264bpd::timerFired(OSObject* owner, IOTimerEventSource* sender)
{
    AppleH264bpd* me = OSDynamicCast(AppleH264bpd, owner);
    if (me) {
        me->timerFired();
    }
}

uint64_t AppleH264bpd::getCurrentTimestamp(void)
{
    uint64_t time;
    clock_get_uptime(&time);
    return time;
}

uint32_t AppleH264bpd::getProfileString(uint32_t profile, char* buf, uint32_t len)
{
    const char* str;
    
    switch (profile) {
        case H264_PROFILE_BASELINE: str = "Baseline"; break;
        case H264_PROFILE_MAIN:     str = "Main"; break;
        case H264_PROFILE_EXTENDED: str = "Extended"; break;
        case H264_PROFILE_HIGH:     str = "High"; break;
        case H264_PROFILE_HIGH10:   str = "High 10"; break;
        case H264_PROFILE_HIGH422:  str = "High 4:2:2"; break;
        case H264_PROFILE_HIGH444:  str = "High 4:4:4"; break;
        default:                     str = "Unknown"; break;
    }
    
    return snprintf(buf, len, "%s", str);
}

uint32_t AppleH264bpd::getLevelString(uint32_t level, char* buf, uint32_t len)
{
    uint32_t major = level / 10;
    uint32_t minor = level % 10;
    
    if (level == 9) {
        return snprintf(buf, len, "1b");
    } else {
        return snprintf(buf, len, "%d.%d", major, minor);
    }
}

/*==============================================================================
 * Hardware Initialization
 *==============================================================================*/

bool AppleH264bpd::initializeHardware(void)
{
    IORegistryEntry* gfx;
    OSData* data;
    
    IOLog("AppleH264bpd: Initializing hardware decoder\n");
    
    /* Get graphics hardware info */
    gfx = IORegistryEntry::fromPath("/", gIODTPlane);
    if (gfx) {
        /* Read hardware capabilities */
        data = OSDynamicCast(OSData, gfx->getProperty("h264-decoder"));
        if (data) {
            /* Parse hardware capabilities */
        }
        
        data = OSDynamicCast(OSData, gfx->getProperty("max-width"));
        if (data && data->getLength() == 4) {
            fMaxWidth = *(uint32_t*)data->getBytesNoCopy();
        } else {
            fMaxWidth = H264_MAX_WIDTH;
        }
        
        data = OSDynamicCast(OSData, gfx->getProperty("max-height"));
        if (data && data->getLength() == 4) {
            fMaxHeight = *(uint32_t*)data->getBytesNoCopy();
        } else {
            fMaxHeight = H264_MAX_HEIGHT;
        }
        
        data = OSDynamicCast(OSData, gfx->getProperty("max-mb-per-frame"));
        if (data && data->getLength() == 4) {
            fMaxMBPerFrame = *(uint32_t*)data->getBytesNoCopy();
        } else {
            fMaxMBPerFrame = H264_MAX_MB_PER_FRAME;
        }
        
        data = OSDynamicCast(OSData, gfx->getProperty("max-dpb-size"));
        if (data && data->getLength() == 4) {
            fMaxDPBSize = *(uint32_t*)data->getBytesNoCopy();
        } else {
            fMaxDPBSize = H264_MAX_DPB_SIZE;
        }
        
        data = OSDynamicCast(OSData, gfx->getProperty("max-ref-frames"));
        if (data && data->getLength() == 4) {
            fMaxRefFrames = *(uint32_t*)data->getBytesNoCopy();
        } else {
            fMaxRefFrames = H264_MAX_REF_FRAMES;
        }
        
        /* Check supported profiles */
        data = OSDynamicCast(OSData, gfx->getProperty("supported-profiles"));
        if (data) {
            uint32_t* profiles = (uint32_t*)data->getBytesNoCopy();
            fNumProfiles = data->getLength() / 4;
            for (uint32_t i = 0; i < fNumProfiles && i < 16; i++) {
                fSupportsProfiles[i] = profiles[i];
            }
        }
        
        /* Check supported levels */
        data = OSDynamicCast(OSData, gfx->getProperty("supported-levels"));
        if (data) {
            uint32_t* levels = (uint32_t*)data->getBytesNoCopy();
            fNumLevels = data->getLength() / 4;
            for (uint32_t i = 0; i < fNumLevels && i < 16; i++) {
                fSupportsLevels[i] = levels[i];
            }
        }
        
        /* Feature flags */
        data = OSDynamicCast(OSData, gfx->getProperty("features"));
        if (data && data->getLength() >= 4) {
            uint32_t features = *(uint32_t*)data->getBytesNoCopy();
            fSupports444 = (features & (1 << 0)) != 0;
            fSupports422 = (features & (1 << 1)) != 0;
            fSupports420 = (features & (1 << 2)) != 0;
            fSupportsMBAFF = (features & (1 << 3)) != 0;
            fSupportsPAFF = (features & (1 << 4)) != 0;
            fSupportsCABAC = (features & (1 << 5)) != 0;
            fSupports8x8Transform = (features & (1 << 6)) != 0;
        }
        
        gfx->release();
    }
    
    IOLog("AppleH264bpd: Hardware capabilities:\n");
    IOLog("  Max Resolution: %d x %d\n", fMaxWidth, fMaxHeight);
    IOLog("  Max MB/Frame: %d\n", fMaxMBPerFrame);
    IOLog("  DPB Size: %d\n", fMaxDPBSize);
    IOLog("  Max Ref Frames: %d\n", fMaxRefFrames);
    IOLog("  Support 4:2:0: %s\n", fSupports420 ? "Yes" : "No");
    IOLog("  Support 4:2:2: %s\n", fSupports422 ? "Yes" : "No");
    IOLog("  Support 4:4:4: %s\n", fSupports444 ? "Yes" : "No");
    IOLog("  Support MBAFF: %s\n", fSupportsMBAFF ? "Yes" : "No");
    IOLog("  Support CABAC: %s\n", fSupportsCABAC ? "Yes" : "No");
    
    return true;
}

void AppleH264bpd::shutdownHardware(void)
{
    IOLog("AppleH264bpd: Shutting down hardware\n");
    
    /* Clean up any active contexts */
    lck_mtx_lock(fContextLock);
    for (int i = 0; i < 8; i++) {
        if (fContexts[i]) {
            destroyDecoderContext(fContexts[i]->context_id);
        }
    }
    lck_mtx_unlock(fContextLock);
}

/*==============================================================================
 * Work Loop Management
 *==============================================================================*/

bool AppleH264bpd::createWorkLoop(void)
{
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        IOLog("AppleH264bpd: Failed to create work loop\n");
        return false;
    }
    
    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        IOLog("AppleH264bpd: Failed to create command gate\n");
        fWorkLoop->release();
        fWorkLoop = NULL;
        return false;
    }
    
    fWorkLoop->addEventSource(fCommandGate);
    
    fTimerSource = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &AppleH264bpd::timerFired));
    
    if (!fTimerSource) {
        IOLog("AppleH264bpd: Failed to create timer source\n");
        fWorkLoop->removeEventSource(fCommandGate);
        fCommandGate->release();
        fCommandGate = NULL;
        fWorkLoop->release();
        fWorkLoop = NULL;
        return false;
    }
    
    fWorkLoop->addEventSource(fTimerSource);
    
    return true;
}

void AppleH264bpd::destroyWorkLoop(void)
{
    if (fWorkLoop) {
        if (fTimerSource) {
            fTimerSource->cancelTimeout();
            fWorkLoop->removeEventSource(fTimerSource);
            fTimerSource->release();
            fTimerSource = NULL;
        }
        
        if (fCommandGate) {
            fWorkLoop->removeEventSource(fCommandGate);
            fCommandGate->release();
            fCommandGate = NULL;
        }
        
        fWorkLoop->release();
        fWorkLoop = NULL;
    }
}

/*==============================================================================
 * AppleH264bpd Initialization
 *==============================================================================*/

bool AppleH264bpd::init(OSDictionary* dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    
    fDriverVersion = APPLE_H264BPD_VERSION;
    fDriverRevision = APPLE_H264BPD_REVISION;
    strlcpy(fDriverBuild, __DATE__ " " __TIME__, sizeof(fDriverBuild));
    
    /* Create lock group */
    fLockGroup = lck_grp_alloc_init("AppleH264bpd", LCK_GRP_ATTR_NULL);
    if (!fLockGroup) {
        return false;
    }
    
    fLockAttr = lck_attr_alloc_init();
    if (!fLockAttr) {
        lck_grp_free(fLockGroup);
        return false;
    }
    
    /* Initialize locks */
    fContextLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fBufferLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fOutputLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    
    if (!fContextLock || !fBufferLock || !fOutputLock) {
        return false;
    }
    
    /* Initialize arrays */
    for (int i = 0; i < 8; i++) {
        fContexts[i] = NULL;
    }
    fNumContexts = 0;
    
    for (int i = 0; i < 32; i++) {
        fBitstreamBuffers[i] = NULL;
        fOutputQueue[i] = NULL;
    }
    
    fOutputHead = 0;
    fOutputTail = 0;
    fOutputCount = 0;
    
    /* Initialize hardware capabilities with defaults */
    fMaxWidth = H264_MAX_WIDTH;
    fMaxHeight = H264_MAX_HEIGHT;
    fMaxMBPerFrame = H264_MAX_MB_PER_FRAME;
    fMaxDPBSize = H264_MAX_DPB_SIZE;
    fMaxRefFrames = H264_MAX_REF_FRAMES;
    
    fSupports420 = true;
    fSupportsCABAC = true;
    
    /* Add default profiles */
    fSupportsProfiles[0] = H264_PROFILE_BASELINE;
    fSupportsProfiles[1] = H264_PROFILE_MAIN;
    fSupportsProfiles[2] = H264_PROFILE_HIGH;
    fNumProfiles = 3;
    
    /* Add default levels */
    fSupportsLevels[0] = H264_LEVEL_3;
    fSupportsLevels[1] = H264_LEVEL_3_1;
    fSupportsLevels[2] = H264_LEVEL_4;
    fSupportsLevels[3] = H264_LEVEL_4_1;
    fNumLevels = 4;
    
    /* Initialize statistics */
    fTotalFrames = 0;
    fTotalIDR = 0;
    fTotalP = 0;
    fTotalB = 0;
    fTotalBytes = 0;
    fTotalErrors = 0;
    fTotalTimeouts = 0;
    
    return true;
}

void AppleH264bpd::free(void)
{
    /* Free locks */
    if (fContextLock) lck_mtx_free(fContextLock, fLockGroup);
    if (fBufferLock) lck_mtx_free(fBufferLock, fLockGroup);
    if (fOutputLock) lck_mtx_free(fOutputLock, fLockGroup);
    
    if (fLockAttr) lck_attr_free(fLockAttr);
    if (fLockGroup) lck_grp_free(fLockGroup);
    
    super::free();
}

bool AppleH264bpd::start(IOService* provider)
{
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("AppleH264bpd: Starting H.264 Bitstream Parser/Decoder version %d.%d\n",
          (fDriverVersion >> 16) & 0xFFFF, fDriverVersion & 0xFFFF);
    
    /* Create work loop */
    if (!createWorkLoop()) {
        IOLog("AppleH264bpd: Failed to create work loop\n");
        return false;
    }
    
    /* Initialize hardware */
    if (!initializeHardware()) {
        IOLog("AppleH264bpd: Hardware initialization failed\n");
        destroyWorkLoop();
        return false;
    }
    
    /* Register power management */
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, NULL, 0);
    
    /* Start timer */
    if (fTimerSource) {
        fTimerSource->setTimeoutMS(100);
    }
    
    /* Publish properties */
    setProperty("Driver Version", fDriverVersion, 32);
    setProperty("Driver Revision", fDriverRevision, 32);
    setProperty("Driver Build", fDriverBuild);
    setProperty("Max Width", fMaxWidth, 32);
    setProperty("Max Height", fMaxHeight, 32);
    setProperty("Max DPB Size", fMaxDPBSize, 32);
    setProperty("Max Reference Frames", fMaxRefFrames, 32);
    setProperty("Supports 4:2:0", fSupports420 ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("Supports 4:2:2", fSupports422 ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("Supports 4:4:4", fSupports444 ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("Supports CABAC", fSupportsCABAC ? kOSBooleanTrue : kOSBooleanFalse);
    
    /* Register service */
    registerService();
    
    IOLog("AppleH264bpd: Started successfully\n");
    
    return true;
}

void AppleH264bpd::stop(IOService* provider)
{
    IOLog("AppleH264bpd: Stopping\n");
    
    /* Stop timer */
    if (fTimerSource) {
        fTimerSource->cancelTimeout();
    }
    
    /* Shutdown hardware */
    shutdownHardware();
    
    /* Power management */
    PMstop();
    
    /* Destroy work loop */
    destroyWorkLoop();
    
    super::stop(provider);
}

IOReturn AppleH264bpd::setPowerState(unsigned long powerState, IOService* device)
{
    if (powerState == 0) {
        /* Going to sleep */
        IOLog("AppleH264bpd: Preparing for sleep\n");
        
        /* Flush any pending operations */
        /* Save hardware state */
        
    } else {
        /* Waking up */
        IOLog("AppleH264bpd: Waking from sleep\n");
        
        /* Restore hardware state */
        initializeHardware();
    }
    
    return IOPMAckImplied;
}

IOReturn AppleH264bpd::newUserClient(task_t owningTask,
                                      void* securityID,
                                      UInt32 type,
                                      OSDictionary* properties,
                                      IOUserClient** handler)
{
    AppleH264bpdUserClient* client;
    
    client = new AppleH264bpdUserClient;
    if (!client) {
        return kIOReturnNoMemory;
    }
    
    if (!client->init()) {
        client->release();
        return kIOReturnNoMemory;
    }
    
    if (!client->attach(this)) {
        client->release();
        return kIOReturnNoMemory;
    }
    
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnNoMemory;
    }
    
    *handler = client;
    
    return kIOReturnSuccess;
}

/*==============================================================================
 * AppleH264bpdUserClient Implementation
 *==============================================================================*/

bool AppleH264bpdUserClient::init(OSDictionary* dictionary)
{
    if (!IOUserClient::init(dictionary)) {
        return false;
    }
    
    fTask = NULL;
    fPid = 0;
    fClientId = 0;
    fProvider = NULL;
    fState = 0;
    fInputBuffer = NULL;
    fOutputBuffer = NULL;
    fInputMapping = NULL;
    fOutputMapping = NULL;
    fInputSize = 0;
    fOutputSize = 0;
    fDecoderContextId = 0;
    fValid = false;
    
    return true;
}

void AppleH264bpdUserClient::free(void)
{
    if (fInputMapping && fInputBuffer) {
        fInputBuffer->complete();
        fInputMapping = NULL;
    }
    
    if (fInputBuffer) {
        fInputBuffer->release();
        fInputBuffer = NULL;
    }
    
    if (fOutputMapping && fOutputBuffer) {
        fOutputBuffer->complete();
        fOutputMapping = NULL;
    }
    
    if (fOutputBuffer) {
        fOutputBuffer->release();
        fOutputBuffer = NULL;
    }
    
    IOUserClient::free();
}

bool AppleH264bpdUserClient::start(IOService* provider)
{
    if (!IOUserClient::start(provider)) {
        return false;
    }
    
    fProvider = OSDynamicCast(AppleH264bpd, provider);
    if (!fProvider) {
        return false;
    }
    
    fTask = current_task();
    fPid = pid_from_task(fTask);
    fValid = true;
    
    return true;
}

void AppleH264bpdUserClient::stop(IOService* provider)
{
    if (fValid && fProvider && fDecoderContextId) {
        fProvider->destroyDecoderContext(fDecoderContextId);
    }
    
    fValid = false;
    
    IOUserClient::stop(provider);
}

IOReturn AppleH264bpdUserClient::clientClose(void)
{
    if (fValid && fProvider && fDecoderContextId) {
        fProvider->destroyDecoderContext(fDecoderContextId);
    }
    
    fValid = false;
    
    terminate();
    
    return kIOReturnSuccess;
}

IOReturn AppleH264bpdUserClient::clientDied(void)
{
    return clientClose();
}

IOReturn AppleH264bpdUserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)
{
    return kIOReturnUnsupported;
}

IOReturn AppleH264bpdUserClient::externalMethod(uint32_t selector,
                                                 IOExternalMethodArguments* arguments,
                                                 IOExternalMethodDispatch* dispatch,
                                                 OSObject* target,
                                                 void* reference)
{
    if (selector >= kMethodCount) {
        return kIOReturnBadArgument;
    }
    
    dispatch = (IOExternalMethodDispatch*)&sMethods[selector];
    
    return dispatch->function(this, reference, arguments);
}

/* External method implementations */

IOReturn AppleH264bpdUserClient::sCreateDecoder(AppleH264bpdUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args)
{
    uint32_t context_id;
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ret = target->fProvider->createDecoderContext(&context_id);
    
    if (ret == H264_SUCCESS) {
        target->fDecoderContextId = context_id;
        args->scalarOutput[0] = context_id;
        return kIOReturnSuccess;
    }
    
    return kIOReturnError;
}

IOReturn AppleH264bpdUserClient::sDestroyDecoder(AppleH264bpdUserClient* target,
                                                  void* reference,
                                                  IOExternalMethodArguments* args)
{
    uint32_t context_id = (uint32_t)args->scalarInput[0];
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ret = target->fProvider->destroyDecoderContext(context_id);
    
    return (ret == H264_SUCCESS) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH264bpdUserClient::sConfigureDecoder(AppleH264bpdUserClient* target,
                                                    void* reference,
                                                    IOExternalMethodArguments* args)
{
    uint32_t context_id = (uint32_t)args->scalarInput[0];
    uint32_t width = (uint32_t)args->scalarInput[1];
    uint32_t height = (uint32_t)args->scalarInput[2];
    
    h264_decoder_ctx_t* ctx;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ctx = target->fProvider->findDecoderContext(context_id);
    if (!ctx) {
        return kIOReturnBadArgument;
    }
    
    /* Configure decoder */
    ctx->width = width;
    ctx->height = height;
    ctx->state = H264_STATE_CONFIGURED;
    
    return kIOReturnSuccess;
}

IOReturn AppleH264bpdUserClient::sDecodeNAL(AppleH264bpdUserClient* target,
                                             void* reference,
                                             IOExternalMethodArguments* args)
{
    uint32_t context_id = (uint32_t)args->scalarInput[0];
    uint64_t pts = (uint64_t)args->scalarInput[1];
    uint64_t dts = pts;  /* For simplicity */
    
    IOMemoryDescriptor* buffer = args->structureInputDescriptor;
    uint32_t buffer_size = (uint32_t)args->structureInputSize;
    
    h264_decoder_ctx_t* ctx;
    uint8_t* mapping;
    int ret;
    
    if (!target || !target->fValid || !target->fProvider || !buffer) {
        return kIOReturnNotAttached;
    }
    
    ctx = target->fProvider->findDecoderContext(context_id);
    if (!ctx) {
        return kIOReturnBadArgument;
    }
    
    /* Map input buffer */
    mapping = (uint8_t*)buffer->map();
    if (!mapping) {
        return kIOReturnNoMemory;
    }
    
    /* Decode NAL unit */
    ret = target->fProvider->decodeNALUnit(ctx, mapping, buffer_size, pts, dts);
    
    buffer->unmap();
    
    args->scalarOutput[0] = ret;
    
    return (ret >= 0) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH264bpdUserClient::sDecodeFrame(AppleH264bpdUserClient* target,
                                               void* reference,
                                               IOExternalMethodArguments* args)
{
    /* Similar to decodeNAL but for full frames */
    return sDecodeNAL(target, reference, args);
}

IOReturn AppleH264bpdUserClient::sGetOutput(AppleH264bpdUserClient* target,
                                             void* reference,
                                             IOExternalMethodArguments* args)
{
    h264_frame_output_t* output = NULL;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Get next output frame from queue */
    lck_mtx_lock(target->fProvider->fOutputLock);
    
    if (target->fProvider->fOutputCount > 0) {
        output = target->fProvider->fOutputQueue[target->fProvider->fOutputHead];
        target->fProvider->fOutputHead = (target->fProvider->fOutputHead + 1) % 32;
        target->fProvider->fOutputCount--;
    }
    
    lck_mtx_unlock(target->fProvider->fOutputLock);
    
    if (output) {
        /* Copy output to user space */
        if (args->structureOutputSize >= sizeof(h264_frame_output_t)) {
            memcpy(args->structureOutput, output, sizeof(h264_frame_output_t));
            IOFree(output, sizeof(h264_frame_output_t));
            return kIOReturnSuccess;
        } else {
            IOFree(output, sizeof(h264_frame_output_t));
            return kIOReturnBadArgument;
        }
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleH264bpdUserClient::sFlushDecoder(AppleH264bpdUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args)
{
    uint32_t context_id = (uint32_t)args->scalarInput[0];
    h264_decoder_ctx_t* ctx;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ctx = target->fProvider->findDecoderContext(context_id);
    if (!ctx) {
        return kIOReturnBadArgument;
    }
    
    /* Flush decoder - output all remaining frames */
    while (ctx->dpb_count > 0) {
        target->fProvider->dpbBump(ctx);
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleH264bpdUserClient::sResetDecoder(AppleH264bpdUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args)
{
    uint32_t context_id = (uint32_t)args->scalarInput[0];
    h264_decoder_ctx_t* ctx;
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ctx = target->fProvider->findDecoderContext(context_id);
    if (!ctx) {
        return kIOReturnBadArgument;
    }
    
    ret = target->fProvider->resetDecoderContext(ctx);
    
    return (ret == H264_SUCCESS) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH264bpdUserClient::sGetCapabilities(AppleH264bpdUserClient* target,
                                                    void* reference,
                                                    IOExternalMethodArguments* args)
{
    struct {
        uint32_t max_width;
        uint32_t max_height;
        uint32_t max_dpb_size;
        uint32_t max_ref_frames;
        uint32_t num_profiles;
        uint32_t profiles[16];
        uint32_t num_levels;
        uint32_t levels[16];
        uint32_t features;
    } caps;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    bzero(&caps, sizeof(caps));
    
    caps.max_width = target->fProvider->fMaxWidth;
    caps.max_height = target->fProvider->fMaxHeight;
    caps.max_dpb_size = target->fProvider->fMaxDPBSize;
    caps.max_ref_frames = target->fProvider->fMaxRefFrames;
    
    caps.num_profiles = target->fProvider->fNumProfiles;
    for (uint32_t i = 0; i < caps.num_profiles; i++) {
        caps.profiles[i] = target->fProvider->fSupportsProfiles[i];
    }
    
    caps.num_levels = target->fProvider->fNumLevels;
    for (uint32_t i = 0; i < caps.num_levels; i++) {
        caps.levels[i] = target->fProvider->fSupportsLevels[i];
    }
    
    caps.features = (target->fProvider->fSupports420 ? (1 << 2) : 0) |
                    (target->fProvider->fSupports422 ? (1 << 1) : 0) |
                    (target->fProvider->fSupports444 ? (1 << 0) : 0) |
                    (target->fProvider->fSupportsMBAFF ? (1 << 3) : 0) |
                    (target->fProvider->fSupportsPAFF ? (1 << 4) : 0) |
                    (target->fProvider->fSupportsCABAC ? (1 << 5) : 0) |
                    (target->fProvider->fSupports8x8Transform ? (1 << 6) : 0);
    
    if (args->structureOutputSize >= sizeof(caps)) {
        memcpy(args->structureOutput, &caps, sizeof(caps));
        return kIOReturnSuccess;
    }
    
    return kIOReturnBadArgument;
}

IOReturn AppleH264bpdUserClient::sGetStatistics(AppleH264bpdUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args)
{
    uint32_t context_id = (uint32_t)args->scalarInput[0];
    struct {
        uint64_t frames_decoded;
        uint64_t idr_frames;
        uint64_t p_frames;
        uint64_t b_frames;
        uint64_t bytes_processed;
        uint64_t errors;
        uint32_t state;
    } stats;
    
    h264_decoder_ctx_t* ctx;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ctx = target->fProvider->findDecoderContext(context_id);
    if (!ctx) {
        return kIOReturnBadArgument;
    }
    
    bzero(&stats, sizeof(stats));
    
    stats.frames_decoded = ctx->frames_decoded;
    stats.idr_frames = ctx->idr_frames;
    stats.p_frames = ctx->p_frames;
    stats.b_frames = ctx->b_frames;
    stats.bytes_processed = ctx->bytes_processed;
    stats.errors = ctx->errors;
    stats.state = ctx->state;
    
    if (args->structureOutputSize >= sizeof(stats)) {
        memcpy(args->structureOutput, &stats, sizeof(stats));
        return kIOReturnSuccess;
    }
    
    return kIOReturnBadArgument;
}
