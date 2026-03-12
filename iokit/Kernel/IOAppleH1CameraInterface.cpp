/*
 * Copyright (c) 2018-2022 Apple Inc. All rights reserved.
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
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOInterruptEventSource.h>

#include <IOKit/usb/IOUSBBus.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>

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
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/syslog.h>

#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/host.h>
#include <kern/processor.h>
#include <kern/policy.h>

#include <machine/machine_routines.h>
#include <machine/cpu_capabilities.h>

#include <uuid/uuid.h>

/*==============================================================================
 * AppleH1CameraInterface - H1 Bridge Chip Camera Interface Driver
 * 
 * This driver provides the interface between the Apple H1 bridge chip
 * and the camera subsystem on Macs with T2/H1 chips (iBridge).
 * 
 * The H1 chip (also known as iBridge) handles:
 * - Camera sensor control (I2C/CCI)
 * - MIPI CSI-2 receiver interface
 * - Image signal processing (ISP)
 * - Secure video path
 * - Power management for camera subsystem
 *==============================================================================*/

/*==============================================================================
 * Constants and Definitions
 *==============================================================================*/

#define APPLE_H1_CAMERA_VERSION         0x00030001  /* Version 3.1 */
#define APPLE_H1_CAMERA_REVISION        0x00000002

/* Maximum values */
#define H1_MAX_CAMERA_SENSORS           4
#define H1_MAX_STREAMS                   8
#define H1_MAX_FRAME_BUFFERS              16
#define H1_MAX_IMAGE_WIDTH                8192
#define H1_MAX_IMAGE_HEIGHT               8192
#define H1_MAX_FRAME_SIZE                  0x4000000  /* 64MB */
#define H1_MAX_METADATA_SIZE               0x10000    /* 64KB */
#define H1_MAX_EXPOSURE_VALUE              0xFFFFFFFF
#define H1_MAX_GAIN_VALUE                   0xFFFF
#define H1_MAX_FPS                          240
#define H1_MAX_ISO                          102400
#define H1_MAX_FOCUS_STEPS                   1000
#define H1_MAX_ZOOM_FACTOR                    10

/* H1 Bridge chip registers (via AppleSMC/AppleH1) */
#define H1_REG_CAMERA_CONTROL             0x1000
#define H1_REG_CAMERA_STATUS              0x1004
#define H1_REG_CAMERA_INT_STATUS           0x1008
#define H1_REG_CAMERA_INT_ENABLE           0x100C
#define H1_REG_CAMERA_SENSOR_SELECT        0x1010
#define H1_REG_CAMERA_STREAM_CONTROL       0x1014
#define H1_REG_CAMERA_FRAME_COUNT          0x1018
#define H1_REG_CAMERA_ERROR_COUNT          0x101C
#define H1_REG_CAMERA_POWER_STATE          0x1020
#define H1_REG_CAMERA_CLOCK_CONTROL        0x1024
#define H1_REG_CAMERA_MIPI_CONTROL         0x1028
#define H1_REG_CAMERA_MIPI_STATUS          0x102C
#define H1_REG_CAMERA_CSI2_PHY_CONTROL     0x1030
#define H1_REG_CAMERA_CSI2_PHY_STATUS      0x1034
#define H1_REG_CAMERA_ISP_CONTROL          0x1040
#define H1_REG_CAMERA_ISP_STATUS           0x1044
#define H1_REG_CAMERA_ISP_PARAM_ADDR       0x1048
#define H1_REG_CAMERA_ISP_PARAM_SIZE       0x104C
#define H1_REG_CAMERA_DMA_ADDR             0x1050
#define H1_REG_CAMERA_DMA_SIZE             0x1054
#define H1_REG_CAMERA_DMA_CONTROL          0x1058
#define H1_REG_CAMERA_DMA_STATUS           0x105C
#define H1_REG_CAMERA_SECURE_CONTROL       0x1060
#define H1_REG_CAMERA_SECURE_STATUS        0x1064
#define H1_REG_CAMERA_THERMAL_STATUS       0x1070
#define H1_REG_CAMERA_VOLTAGE_STATUS       0x1074
#define H1_REG_CAMERA_CURRENT_STATUS       0x1078
#define H1_REG_CAMERA_DEBUG                 0x1FF0

/* Camera control bits */
#define H1_CTRL_POWER_UP                   (1 << 0)
#define H1_CTRL_POWER_DOWN                  (1 << 1)
#define H1_CTRL_RESET                       (1 << 2)
#define H1_CTRL_START_STREAM                (1 << 3)
#define H1_CTRL_STOP_STREAM                  (1 << 4)
#define H1_CTRL_CAPTURE_IMAGE                (1 << 5)
#define H1_CTRL_AF_TRIGGER                   (1 << 6)
#define H1_CTRL_AE_TRIGGER                   (1 << 7)
#define H1_CTRL_AWB_TRIGGER                  (1 << 8)
#define H1_CTRL_FLASH_ENABLE                  (1 << 9)
#define H1_CTRL_TORCH_ENABLE                  (1 << 10)

/* Camera status bits */
#define H1_STATUS_POWERED                    (1 << 0)
#define H1_STATUS_READY                      (1 << 1)
#define H1_STATUS_STREAMING                   (1 << 2)
#define H1_STATUS_CAPTURING                   (1 << 3)
#define H1_STATUS_FOCUSING                    (1 << 4)
#define H1_STATUS_FOCUS_LOCKED                (1 << 5)
#define H1_STATUS_ERROR                       (1 << 31)

/* Interrupt bits */
#define H1_INT_FRAME_READY                   (1 << 0)
#define H1_INT_FRAME_DONE                     (1 << 1)
#define H1_INT_FRAME_ERROR                    (1 << 2)
#define H1_INT_STREAM_STARTED                 (1 << 3)
#define H1_INT_STREAM_STOPPED                 (1 << 4)
#define H1_INT_AF_COMPLETE                    (1 << 5)
#define H1_INT_AE_COMPLETE                    (1 << 6)
#define H1_INT_AWB_COMPLETE                   (1 << 7)
#define H1_INT_SENSOR_ERROR                   (1 << 8)
#define H1_INT_MIPI_ERROR                     (1 << 9)
#define H1_INT_ISP_DONE                       (1 << 10)
#define H1_INT_THERMAL_WARNING                 (1 << 11)
#define H1_INT_THERMAL_SHUTDOWN                (1 << 12)
#define H1_INT_SECURE_VIOLATION                (1 << 13)

/* Power states */
#define H1_POWER_OFF                         0
#define H1_POWER_SLEEP                        1
#define H1_POWER_IDLE                         2
#define H1_POWER_ACTIVE                       3
#define H1_POWER_MAX                          4

/* MIPI CSI-2 Lane configurations */
#define H1_MIPI_LANES_1                       1
#define H1_MIPI_LANES_2                       2
#define H1_MIPI_LANES_4                       4

/* Camera sensor types */
enum {
    kH1SensorTypeUnknown           = 0,
    kH1SensorTypeRaw               = 1,      /* RAW Bayer */
    kH1SensorTypeYUV               = 2,      /* YUV 4:2:2 */
    kH1SensorTypeRGB               = 3,      /* RGB */
    kH1SensorTypeIR                = 4,      /* Infrared */
    kH1SensorTypeDepth             = 5,      /* Depth (ToF) */
    kH1SensorTypeMono               = 6,      /* Monochrome */
    kH1SensorTypeJpeg               = 7       /* JPEG compressed */
};

/* Pixel formats (FourCC codes) */
#define H1_PIXEL_FMT_RAW8           0x52575738  /* RAW8 */
#define H1_PIXEL_FMT_RAW10          0x52573130  /* RAW10 */
#define H1_PIXEL_FMT_RAW12          0x52573132  /* RAW12 */
#define H1_PIXEL_FMT_RAW14          0x52573134  /* RAW14 */
#define H1_PIXEL_FMT_RAW16          0x52573136  /* RAW16 */
#define H1_PIXEL_FMT_YUYV           0x59555956  /* YUYV 4:2:2 */
#define H1_PIXEL_FMT_UYVY           0x55595659  /* UYVY 4:2:2 */
#define H1_PIXEL_FMT_NV12           0x4E563132  /* NV12 */
#define H1_PIXEL_FMT_NV21           0x4E563231  /* NV21 */
#define H1_PIXEL_FMT_RGB565         0x52474236  /* RGB565 */
#define H1_PIXEL_FMT_RGB24          0x52473234  /* RGB24 */
#define H1_PIXEL_FMT_BGR24          0x42473234  /* BGR24 */
#define H1_PIXEL_FMT_ARGB32         0x41524742  /* ARGB32 */
#define H1_PIXEL_FMT_JPEG           0x4A504547  /* JPEG */
#define H1_PIXEL_FMT_HEIC           0x48454943  /* HEIC */

/* Stream types */
enum {
    kH1StreamTypePreview           = 0x01,
    kH1StreamTypeCapture           = 0x02,
    kH1StreamTypeVideo             = 0x04,
    kH1StreamTypeDepth             = 0x08,
    kH1StreamTypeMetadata          = 0x10,
    kH1StreamTypeThumbnail         = 0x20
};

/* Frame flags */
#define H1_FRAME_FLAG_VALID                  (1 << 0)
#define H1_FRAME_FLAG_ERROR                   (1 << 1)
#define H1_FRAME_FLAG_DROPPED                 (1 << 2)
#define H1_FRAME_FLAG_CORRUPT                 (1 << 3)
#define H1_FRAME_FLAG_KEY_FRAME                (1 << 4)
#define H1_FRAME_FLAG_HDR                      (1 << 5)
#define H1_FRAME_FLAG_FLASH                    (1 << 6)
#define H1_FRAME_FLAG_TORCH                    (1 << 7)

/* Error codes */
#define kH1Success                   0
#define kH1ErrGeneral                -1
#define kH1ErrNoMemory               -2
#define kH1ErrInvalidParam           -3
#define kH1ErrNotReady               -4
#define kH1ErrBusy                   -5
#define kH1ErrTimeout                 -6
#define kH1ErrAccessDenied           -7
#define kH1ErrNoData                 -8
#define kH1ErrNotSupported           -9
#define kH1ErrBufferTooSmall         -10
#define kH1ErrSensorError            -11
#define kH1ErrMipiError              -12
#define kH1ErrIspError               -13
#define kH1ErrDmaError               -14
#define kH1ErrSecureViolation        -15
#define kH1ErrThermalShutdown        -16
#define kH1ErrPowerFailure           -17

/*==============================================================================
 * Data Structures
 *==============================================================================*/

/* Camera sensor capabilities */
typedef struct {
    char            name[64];                   /* Sensor name */
    uint32_t        sensor_type;                 /* Sensor type */
    uint32_t        pixel_format;                 /* Native pixel format */
    uint32_t        bit_depth;                    /* Bit depth */
    uint32_t        max_width;                     /* Maximum width */
    uint32_t        max_height;                    /* Maximum height */
    uint32_t        min_width;                     /* Minimum width */
    uint32_t        min_height;                    /* Minimum height */
    uint32_t        supported_fps[16];             /* Supported FPS values */
    uint32_t        num_fps;                       /* Number of FPS values */
    uint32_t        exposure_min;                  /* Minimum exposure (µs) */
    uint32_t        exposure_max;                  /* Maximum exposure (µs) */
    uint32_t        gain_min;                      /* Minimum gain (0.001 dB) */
    uint32_t        gain_max;                      /* Maximum gain (0.001 dB) */
    uint32_t        iso_min;                       /* Minimum ISO */
    uint32_t        iso_max;                       /* Maximum ISO */
    bool            has_autofocus;                 /* Supports autofocus */
    bool            has_autoexposure;              /* Supports autoexposure */
    bool            has_autowhitebalance;          /* Supports AWB */
    bool            has_flash;                     /* Has flash */
    bool            has_torch;                     /* Has torch mode */
    bool            has_hdr;                        /* Supports HDR */
    bool            has_eis;                        /* Electronic image stabilization */
    bool            has_ois;                        /* Optical image stabilization */
    bool            has_secure_mode;                /* Secure video mode */
    uint32_t        capabilities;                   /* Additional capabilities */
} h1_sensor_caps_t;

/* Camera sensor configuration */
typedef struct {
    uint32_t        sensor_id;                     /* Sensor ID (0-3) */
    uint32_t        width;                          /* Image width */
    uint32_t        height;                         /* Image height */
    uint32_t        fps;                            /* Frames per second */
    uint32_t        pixel_format;                   /* Pixel format */
    uint32_t        bit_depth;                      /* Bit depth */
    uint32_t        crop_x;                         /* Crop X offset */
    uint32_t        crop_y;                         /* Crop Y offset */
    uint32_t        crop_width;                     /* Crop width */
    uint32_t        crop_height;                    /* Crop height */
    uint32_t        exposure;                       /* Exposure time (µs) */
    uint32_t        gain;                           /* Gain (0.001 dB) */
    uint32_t        iso;                            /* ISO value */
    uint32_t        whitebalance_red;               /* White balance red gain */
    uint32_t        whitebalance_green;             /* White balance green gain */
    uint32_t        whitebalance_blue;              /* White balance blue gain */
    uint32_t        focus_position;                  /* Focus position (0-1000) */
    uint32_t        zoom_factor;                     /* Zoom factor (1-10) */
    bool            hdr_mode;                        /* HDR enabled */
    bool            flash_mode;                      /* Flash enabled */
    bool            torch_mode;                      /* Torch enabled */
    bool            secure_mode;                     /* Secure mode enabled */
    uint32_t        stream_mask;                     /* Active streams mask */
    uint32_t        reserved[8];
} h1_sensor_config_t;

/* Frame buffer */
typedef struct {
    uint32_t        frame_id;                       /* Frame ID */
    uint32_t        sensor_id;                      /* Sensor ID */
    uint32_t        stream_id;                      /* Stream ID */
    uint64_t        timestamp;                       /* Capture timestamp */
    uint64_t        exposure_start;                  /* Exposure start time */
    uint64_t        exposure_end;                    /* Exposure end time */
    uint32_t        width;                           /* Image width */
    uint32_t        height;                          /* Image height */
    uint32_t        stride;                          /* Line stride (bytes) */
    uint32_t        pixel_format;                    /* Pixel format */
    uint32_t        bit_depth;                       /* Bit depth */
    uint32_t        data_size;                       /* Data size (bytes) */
    uint32_t        metadata_size;                   /* Metadata size (bytes) */
    uint32_t        flags;                           /* Frame flags */
    uint32_t        exposure;                        /* Actual exposure (µs) */
    uint32_t        gain;                            /* Actual gain (0.001 dB) */
    uint32_t        iso;                             /* Actual ISO */
    uint32_t        temperature;                      /* Sensor temperature */
    uint32_t        error_code;                      /* Error code if any */
    uint64_t        user_data;                       /* User data */
    uint8_t*        data;                            /* Frame data */
    uint8_t*        metadata;                        /* Frame metadata */
} h1_frame_t;

/* Stream configuration */
typedef struct {
    uint32_t        stream_id;                      /* Stream ID */
    uint32_t        stream_type;                     /* Stream type */
    uint32_t        sensor_id;                       /* Source sensor */
    uint32_t        width;                           /* Stream width */
    uint32_t        height;                          /* Stream height */
    uint32_t        stride;                          /* Line stride */
    uint32_t        pixel_format;                    /* Pixel format */
    uint32_t        bit_depth;                       /* Bit depth */
    uint32_t        fps;                             /* Target FPS */
    uint32_t        buffer_count;                    /* Number of buffers */
    uint32_t        buffer_size;                      /* Buffer size */
    bool            circular;                         /* Circular buffer */
    bool            secure;                           /* Secure stream */
    uint64_t        dma_address;                      /* DMA address */
    uint32_t        reserved[8];
} h1_stream_config_t;

/* ISP parameters */
typedef struct {
    uint32_t        sharpness;                       /* Sharpness (0-100) */
    uint32_t        contrast;                        /* Contrast (0-100) */
    uint32_t        brightness;                      /* Brightness (0-100) */
    uint32_t        saturation;                      /* Saturation (0-100) */
    uint32_t        hue;                             /* Hue (0-360) */
    uint32_t        denoise_level;                   /* Denoise level (0-100) */
    uint32_t        gamma;                           /* Gamma correction */
    uint32_t        black_level;                      /* Black level */
    uint32_t        white_level;                      /* White level */
    uint32_t        color_temp;                       /* Color temperature */
    int32_t         lens_shading[4][4];               /* Lens shading correction */
    uint32_t        reserved[32];
} h1_isp_params_t;

/* Event data */
typedef struct {
    uint32_t        event_id;                        /* Event ID */
    uint32_t        event_type;                      /* Event type */
    uint64_t        timestamp;                       /* Event timestamp */
    uint32_t        sensor_id;                       /* Related sensor */
    uint32_t        data_size;                        /* Additional data size */
    uint8_t         data[0];                          /* Additional data */
} h1_camera_event_t;

/* Statistics */
typedef struct {
    uint64_t        frames_captured;                  /* Total frames captured */
    uint64_t        frames_dropped;                   /* Frames dropped */
    uint64_t        frames_error;                      /* Frames with errors */
    uint64_t        bytes_transferred;                 /* Total bytes */
    uint64_t        exposure_time;                     /* Total exposure time */
    uint32_t        temperature_peak;                  /* Peak temperature */
    uint32_t        power_consumption;                  /* Average power (mW) */
    uint32_t        error_count;                        /* Error count */
    uint32_t        thermal_warnings;                   /* Thermal warnings */
    uint32_t        secure_violations;                  /* Security violations */
    uint32_t        reserved[8];
} h1_camera_stats_t;

/*==============================================================================
 * AppleH1CameraInterface Main Class
 *==============================================================================*/

class AppleH1CameraInterface : public IOService
{
    OSDeclareDefaultStructors(AppleH1CameraInterface)
    
private:
    /* IOKit resources */
    IOWorkLoop*                 fWorkLoop;
    IOCommandGate*              fCommandGate;
    IOTimerEventSource*         fTimerSource;
    IOInterruptEventSource*     fInterruptSource;
    
    /* H1 bridge interface */
    IOService*                  fH1Service;
    IOMemoryMap*                fH1MemoryMap;
    volatile uint32_t*          fH1Registers;
    IOInterruptController*      fInterruptController;
    
    /* USB interface (for external cameras) */
    IOUSBHostDevice*            fUSBDevice;
    IOUSBHostInterface*         fUSBInterface;
    IOUSBHostPipe*              fUSBControlPipe;
    IOUSBHostPipe*              fUSBBulkPipe;
    
    /* State */
    uint32_t                    fState;
    uint32_t                    fFlags;
    uint64_t                    fStartTime;
    
    /* Version information */
    uint32_t                    fDriverVersion;
    uint32_t                    fDriverRevision;
    char                        fDriverBuild[32];
    
    /* Camera sensors */
    h1_sensor_caps_t            fSensorCaps[H1_MAX_CAMERA_SENSORS];
    h1_sensor_config_t          fSensorConfigs[H1_MAX_CAMERA_SENSORS];
    uint32_t                    fNumSensors;
    uint32_t                    fActiveSensor;
    lck_mtx_t*                  fSensorLock;
    
    /* Stream management */
    h1_stream_config_t          fStreams[H1_MAX_STREAMS];
    uint32_t                    fNumStreams;
    uint32_t                    fActiveStreams;
    lck_mtx_t*                  fStreamLock;
    
    /* Frame buffers */
    h1_frame_t*                 fFrames[H1_MAX_FRAME_BUFFERS];
    uint32_t                    fFrameCount;
    uint32_t                    fFrameHead;
    uint32_t                    fFrameTail;
    uint32_t                    fFramePending;
    lck_mtx_t*                  fFrameLock;
    
    /* DMA buffers */
    IOBufferMemoryDescriptor*   fDMABuffer;
    uint64_t                    fDMAPhysAddr;
    uint32_t                    fDMASize;
    uint32_t                    fDMAPosition;
    
    /* ISP */
    h1_isp_params_t             fISPParams;
    bool                        fISPEnabled;
    lck_mtx_t*                  fISPLock;
    
    /* Power management */
    uint32_t                    fPowerState;
    uint32_t                    fThermalLevel;
    uint32_t                    fVoltage;
    uint32_t                    fCurrent;
    uint64_t                    fLastPowerSample;
    
    /* Statistics */
    h1_camera_stats_t           fStats;
    
    /* Security */
    bool                        fSecureMode;
    uint8_t                     fSecureKey[32];
    uint64_t                    fSecureSessionId;
    
    /* Client management */
    task_t                      fExclusiveClient;
    uint32_t                    fClientCount;
    lck_mtx_t*                  fClientLock;
    
    /* Event queue */
    h1_camera_event_t**         fEventQueue;
    uint32_t                    fEventQueueHead;
    uint32_t                    fEventQueueTail;
    uint32_t                    fEventQueueSize;
    uint32_t                    fEventQueueMax;
    lck_mtx_t*                  fEventLock;
    
    /* Locking */
    lck_grp_t*                  fLockGroup;
    lck_attr_t*                 fLockAttr;
    
    /*==============================================================================
     * Private Methods
     *==============================================================================*/
    
    bool                        initializeHardware(void);
    void                        shutdownHardware(void);
    
    bool                        createWorkLoop(void);
    void                        destroyWorkLoop(void);
    
    void                        timerFired(void);
    static void                 timerFired(OSObject* owner, IOTimerEventSource* sender);
    
    void                        interruptHandler(void);
    static void                 interruptHandler(OSObject* owner, IOInterruptEventSource* sender, int count);
    
    IOReturn                    handleCommand(void* arg0, void* arg1, void* arg2, void* arg3);
    
    /* H1 register access */
    uint32_t                    readH1Register(uint32_t offset);
    void                        writeH1Register(uint32_t offset, uint32_t value);
    bool                        waitForH1Bit(uint32_t offset, uint32_t bit, bool set, uint32_t timeout);
    
    /* Sensor management */
    int                         enumerateSensors(void);
    int                         powerOnSensor(uint32_t sensorId);
    int                         powerOffSensor(uint32_t sensorId);
    int                         configureSensor(uint32_t sensorId, h1_sensor_config_t* config);
    int                         startSensor(uint32_t sensorId);
    int                         stopSensor(uint32_t sensorId);
    
    /* Stream management */
    int                         createStream(h1_stream_config_t* config, uint32_t* streamId);
    int                         destroyStream(uint32_t streamId);
    int                         startStream(uint32_t streamId);
    int                         stopStream(uint32_t streamId);
    int                         queueFrame(uint32_t streamId, h1_frame_t* frame);
    int                         dequeueFrame(h1_frame_t** frame);
    
    /* Frame processing */
    int                         allocateFrameBuffers(uint32_t count, uint32_t size);
    void                        freeFrameBuffers(void);
    int                         processFrame(h1_frame_t* frame);
    
    /* MIPI CSI-2 interface */
    int                         configureMIPI(uint32_t lanes, uint32_t clock);
    int                         startMIPI(void);
    int                         stopMIPI(void);
    
    /* ISP processing */
    int                         configureISP(h1_isp_params_t* params);
    int                         enableISP(bool enable);
    int                         processISP(h1_frame_t* input, h1_frame_t* output);
    
    /* Security */
    int                         enableSecureMode(uint64_t sessionId, uint8_t* key);
    int                         disableSecureMode(void);
    bool                        validateSecureFrame(h1_frame_t* frame);
    
    /* Event handling */
    int                         queueEvent(uint32_t eventType, uint32_t sensorId, void* data, uint32_t dataSize);
    int                         processEvents(void);
    
    /* Power management */
    void                        updatePowerState(uint32_t newState);
    void                        samplePowerData(void);
    void                        handleThermalEvent(void);
    
    /* USB interface (for external cameras) */
    bool                        probeUSBDevice(IOUSBHostDevice* device);
    bool                        configureUSBInterface(IOUSBHostInterface* interface);
    int                         usbControlRequest(uint8_t bRequest, uint16_t wValue, uint16_t wIndex, void* data, uint16_t wLength);
    int                         usbBulkTransfer(void* data, uint32_t length, uint32_t timeout);
    
protected:
    bool                        init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                        free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                        start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                        stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    /* Power management */
    IOReturn                    setPowerState(unsigned long powerState, IOService* device) APPLE_KEXT_OVERRIDE;
    IOReturn                    powerStateWillChangeTo(IOPMPowerFlags capabilities,
                                                        unsigned long stateNumber,
                                                        IOService* whatDevice) APPLE_KEXT_OVERRIDE;
    
    /* User client access */
    IOReturn                    newUserClient(task_t owningTask,
                                              void* securityID,
                                              UInt32 type,
                                              OSDictionary* properties,
                                              IOUserClient** handler) APPLE_KEXT_OVERRIDE;
    
    /* Public API */
    int                         getSensorCount(uint32_t* count);
    int                         getSensorCaps(uint32_t sensorId, h1_sensor_caps_t* caps);
    int                         setSensorConfig(uint32_t sensorId, h1_sensor_config_t* config);
    int                         getSensorConfig(uint32_t sensorId, h1_sensor_config_t* config);
    
    int                         startCapture(uint32_t streamMask);
    int                         stopCapture(void);
    int                         captureFrame(h1_frame_t* frame, uint32_t timeout);
    
    int                         setISPParams(h1_isp_params_t* params);
    int                         getISPParams(h1_isp_params_t* params);
    
    int                         setFocus(uint32_t sensorId, uint32_t position);
    int                         setExposure(uint32_t sensorId, uint32_t exposure, uint32_t gain);
    int                         setWhiteBalance(uint32_t sensorId, uint32_t red, uint32_t green, uint32_t blue);
    
    int                         getStatistics(h1_camera_stats_t* stats);
    int                         resetStatistics(void);
    
    /* Secure camera API */
    int                         enterSecureMode(uint64_t sessionId, uint8_t* key, uint32_t keyLen);
    int                         exitSecureMode(void);
    bool                        isSecureModeEnabled(void);
};

OSDefineMetaClassAndStructors(AppleH1CameraInterface, IOService)

/*==============================================================================
 * AppleH1CameraUserClient - User client for camera access
 *==============================================================================*/

class AppleH1CameraUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(AppleH1CameraUserClient)
    
private:
    task_t                      fTask;
    pid_t                       fPid;
    AppleH1CameraInterface*     fProvider;
    uint32_t                    fPermissions;
    uint32_t                    fState;
    uint32_t                    fStreamMask;
    
    IOMemoryDescriptor*         fSharedMemory;
    void*                       fSharedBuffer;
    uint32_t                    fSharedBufferSize;
    
    bool                        fValid;
    
protected:
    bool                        init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                        free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                        start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                        stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    /* IOUserClient overrides */
    IOReturn                    clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn                    clientDied(void) APPLE_KEXT_OVERRIDE;
    
    IOReturn                    registerNotificationPort(mach_port_t port, UInt32 type,
                                                          UInt32 refCon) APPLE_KEXT_OVERRIDE;
    
    IOReturn                    externalMethod(uint32_t selector, IOExternalMethodArguments* arguments,
                                                IOExternalMethodDispatch* dispatch,
                                                OSObject* target,
                                                void* reference) APPLE_KEXT_OVERRIDE;
    
    /* External methods */
    static IOReturn             sGetInfo(AppleH1CameraUserClient* target,
                                          void* reference,
                                          IOExternalMethodArguments* args);
    
    static IOReturn             sGetSensorCaps(AppleH1CameraUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args);
    
    static IOReturn             sConfigureSensor(AppleH1CameraUserClient* target,
                                                  void* reference,
                                                  IOExternalMethodArguments* args);
    
    static IOReturn             sStartCapture(AppleH1CameraUserClient* target,
                                               void* reference,
                                               IOExternalMethodArguments* args);
    
    static IOReturn             sStopCapture(AppleH1CameraUserClient* target,
                                              void* reference,
                                              IOExternalMethodArguments* args);
    
    static IOReturn             sCaptureFrame(AppleH1CameraUserClient* target,
                                               void* reference,
                                               IOExternalMethodArguments* args);
    
    static IOReturn             sSetFocus(AppleH1CameraUserClient* target,
                                           void* reference,
                                           IOExternalMethodArguments* args);
    
    static IOReturn             sSetExposure(AppleH1CameraUserClient* target,
                                              void* reference,
                                              IOExternalMethodArguments* args);
    
    static IOReturn             sSetWhiteBalance(AppleH1CameraUserClient* target,
                                                  void* reference,
                                                  IOExternalMethodArguments* args);
    
    static IOReturn             sSetISPParams(AppleH1CameraUserClient* target,
                                               void* reference,
                                               IOExternalMethodArguments* args);
    
    static IOReturn             sEnterSecureMode(AppleH1CameraUserClient* target,
                                                  void* reference,
                                                  IOExternalMethodArguments* args);
    
    static IOReturn             sExitSecureMode(AppleH1CameraUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args);
    
    static IOReturn             sGetStatistics(AppleH1CameraUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args);
};

OSDefineMetaClassAndStructors(AppleH1CameraUserClient, IOUserClient)

/*==============================================================================
 * External Method Dispatch Table
 *==============================================================================*/

enum {
    kMethodGetInfo,
    kMethodGetSensorCaps,
    kMethodConfigureSensor,
    kMethodStartCapture,
    kMethodStopCapture,
    kMethodCaptureFrame,
    kMethodSetFocus,
    kMethodSetExposure,
    kMethodSetWhiteBalance,
    kMethodSetISPParams,
    kMethodEnterSecureMode,
    kMethodExitSecureMode,
    kMethodGetStatistics,
    kMethodCount
};

static IOExternalMethodDispatch sMethods[kMethodCount] = {
    {   /* kMethodGetInfo */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sGetInfo,
        0, 0, 0, 1
    },
    {   /* kMethodGetSensorCaps */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sGetSensorCaps,
        1, 0, 0, 1
    },
    {   /* kMethodConfigureSensor */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sConfigureSensor,
        1, 1, 0, 0
    },
    {   /* kMethodStartCapture */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sStartCapture,
        1, 0, 0, 0
    },
    {   /* kMethodStopCapture */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sStopCapture,
        0, 0, 0, 0
    },
    {   /* kMethodCaptureFrame */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sCaptureFrame,
        1, 0, 0, 1
    },
    {   /* kMethodSetFocus */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sSetFocus,
        2, 0, 0, 0
    },
    {   /* kMethodSetExposure */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sSetExposure,
        3, 0, 0, 0
    },
    {   /* kMethodSetWhiteBalance */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sSetWhiteBalance,
        4, 0, 0, 0
    },
    {   /* kMethodSetISPParams */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sSetISPParams,
        0, 1, 0, 0
    },
    {   /* kMethodEnterSecureMode */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sEnterSecureMode,
        2, 0, 0, 0
    },
    {   /* kMethodExitSecureMode */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sExitSecureMode,
        0, 0, 0, 0
    },
    {   /* kMethodGetStatistics */
        (IOExternalMethodAction)&AppleH1CameraUserClient::sGetStatistics,
        0, 0, 0, 1
    }
};

/*==============================================================================
 * AppleH1CameraInterface Implementation
 *==============================================================================*/

#pragma mark - AppleH1CameraInterface::Initialization

bool AppleH1CameraInterface::init(OSDictionary* dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    
    /* Initialize version information */
    fDriverVersion = APPLE_H1_CAMERA_VERSION;
    fDriverRevision = APPLE_H1_CAMERA_REVISION;
    strlcpy(fDriverBuild, __DATE__ " " __TIME__, sizeof(fDriverBuild));
    
    /* Initialize state */
    fState = 0;
    fFlags = 0;
    fStartTime = 0;
    
    /* Create lock group */
    fLockGroup = lck_grp_alloc_init("AppleH1Camera", LCK_GRP_ATTR_NULL);
    if (!fLockGroup) {
        return false;
    }
    
    fLockAttr = lck_attr_alloc_init();
    if (!fLockAttr) {
        lck_grp_free(fLockGroup);
        return false;
    }
    
    /* Initialize locks */
    fSensorLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fStreamLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fFrameLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fISPLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fClientLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fEventLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    
    if (!fSensorLock || !fStreamLock || !fFrameLock || 
        !fISPLock || !fClientLock || !fEventLock) {
        return false;
    }
    
    /* Initialize sensor data */
    bzero(fSensorCaps, sizeof(fSensorCaps));
    bzero(fSensorConfigs, sizeof(fSensorConfigs));
    fNumSensors = 0;
    fActiveSensor = 0;
    
    /* Initialize streams */
    bzero(fStreams, sizeof(fStreams));
    fNumStreams = 0;
    fActiveStreams = 0;
    
    /* Initialize frame buffers */
    bzero(fFrames, sizeof(fFrames));
    fFrameCount = 0;
    fFrameHead = 0;
    fFrameTail = 0;
    fFramePending = 0;
    
    /* Initialize ISP */
    bzero(&fISPParams, sizeof(fISPParams));
    fISPEnabled = false;
    
    /* Initialize power management */
    fPowerState = H1_POWER_OFF;
    fThermalLevel = 0;
    fVoltage = 0;
    fCurrent = 0;
    fLastPowerSample = 0;
    
    /* Initialize statistics */
    bzero(&fStats, sizeof(fStats));
    
    /* Initialize security */
    fSecureMode = false;
    bzero(fSecureKey, sizeof(fSecureKey));
    fSecureSessionId = 0;
    
    /* Initialize client management */
    fExclusiveClient = NULL;
    fClientCount = 0;
    
    /* Initialize event queue */
    fEventQueueMax = 64;
    fEventQueue = (h1_camera_event_t**)IOMalloc(sizeof(h1_camera_event_t*) * fEventQueueMax);
    if (!fEventQueue) {
        return false;
    }
    bzero(fEventQueue, sizeof(h1_camera_event_t*) * fEventQueueMax);
    fEventQueueHead = 0;
    fEventQueueTail = 0;
    fEventQueueSize = 0;
    
    return true;
}

void AppleH1CameraInterface::free(void)
{
    /* Free frame buffers */
    freeFrameBuffers();
    
    /* Free event queue */
    if (fEventQueue) {
        for (int i = 0; i < fEventQueueMax; i++) {
            if (fEventQueue[i]) {
                IOFree(fEventQueue[i], sizeof(h1_camera_event_t) + fEventQueue[i]->data_size);
            }
        }
        IOFree(fEventQueue, sizeof(h1_camera_event_t*) * fEventQueueMax);
        fEventQueue = NULL;
    }
    
    /* Free DMA buffer */
    if (fDMABuffer) {
        fDMABuffer->release();
        fDMABuffer = NULL;
    }
    
    /* Free locks */
    if (fSensorLock) lck_mtx_free(fSensorLock, fLockGroup);
    if (fStreamLock) lck_mtx_free(fStreamLock, fLockGroup);
    if (fFrameLock) lck_mtx_free(fFrameLock, fLockGroup);
    if (fISPLock) lck_mtx_free(fISPLock, fLockGroup);
    if (fClientLock) lck_mtx_free(fClientLock, fLockGroup);
    if (fEventLock) lck_mtx_free(fEventLock, fLockGroup);
    
    if (fLockAttr) lck_attr_free(fLockAttr);
    if (fLockGroup) lck_grp_free(fLockGroup);
    
    super::free();
}

bool AppleH1CameraInterface::createWorkLoop(void)
{
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        IOLog("AppleH1Camera: Failed to create work loop\n");
        return false;
    }
    
    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        IOLog("AppleH1Camera: Failed to create command gate\n");
        fWorkLoop->release();
        fWorkLoop = NULL;
        return false;
    }
    
    fWorkLoop->addEventSource(fCommandGate);
    
    fTimerSource = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &AppleH1CameraInterface::timerFired));
    
    if (!fTimerSource) {
        IOLog("AppleH1Camera: Failed to create timer source\n");
        fWorkLoop->removeEventSource(fCommandGate);
        fCommandGate->release();
        fCommandGate = NULL;
        fWorkLoop->release();
        fWorkLoop = NULL;
        return false;
    }
    
    fWorkLoop->addEventSource(fTimerSource);
    
    /* Create interrupt source for H1 interrupts */
    fInterruptSource = IOInterruptEventSource::interruptEventSource(this,
        OSMemberFunctionCast(IOInterruptEventAction, this,
                             &AppleH1CameraInterface::interruptHandler),
        fH1Service, 0);
    
    if (fInterruptSource) {
        fWorkLoop->addEventSource(fInterruptSource);
    }
    
    return true;
}

void AppleH1CameraInterface::destroyWorkLoop(void)
{
    if (fWorkLoop) {
        if (fInterruptSource) {
            fWorkLoop->removeEventSource(fInterruptSource);
            fInterruptSource->release();
            fInterruptSource = NULL;
        }
        
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

#pragma mark - AppleH1CameraInterface::Hardware Initialization

bool AppleH1CameraInterface::initializeHardware(void)
{
    IORegistryEntry* h1Entry;
    
    /* Find H1 bridge service */
    h1Entry = IORegistryEntry::fromPath("/AppleH1", gIODTPlane);
    if (!h1Entry) {
        h1Entry = IORegistryEntry::fromPath("/iBridge", gIODTPlane);
    }
    
    if (!h1Entry) {
        IOLog("AppleH1Camera: H1 bridge not found\n");
        return false;
    }
    
    fH1Service = OSDynamicCast(IOService, h1Entry);
    if (!fH1Service) {
        IOLog("AppleH1Camera: H1 service not available\n");
        h1Entry->release();
        return false;
    }
    
    /* Get register memory */
    OSData* regData = OSDynamicCast(OSData, fH1Service->getProperty("reg"));
    if (regData && regData->getLength() >= 8) {
        uint64_t physAddr = *(uint64_t*)regData->getBytesNoCopy();
        uint64_t regSize = *(uint64_t*)((uint8_t*)regData->getBytesNoCopy() + 8);
        
        fH1MemoryMap = fH1Service->mapDeviceMemoryWithIndex(0);
        if (fH1MemoryMap) {
            fH1Registers = (volatile uint32_t*)fH1MemoryMap->getVirtualAddress();
            IOLog("AppleH1Camera: H1 registers mapped at %p\n", fH1Registers);
        }
    }
    
    h1Entry->release();
    
    /* Enumerate camera sensors */
    enumerateSensors();
    
    /* Initialize ISP */
    bzero(&fISPParams, sizeof(fISPParams));
    fISPParams.sharpness = 50;
    fISPParams.contrast = 50;
    fISPParams.brightness = 50;
    fISPParams.saturation = 50;
    fISPParams.gamma = 220;  /* ~2.2 gamma */
    fISPParams.black_level = 0;
    fISPParams.white_level = 1023;  /* 10-bit */
    fISPParams.color_temp = 5000;    /* Daylight */
    fISPEnabled = false;
    
    /* Allocate DMA buffer for frame transfer */
    fDMASize = H1_MAX_FRAME_SIZE * 2;  /* Double buffer */
    fDMABuffer = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        fDMASize, PAGE_SIZE);
    
    if (!fDMABuffer) {
        IOLog("AppleH1Camera: Failed to allocate DMA buffer\n");
        return false;
    }
    
    fDMAPhysAddr = fDMABuffer->getPhysicalSegment(0, NULL);
    fDMAPosition = 0;
    
    /* Configure MIPI CSI-2 interface */
    configureMIPI(H1_MIPI_LANES_4, 800000000);  /* 4 lanes, 800 MHz */
    
    fStartTime = mach_absolute_time();
    
    return true;
}

void AppleH1CameraInterface::shutdownHardware(void)
{
    /* Stop all streams */
    stopCapture();
    
    /* Power off all sensors */
    for (int i = 0; i < fNumSensors; i++) {
        powerOffSensor(i);
    }
    
    /* Stop MIPI interface */
    stopMIPI();
    
    /* Disable ISP */
    enableISP(false);
    
    /* Release H1 registers */
    if (fH1MemoryMap) {
        fH1MemoryMap->release();
        fH1MemoryMap = NULL;
        fH1Registers = NULL;
    }
    
    /* Release DMA buffer */
    if (fDMABuffer) {
        fDMABuffer->release();
        fDMABuffer = NULL;
    }
}

uint32_t AppleH1CameraInterface::readH1Register(uint32_t offset)
{
    if (!fH1Registers) {
        return 0xFFFFFFFF;
    }
    
    return fH1Registers[offset / 4];
}

void AppleH1CameraInterface::writeH1Register(uint32_t offset, uint32_t value)
{
    if (!fH1Registers) {
        return;
    }
    
    fH1Registers[offset / 4] = value;
    
    /* Memory barrier */
    OSMemoryBarrier();
}

bool AppleH1CameraInterface::waitForH1Bit(uint32_t offset, uint32_t bit, bool set, uint32_t timeout)
{
    uint32_t value;
    uint32_t ticks = 0;
    uint32_t delay = 10;  /* 10 us */
    
    while (ticks < (timeout * 1000)) {
        value = readH1Register(offset);
        
        if (set) {
            if (value & bit) return true;
        } else {
            if (!(value & bit)) return true;
        }
        
        IODelay(delay);
        ticks += delay;
    }
    
    return false;
}

#pragma mark - AppleH1CameraInterface::Timer and Interrupt Handlers

void AppleH1CameraInterface::timerFired(void)
{
    /* Update statistics */
    fStats.frames_captured = fFrameCount;
    fStats.frames_dropped = 0;  /* Would track dropped frames */
    fStats.frames_error = 0;
    fStats.bytes_transferred = fStats.frames_captured * 1920 * 1080 * 2;  /* Approx */
    
    /* Sample power data */
    samplePowerData();
    
    /* Process events */
    processEvents();
    
    /* Reschedule timer */
    fTimerSource->setTimeoutMS(1000);  /* 1 second */
}

void AppleH1CameraInterface::timerFired(OSObject* owner, IOTimerEventSource* sender)
{
    AppleH1CameraInterface* me = OSDynamicCast(AppleH1CameraInterface, owner);
    if (me) {
        me->timerFired();
    }
}

void AppleH1CameraInterface::interruptHandler(void)
{
    uint32_t intStatus;
    
    if (!fH1Registers) {
        return;
    }
    
    /* Read interrupt status */
    intStatus = readH1Register(H1_REG_CAMERA_INT_STATUS);
    
    /* Clear interrupts */
    writeH1Register(H1_REG_CAMERA_INT_STATUS, intStatus);
    
    /* Handle frame ready interrupt */
    if (intStatus & H1_INT_FRAME_READY) {
        /* Frame is ready in DMA buffer */
        h1_frame_t* frame;
        
        lck_mtx_lock(fFrameLock);
        
        if (fFramePending < H1_MAX_FRAME_BUFFERS) {
            frame = fFrames[fFrameHead];
            if (frame) {
                frame->timestamp = mach_absolute_time();
                frame->flags |= H1_FRAME_FLAG_VALID;
                
                fFrameHead = (fFrameHead + 1) % H1_MAX_FRAME_BUFFERS;
                fFramePending++;
                
                /* Queue event */
                queueEvent(0x01, frame->sensor_id, frame, sizeof(h1_frame_t));
            }
        }
        
        lck_mtx_unlock(fFrameLock);
    }
    
    /* Handle frame error */
    if (intStatus & H1_INT_FRAME_ERROR) {
        fStats.frames_error++;
        queueEvent(0x02, 0, NULL, 0);
    }
    
    /* Handle autofocus complete */
    if (intStatus & H1_INT_AF_COMPLETE) {
        queueEvent(0x10, fActiveSensor, NULL, 0);
    }
    
    /* Handle thermal warning */
    if (intStatus & H1_INT_THERMAL_WARNING) {
        fStats.thermal_warnings++;
        handleThermalEvent();
    }
    
    /* Handle thermal shutdown */
    if (intStatus & H1_INT_THERMAL_SHUTDOWN) {
        stopCapture();
        queueEvent(0x20, 0, NULL, 0);
    }
    
    /* Handle security violation */
    if (intStatus & H1_INT_SECURE_VIOLATION) {
        fStats.secure_violations++;
        disableSecureMode();
        queueEvent(0x30, 0, NULL, 0);
    }
}

void AppleH1CameraInterface::interruptHandler(OSObject* owner, IOInterruptEventSource* sender, int count)
{
    AppleH1CameraInterface* me = OSDynamicCast(AppleH1CameraInterface, owner);
    if (me) {
        me->interruptHandler();
    }
}

#pragma mark - AppleH1CameraInterface::Sensor Management

int AppleH1CameraInterface::enumerateSensors(void)
{
    /* In real implementation, would query H1 for connected sensors */
    
    /* Mock data for built-in iSight camera */
    strlcpy(fSensorCaps[0].name, "Apple iSight Camera", sizeof(fSensorCaps[0].name));
    fSensorCaps[0].sensor_type = kH1SensorTypeRaw;
    fSensorCaps[0].pixel_format = H1_PIXEL_FMT_RAW10;
    fSensorCaps[0].bit_depth = 10;
    fSensorCaps[0].max_width = 4608;
    fSensorCaps[0].max_height = 2592;
    fSensorCaps[0].min_width = 640;
    fSensorCaps[0].min_height = 480;
    fSensorCaps[0].supported_fps[0] = 30;
    fSensorCaps[0].supported_fps[1] = 60;
    fSensorCaps[0].supported_fps[2] = 120;
    fSensorCaps[0].num_fps = 3;
    fSensorCaps[0].exposure_min = 100;
    fSensorCaps[0].exposure_max = 100000;
    fSensorCaps[0].gain_min = 100;
    fSensorCaps[0].gain_max = 16000;
    fSensorCaps[0].iso_min = 100;
    fSensorCaps[0].iso_max = 3200;
    fSensorCaps[0].has_autofocus = true;
    fSensorCaps[0].has_autoexposure = true;
    fSensorCaps[0].has_autowhitebalance = true;
    fSensorCaps[0].has_flash = false;
    fSensorCaps[0].has_torch = false;
    fSensorCaps[0].has_hdr = true;
    fSensorCaps[0].has_eis = true;
    fSensorCaps[0].has_ois = false;
    fSensorCaps[0].has_secure_mode = true;
    fSensorCaps[0].capabilities = 0;
    
    fNumSensors = 1;
    
    IOLog("AppleH1Camera: Found %d camera sensor(s)\n", fNumSensors);
    
    return kH1Success;
}

int AppleH1CameraInterface::powerOnSensor(uint32_t sensorId)
{
    if (sensorId >= fNumSensors) {
        return kH1ErrInvalidParam;
    }
    
    /* Power on sequence via H1 */
    writeH1Register(H1_REG_CAMERA_SENSOR_SELECT, sensorId);
    writeH1Register(H1_REG_CAMERA_CONTROL, H1_CTRL_POWER_UP);
    
    /* Wait for power up */
    if (!waitForH1Bit(H1_REG_CAMERA_STATUS, H1_STATUS_POWERED, true, 100)) {
        return kH1ErrTimeout;
    }
    
    return kH1Success;
}

int AppleH1CameraInterface::powerOffSensor(uint32_t sensorId)
{
    if (sensorId >= fNumSensors) {
        return kH1ErrInvalidParam;
    }
    
    writeH1Register(H1_REG_CAMERA_SENSOR_SELECT, sensorId);
    writeH1Register(H1_REG_CAMERA_CONTROL, H1_CTRL_POWER_DOWN);
    
    return kH1Success;
}

int AppleH1CameraInterface::configureSensor(uint32_t sensorId, h1_sensor_config_t* config)
{
    if (sensorId >= fNumSensors || !config) {
        return kH1ErrInvalidParam;
    }
    
    lck_mtx_lock(fSensorLock);
    
    /* Validate configuration */
    if (config->width > fSensorCaps[sensorId].max_width ||
        config->height > fSensorCaps[sensorId].max_height) {
        lck_mtx_unlock(fSensorLock);
        return kH1ErrInvalidParam;
    }
    
    /* Store configuration */
    memcpy(&fSensorConfigs[sensorId], config, sizeof(h1_sensor_config_t));
    
    /* Send configuration to sensor via I2C/CCI */
    /* In real implementation, would write to sensor registers */
    
    lck_mtx_unlock(fSensorLock);
    
    return kH1Success;
}

int AppleH1CameraInterface::startSensor(uint32_t sensorId)
{
    if (sensorId >= fNumSensors) {
        return kH1ErrInvalidParam;
    }
    
    writeH1Register(H1_REG_CAMERA_SENSOR_SELECT, sensorId);
    
    /* Configure MIPI based on sensor config */
    configureMIPI(H1_MIPI_LANES_4, 800000000);
    
    /* Start MIPI receiver */
    startMIPI();
    
    /* Enable ISP if configured */
    if (fSensorConfigs[sensorId].stream_mask & kH1StreamTypeCapture) {
        enableISP(true);
    }
    
    return kH1Success;
}

int AppleH1CameraInterface::stopSensor(uint32_t sensorId)
{
    if (sensorId >= fNumSensors) {
        return kH1ErrInvalidParam;
    }
    
    writeH1Register(H1_REG_CAMERA_SENSOR_SELECT, sensorId);
    writeH1Register(H1_REG_CAMERA_CONTROL, H1_CTRL_STOP_STREAM);
    
    return kH1Success;
}

#pragma mark - AppleH1CameraInterface::MIPI Interface

int AppleH1CameraInterface::configureMIPI(uint32_t lanes, uint32_t clock)
{
    uint32_t mipiCtrl = 0;
    
    /* Configure number of lanes */
    mipiCtrl |= (lanes & 0x3) << 0;
    
    /* Configure clock (simplified) */
    mipiCtrl |= (clock / 1000000) << 8;
    
    writeH1Register(H1_REG_CAMERA_MIPI_CONTROL, mipiCtrl);
    
    return kH1Success;
}

int AppleH1CameraInterface::startMIPI(void)
{
    uint32_t mipiCtrl;
    
    mipiCtrl = readH1Register(H1_REG_CAMERA_MIPI_CONTROL);
    mipiCtrl |= (1 << 31);  /* Enable */
    writeH1Register(H1_REG_CAMERA_MIPI_CONTROL, mipiCtrl);
    
    /* Wait for PHY lock */
    if (!waitForH1Bit(H1_REG_CAMERA_CSI2_PHY_STATUS, (1 << 0), true, 100)) {
        return kH1ErrTimeout;
    }
    
    return kH1Success;
}

int AppleH1CameraInterface::stopMIPI(void)
{
    uint32_t mipiCtrl;
    
    mipiCtrl = readH1Register(H1_REG_CAMERA_MIPI_CONTROL);
    mipiCtrl &= ~(1 << 31);  /* Disable */
    writeH1Register(H1_REG_CAMERA_MIPI_CONTROL, mipiCtrl);
    
    return kH1Success;
}

#pragma mark - AppleH1CameraInterface::ISP Management

int AppleH1CameraInterface::configureISP(h1_isp_params_t* params)
{
    if (!params) {
        return kH1ErrInvalidParam;
    }
    
    lck_mtx_lock(fISPLock);
    memcpy(&fISPParams, params, sizeof(h1_isp_params_t));
    lck_mtx_unlock(fISPLock);
    
    /* Write ISP parameters to H1 */
    writeH1Register(H1_REG_CAMERA_ISP_PARAM_ADDR, (uint32_t)(uintptr_t)params);
    writeH1Register(H1_REG_CAMERA_ISP_PARAM_SIZE, sizeof(h1_isp_params_t));
    
    return kH1Success;
}

int AppleH1CameraInterface::enableISP(bool enable)
{
    if (enable) {
        writeH1Register(H1_REG_CAMERA_ISP_CONTROL, (1 << 0));
    } else {
        writeH1Register(H1_REG_CAMERA_ISP_CONTROL, 0);
    }
    
    fISPEnabled = enable;
    
    return kH1Success;
}

int AppleH1CameraInterface::processISP(h1_frame_t* input, h1_frame_t* output)
{
    if (!input || !output) {
        return kH1ErrInvalidParam;
    }
    
    /* Configure ISP DMA */
    writeH1Register(H1_REG_CAMERA_DMA_ADDR, (uint32_t)(input->data - (uint8_t*)fDMABuffer->getBytesNoCopy()));
    writeH1Register(H1_REG_CAMERA_DMA_SIZE, input->data_size);
    writeH1Register(H1_REG_CAMERA_DMA_CONTROL, (1 << 0));  /* Start processing */
    
    /* Wait for completion */
    if (!waitForH1Bit(H1_REG_CAMERA_DMA_STATUS, (1 << 0), false, 100)) {
        return kH1ErrTimeout;
    }
    
    /* Copy processed data to output */
    memcpy(output->data, fDMABuffer->getBytesNoCopy(), output->data_size);
    
    return kH1Success;
}

#pragma mark - AppleH1CameraInterface::Frame Buffer Management

int AppleH1CameraInterface::allocateFrameBuffers(uint32_t count, uint32_t size)
{
    if (count > H1_MAX_FRAME_BUFFERS || size > H1_MAX_FRAME_SIZE) {
        return kH1ErrInvalidParam;
    }
    
    lck_mtx_lock(fFrameLock);
    
    for (int i = 0; i < count; i++) {
        fFrames[i] = (h1_frame_t*)IOMalloc(sizeof(h1_frame_t));
        if (!fFrames[i]) {
            /* Clean up already allocated */
            for (int j = 0; j < i; j++) {
                if (fFrames[j]->data) {
                    IOFree(fFrames[j]->data, fFrames[j]->data_size);
                }
                IOFree(fFrames[j], sizeof(h1_frame_t));
                fFrames[j] = NULL;
            }
            lck_mtx_unlock(fFrameLock);
            return kH1ErrNoMemory;
        }
        
        bzero(fFrames[i], sizeof(h1_frame_t));
        
        fFrames[i]->data = (uint8_t*)IOMalloc(size);
        if (!fFrames[i]->data) {
            IOFree(fFrames[i], sizeof(h1_frame_t));
            fFrames[i] = NULL;
            lck_mtx_unlock(fFrameLock);
            return kH1ErrNoMemory;
        }
        
        fFrames[i]->data_size = size;
        fFrames[i]->metadata = (uint8_t*)IOMalloc(H1_MAX_METADATA_SIZE);
        if (fFrames[i]->metadata) {
            fFrames[i]->metadata_size = H1_MAX_METADATA_SIZE;
        }
    }
    
    fFrameCount = count;
    fFrameHead = 0;
    fFrameTail = 0;
    fFramePending = 0;
    
    lck_mtx_unlock(fFrameLock);
    
    return kH1Success;
}

void AppleH1CameraInterface::freeFrameBuffers(void)
{
    lck_mtx_lock(fFrameLock);
    
    for (int i = 0; i < H1_MAX_FRAME_BUFFERS; i++) {
        if (fFrames[i]) {
            if (fFrames[i]->data) {
                IOFree(fFrames[i]->data, fFrames[i]->data_size);
            }
            if (fFrames[i]->metadata) {
                IOFree(fFrames[i]->metadata, fFrames[i]->metadata_size);
            }
            IOFree(fFrames[i], sizeof(h1_frame_t));
            fFrames[i] = NULL;
        }
    }
    
    fFrameCount = 0;
    fFrameHead = 0;
    fFrameTail = 0;
    fFramePending = 0;
    
    lck_mtx_unlock(fFrameLock);
}

int AppleH1CameraInterface::queueFrame(uint32_t streamId, h1_frame_t* frame)
{
    /* Queue frame for client consumption */
    
    lck_mtx_lock(fFrameLock);
    
    /* Add to tail of queue */
    /* Implementation would copy frame to client buffer */
    
    fFrameTail = (fFrameTail + 1) % H1_MAX_FRAME_BUFFERS;
    fFramePending++;
    
    lck_mtx_unlock(fFrameLock);
    
    return kH1Success;
}

int AppleH1CameraInterface::dequeueFrame(h1_frame_t** frame)
{
    if (!frame) {
        return kH1ErrInvalidParam;
    }
    
    lck_mtx_lock(fFrameLock);
    
    if (fFramePending == 0) {
        lck_mtx_unlock(fFrameLock);
        return kH1ErrNoData;
    }
    
    *frame = fFrames[fFrameTail];
    fFrameTail = (fFrameTail + 1) % H1_MAX_FRAME_BUFFERS;
    fFramePending--;
    
    lck_mtx_unlock(fFrameLock);
    
    return kH1Success;
}

int AppleH1CameraInterface::processFrame(h1_frame_t* frame)
{
    if (!frame) {
        return kH1ErrInvalidParam;
    }
    
    /* Validate frame */
    if (!(frame->flags & H1_FRAME_FLAG_VALID)) {
        return kH1ErrInvalidParam;
    }
    
    /* Check for errors */
    if (frame->flags & H1_FRAME_FLAG_ERROR) {
        fStats.frames_error++;
        return kH1ErrGeneral;
    }
    
    /* Update statistics */
    fStats.frames_captured++;
    fStats.bytes_transferred += frame->data_size;
    
    /* Process with ISP if enabled */
    if (fISPEnabled) {
        /* Would process frame through ISP */
    }
    
    /* Check security mode */
    if (fSecureMode) {
        if (!validateSecureFrame(frame)) {
            fStats.secure_violations++;
            return kH1ErrSecureViolation;
        }
    }
    
    return kH1Success;
}

#pragma mark - AppleH1CameraInterface::Stream Management

int AppleH1CameraInterface::createStream(h1_stream_config_t* config, uint32_t* streamId)
{
    if (!config || !streamId) {
        return kH1ErrInvalidParam;
    }
    
    lck_mtx_lock(fStreamLock);
    
    if (fNumStreams >= H1_MAX_STREAMS) {
        lck_mtx_unlock(fStreamLock);
        return kH1ErrBusy;
    }
    
    /* Find free stream ID */
    for (int i = 0; i < H1_MAX_STREAMS; i++) {
        if (fStreams[i].stream_id == 0) {
            memcpy(&fStreams[i], config, sizeof(h1_stream_config_t));
            fStreams[i].stream_id = i + 1;
            fNumStreams++;
            *streamId = fStreams[i].stream_id;
            break;
        }
    }
    
    lck_mtx_unlock(fStreamLock);
    
    return kH1Success;
}

int AppleH1CameraInterface::destroyStream(uint32_t streamId)
{
    lck_mtx_lock(fStreamLock);
    
    for (int i = 0; i < H1_MAX_STREAMS; i++) {
        if (fStreams[i].stream_id == streamId) {
            bzero(&fStreams[i], sizeof(h1_stream_config_t));
            fNumStreams--;
            break;
        }
    }
    
    lck_mtx_unlock(fStreamLock);
    
    return kH1Success;
}

int AppleH1CameraInterface::startStream(uint32_t streamId)
{
    h1_stream_config_t* stream = NULL;
    
    /* Find stream */
    for (int i = 0; i < H1_MAX_STREAMS; i++) {
        if (fStreams[i].stream_id == streamId) {
            stream = &fStreams[i];
            break;
        }
    }
    
    if (!stream) {
        return kH1ErrInvalidParam;
    }
    
    /* Start sensor if not already active */
    if (!(fActiveStreams & (1 << stream->sensor_id))) {
        startSensor(stream->sensor_id);
    }
    
    /* Configure DMA for this stream */
    writeH1Register(H1_REG_CAMERA_DMA_ADDR, fDMAPhysAddr);
    writeH1Register(H1_REG_CAMERA_DMA_SIZE, stream->buffer_size);
    
    /* Start streaming */
    writeH1Register(H1_REG_CAMERA_SENSOR_SELECT, stream->sensor_id);
    writeH1Register(H1_REG_CAMERA_CONTROL, H1_CTRL_START_STREAM);
    
    fActiveStreams |= (1 << streamId);
    
    return kH1Success;
}

int AppleH1CameraInterface::stopStream(uint32_t streamId)
{
    writeH1Register(H1_REG_CAMERA_CONTROL, H1_CTRL_STOP_STREAM);
    
    fActiveStreams &= ~(1 << streamId);
    
    return kH1Success;
}

#pragma mark - AppleH1CameraInterface::Power Management

void AppleH1CameraInterface::updatePowerState(uint32_t newState)
{
    uint32_t oldState = fPowerState;
    
    if (oldState == newState) {
        return;
    }
    
    IOLog("AppleH1Camera: Power state change: %u -> %u\n", oldState, newState);
    
    switch (newState) {
        case H1_POWER_OFF:
            shutdownHardware();
            break;
            
        case H1_POWER_SLEEP:
            writeH1Register(H1_REG_CAMERA_POWER_STATE, 1);
            break;
            
        case H1_POWER_IDLE:
            writeH1Register(H1_REG_CAMERA_POWER_STATE, 2);
            break;
            
        case H1_POWER_ACTIVE:
            writeH1Register(H1_REG_CAMERA_POWER_STATE, 3);
            break;
    }
    
    fPowerState = newState;
}

void AppleH1CameraInterface::samplePowerData(void)
{
    uint32_t raw;
    
    raw = readH1Register(H1_REG_CAMERA_VOLTAGE_STATUS);
    fVoltage = raw;
    
    raw = readH1Register(H1_REG_CAMERA_CURRENT_STATUS);
    fCurrent = raw;
    
    raw = readH1Register(H1_REG_CAMERA_THERMAL_STATUS);
    fThermalLevel = raw & 0xFF;
    
    fStats.power_consumption = fVoltage * fCurrent / 1000;
    
    if (fThermalLevel > fStats.temperature_peak) {
        fStats.temperature_peak = fThermalLevel;
    }
    
    fLastPowerSample = mach_absolute_time();
}

void AppleH1CameraInterface::handleThermalEvent(void)
{
    if (fThermalLevel > 85) {
        /* Critical temperature - reduce framerate */
        for (int i = 0; i < H1_MAX_STREAMS; i++) {
            if (fStreams[i].stream_id != 0) {
                if (fStreams[i].fps > 30) {
                    fStreams[i].fps = 30;
                }
            }
        }
    } else if (fThermalLevel > 75) {
        /* High temperature - throttle */
        for (int i = 0; i < H1_MAX_STREAMS; i++) {
            if (fStreams[i].stream_id != 0) {
                if (fStreams[i].fps > 60) {
                    fStreams[i].fps = 60;
                }
            }
        }
    }
}

#pragma mark - AppleH1CameraInterface::Security

int AppleH1CameraInterface::enableSecureMode(uint64_t sessionId, uint8_t* key)
{
    if (fSecureMode) {
        return kH1ErrBusy;
    }
    
    fSecureSessionId = sessionId;
    memcpy(fSecureKey, key, 32);
    
    /* Enable secure mode in H1 */
    writeH1Register(H1_REG_CAMERA_SECURE_CONTROL, (1 << 0));
    
    if (!waitForH1Bit(H1_REG_CAMERA_SECURE_STATUS, (1 << 0), true, 100)) {
        return kH1ErrTimeout;
    }
    
    fSecureMode = true;
    
    return kH1Success;
}

int AppleH1CameraInterface::disableSecureMode(void)
{
    if (!fSecureMode) {
        return kH1Success;
    }
    
    writeH1Register(H1_REG_CAMERA_SECURE_CONTROL, 0);
    
    fSecureMode = false;
    bzero(fSecureKey, sizeof(fSecureKey));
    fSecureSessionId = 0;
    
    return kH1Success;
}

bool AppleH1CameraInterface::validateSecureFrame(h1_frame_t* frame)
{
    /* In real implementation, would verify cryptographic signature */
    return true;
}

#pragma mark - AppleH1CameraInterface::Event Management

int AppleH1CameraInterface::queueEvent(uint32_t eventType, uint32_t sensorId, void* data, uint32_t dataSize)
{
    h1_camera_event_t* event;
    uint32_t totalSize;
    uint32_t nextTail;
    
    totalSize = sizeof(h1_camera_event_t) + dataSize;
    
    event = (h1_camera_event_t*)IOMalloc(totalSize);
    if (!event) {
        return kH1ErrNoMemory;
    }
    
    bzero(event, totalSize);
    
    event->event_id = (uint32_t)(mach_absolute_time() & 0xFFFFFFFF);
    event->event_type = eventType;
    event->timestamp = mach_absolute_time();
    event->sensor_id = sensorId;
    event->data_size = dataSize;
    
    if (data && dataSize > 0) {
        memcpy(event->data, data, dataSize);
    }
    
    lck_mtx_lock(fEventLock);
    
    nextTail = (fEventQueueTail + 1) % fEventQueueMax;
    
    if (nextTail == fEventQueueHead) {
        /* Queue full - drop oldest */
        if (fEventQueue[fEventQueueHead]) {
            IOFree(fEventQueue[fEventQueueHead], 
                   sizeof(h1_camera_event_t) + fEventQueue[fEventQueueHead]->data_size);
            fEventQueue[fEventQueueHead] = NULL;
        }
        fEventQueueHead = (fEventQueueHead + 1) % fEventQueueMax;
        fEventQueueSize--;
    }
    
    fEventQueue[fEventQueueTail] = event;
    fEventQueueTail = nextTail;
    fEventQueueSize++;
    
    lck_mtx_unlock(fEventLock);
    
    return kH1Success;
}

int AppleH1CameraInterface::processEvents(void)
{
    /* Process events and notify clients */
    
    return kH1Success;
}

#pragma mark - AppleH1CameraInterface::Public API

int AppleH1CameraInterface::getSensorCount(uint32_t* count)
{
    if (!count) {
        return kH1ErrInvalidParam;
    }
    
    *count = fNumSensors;
    return kH1Success;
}

int AppleH1CameraInterface::getSensorCaps(uint32_t sensorId, h1_sensor_caps_t* caps)
{
    if (sensorId >= fNumSensors || !caps) {
        return kH1ErrInvalidParam;
    }
    
    lck_mtx_lock(fSensorLock);
    memcpy(caps, &fSensorCaps[sensorId], sizeof(h1_sensor_caps_t));
    lck_mtx_unlock(fSensorLock);
    
    return kH1Success;
}

int AppleH1CameraInterface::setSensorConfig(uint32_t sensorId, h1_sensor_config_t* config)
{
    return configureSensor(sensorId, config);
}

int AppleH1CameraInterface::getSensorConfig(uint32_t sensorId, h1_sensor_config_t* config)
{
    if (sensorId >= fNumSensors || !config) {
        return kH1ErrInvalidParam;
    }
    
    lck_mtx_lock(fSensorLock);
    memcpy(config, &fSensorConfigs[sensorId], sizeof(h1_sensor_config_t));
    lck_mtx_unlock(fSensorLock);
    
    return kH1Success;
}

int AppleH1CameraInterface::startCapture(uint32_t streamMask)
{
    int ret = kH1Success;
    
    /* Power on sensors */
    for (int i = 0; i < fNumSensors; i++) {
        if (streamMask & (1 << i)) {
            ret = powerOnSensor(i);
            if (ret != kH1Success) {
                return ret;
            }
            
            ret = startSensor(i);
            if (ret != kH1Success) {
                return ret;
            }
        }
    }
    
    /* Allocate frame buffers */
    ret = allocateFrameBuffers(H1_MAX_FRAME_BUFFERS, H1_MAX_FRAME_SIZE);
    
    return ret;
}

int AppleH1CameraInterface::stopCapture(void)
{
    /* Stop all active streams */
    for (int i = 0; i < H1_MAX_STREAMS; i++) {
        if (fStreams[i].stream_id != 0) {
            stopStream(fStreams[i].stream_id);
        }
    }
    
    /* Stop all sensors */
    for (int i = 0; i < fNumSensors; i++) {
        stopSensor(i);
        powerOffSensor(i);
    }
    
    /* Free frame buffers */
    freeFrameBuffers();
    
    return kH1Success;
}

int AppleH1CameraInterface::captureFrame(h1_frame_t* frame, uint32_t timeout)
{
    h1_frame_t* captured;
    int ret;
    
    if (!frame) {
        return kH1ErrInvalidParam;
    }
    
    /* Trigger capture */
    writeH1Register(H1_REG_CAMERA_CONTROL, H1_CTRL_CAPTURE_IMAGE);
    
    /* Wait for frame ready or timeout */
    uint64_t deadline = mach_absolute_time() + (timeout * 1000 * 1000);
    
    while (mach_absolute_time() < deadline) {
        ret = dequeueFrame(&captured);
        if (ret == kH1Success) {
            memcpy(frame, captured, sizeof(h1_frame_t));
            frame->data = NULL;  /* Don't copy data pointer */
            
            /* Copy frame data if provided */
            if (frame->data && frame->data_size >= captured->data_size) {
                memcpy(frame->data, captured->data, captured->data_size);
            }
            
            return kH1Success;
        }
        
        IODelay(1000);  /* 1ms */
    }
    
    return kH1ErrTimeout;
}

int AppleH1CameraInterface::setFocus(uint32_t sensorId, uint32_t position)
{
    if (sensorId >= fNumSensors || position > H1_MAX_FOCUS_STEPS) {
        return kH1ErrInvalidParam;
    }
    
    lck_mtx_lock(fSensorLock);
    fSensorConfigs[sensorId].focus_position = position;
    lck_mtx_unlock(fSensorLock);
    
    /* Trigger autofocus */
    writeH1Register(H1_REG_CAMERA_SENSOR_SELECT, sensorId);
    writeH1Register(H1_REG_CAMERA_CONTROL, H1_CTRL_AF_TRIGGER);
    
    return kH1Success;
}

int AppleH1CameraInterface::setExposure(uint32_t sensorId, uint32_t exposure, uint32_t gain)
{
    if (sensorId >= fNumSensors) {
        return kH1ErrInvalidParam;
    }
    
    lck_mtx_lock(fSensorLock);
    fSensorConfigs[sensorId].exposure = exposure;
    fSensorConfigs[sensorId].gain = gain;
    lck_mtx_unlock(fSensorLock);
    
    /* Trigger autoexposure */
    writeH1Register(H1_REG_CAMERA_SENSOR_SELECT, sensorId);
    writeH1Register(H1_REG_CAMERA_CONTROL, H1_CTRL_AE_TRIGGER);
    
    return kH1Success;
}

int AppleH1CameraInterface::setWhiteBalance(uint32_t sensorId, uint32_t red, uint32_t green, uint32_t blue)
{
    if (sensorId >= fNumSensors) {
        return kH1ErrInvalidParam;
    }
    
    lck_mtx_lock(fSensorLock);
    fSensorConfigs[sensorId].whitebalance_red = red;
    fSensorConfigs[sensorId].whitebalance_green = green;
    fSensorConfigs[sensorId].whitebalance_blue = blue;
    lck_mtx_unlock(fSensorLock);
    
    /* Trigger AWB */
    writeH1Register(H1_REG_CAMERA_SENSOR_SELECT, sensorId);
    writeH1Register(H1_REG_CAMERA_CONTROL, H1_CTRL_AWB_TRIGGER);
    
    return kH1Success;
}

int AppleH1CameraInterface::setISPParams(h1_isp_params_t* params)
{
    return configureISP(params);
}

int AppleH1CameraInterface::getISPParams(h1_isp_params_t* params)
{
    if (!params) {
        return kH1ErrInvalidParam;
    }
    
    lck_mtx_lock(fISPLock);
    memcpy(params, &fISPParams, sizeof(h1_isp_params_t));
    lck_mtx_unlock(fISPLock);
    
    return kH1Success;
}

int AppleH1CameraInterface::getStatistics(h1_camera_stats_t* stats)
{
    if (!stats) {
        return kH1ErrInvalidParam;
    }
    
    memcpy(stats, &fStats, sizeof(h1_camera_stats_t));
    
    return kH1Success;
}

int AppleH1CameraInterface::resetStatistics(void)
{
    bzero(&fStats, sizeof(fStats));
    return kH1Success;
}

int AppleH1CameraInterface::enterSecureMode(uint64_t sessionId, uint8_t* key, uint32_t keyLen)
{
    if (keyLen < 32) {
        return kH1ErrInvalidParam;
    }
    
    return enableSecureMode(sessionId, key);
}

int AppleH1CameraInterface::exitSecureMode(void)
{
    return disableSecureMode();
}

bool AppleH1CameraInterface::isSecureModeEnabled(void)
{
    return fSecureMode;
}

#pragma mark - AppleH1CameraInterface::USB Interface (for external cameras)

bool AppleH1CameraInterface::probeUSBDevice(IOUSBHostDevice* device)
{
    /* Check if this is a supported USB camera */
    
    return false;
}

bool AppleH1CameraInterface::configureUSBInterface(IOUSBHostInterface* interface)
{
    /* Configure USB interface for camera streaming */
    
    return true;
}

int AppleH1CameraInterface::usbControlRequest(uint8_t bRequest, uint16_t wValue, uint16_t wIndex, void* data, uint16_t wLength)
{
    IOUSBDevRequest request;
    IOReturn ret;
    
    if (!fUSBControlPipe) {
        return kH1ErrNotReady;
    }
    
    request.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBVendor, kUSBDevice);
    request.bRequest = bRequest;
    request.wValue = wValue;
    request.wIndex = wIndex;
    request.wLength = wLength;
    request.pData = data;
    request.wLenDone = 0;
    
    ret = fUSBControlPipe->completeIOUSBDevRequest(&request);
    
    return (ret == kIOReturnSuccess) ? kH1Success : kH1ErrGeneral;
}

int AppleH1CameraInterface::usbBulkTransfer(void* data, uint32_t length, uint32_t timeout)
{
    IOReturn ret;
    uint32_t actualLength;
    
    if (!fUSBBulkPipe) {
        return kH1ErrNotReady;
    }
    
    ret = fUSBBulkPipe->io(data, length, &actualLength, timeout);
    
    return (ret == kIOReturnSuccess) ? kH1Success : kH1ErrTimeout;
}

#pragma mark - AppleH1CameraInterface::IOService Overrides

bool AppleH1CameraInterface::start(IOService* provider)
{
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("AppleH1CameraInterface: Starting version %d.%d\n",
          (fDriverVersion >> 16) & 0xFFFF, fDriverVersion & 0xFFFF);
    
    /* Initialize hardware */
    if (!initializeHardware()) {
        IOLog("AppleH1CameraInterface: Hardware initialization failed\n");
        return false;
    }
    
    /* Create work loop */
    if (!createWorkLoop()) {
        IOLog("AppleH1CameraInterface: Failed to create work loop\n");
        shutdownHardware();
        return false;
    }
    
    /* Register power management */
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, NULL, 0);
    
    /* Start timer */
    if (fTimerSource) {
        fTimerSource->setTimeoutMS(1000);
    }
    
    /* Publish properties */
    setProperty("Driver Version", fDriverVersion, 32);
    setProperty("Driver Revision", fDriverRevision, 32);
    setProperty("Driver Build", fDriverBuild);
    setProperty("Sensor Count", fNumSensors, 32);
    
    /* Register service */
    registerService();
    
    IOLog("AppleH1CameraInterface: Started successfully with %d sensor(s)\n", fNumSensors);
    
    return true;
}

void AppleH1CameraInterface::stop(IOService* provider)
{
    IOLog("AppleH1CameraInterface: Stopping\n");
    
    /* Stop timer */
    if (fTimerSource) {
        fTimerSource->cancelTimeout();
    }
    
    /* Stop capture */
    stopCapture();
    
    /* Shutdown hardware */
    shutdownHardware();
    
    /* Destroy work loop */
    destroyWorkLoop();
    
    /* Power management */
    PMstop();
    
    super::stop(provider);
}

IOReturn AppleH1CameraInterface::setPowerState(unsigned long powerState, IOService* device)
{
    if (powerState == 0) {
        /* Going to sleep */
        IOLog("AppleH1CameraInterface: Preparing for sleep\n");
        stopCapture();
        updatePowerState(H1_POWER_SLEEP);
    } else {
        /* Waking up */
        IOLog("AppleH1CameraInterface: Waking from sleep\n");
        updatePowerState(H1_POWER_IDLE);
    }
    
    return IOPMAckImplied;
}

IOReturn AppleH1CameraInterface::powerStateWillChangeTo(IOPMPowerFlags capabilities,
                                                         unsigned long stateNumber,
                                                         IOService* whatDevice)
{
    return IOPMAckImplied;
}

IOReturn AppleH1CameraInterface::newUserClient(task_t owningTask,
                                                void* securityID,
                                                UInt32 type,
                                                OSDictionary* properties,
                                                IOUserClient** handler)
{
    AppleH1CameraUserClient* client;
    IOReturn ret = kIOReturnSuccess;
    
    /* Check if exclusive client exists */
    lck_mtx_lock(fClientLock);
    if (fExclusiveClient != NULL && fExclusiveClient != owningTask) {
        lck_mtx_unlock(fClientLock);
        return kIOReturnExclusiveAccess;
    }
    
    if (fClientCount >= 1 && fExclusiveClient == NULL) {
        /* Only one client allowed for camera access */
        lck_mtx_unlock(fClientLock);
        return kIOReturnExclusiveAccess;
    }
    lck_mtx_unlock(fClientLock);
    
    client = new AppleH1CameraUserClient;
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
    
    lck_mtx_lock(fClientLock);
    fExclusiveClient = owningTask;
    fClientCount++;
    lck_mtx_unlock(fClientLock);
    
    return ret;
}

#pragma mark - AppleH1CameraUserClient Implementation

bool AppleH1CameraUserClient::init(OSDictionary* dictionary)
{
    if (!IOUserClient::init(dictionary)) {
        return false;
    }
    
    fTask = NULL;
    fPid = 0;
    fProvider = NULL;
    fPermissions = 0;
    fState = 0;
    fStreamMask = 0;
    fSharedMemory = NULL;
    fSharedBuffer = NULL;
    fSharedBufferSize = 0;
    fValid = false;
    
    return true;
}

void AppleH1CameraUserClient::free(void)
{
    if (fSharedMemory) {
        fSharedMemory->release();
        fSharedMemory = NULL;
    }
    
    IOUserClient::free();
}

bool AppleH1CameraUserClient::start(IOService* provider)
{
    if (!IOUserClient::start(provider)) {
        return false;
    }
    
    fProvider = OSDynamicCast(AppleH1CameraInterface, provider);
    if (!fProvider) {
        return false;
    }
    
    fTask = current_task();
    fPid = pid_from_task(fTask);
    
    /* Check entitlements for camera access */
    /* In real implementation, would check com.apple.private.camera */
    
    fValid = true;
    
    return true;
}

void AppleH1CameraUserClient::stop(IOService* provider)
{
    if (fValid && fProvider) {
        /* Stop any active streams */
        if (fStreamMask) {
            fProvider->stopCapture();
        }
        
        lck_mtx_lock(fProvider->fClientLock);
        fProvider->fExclusiveClient = NULL;
        fProvider->fClientCount--;
        lck_mtx_unlock(fProvider->fClientLock);
    }
    
    fValid = false;
    
    IOUserClient::stop(provider);
}

IOReturn AppleH1CameraUserClient::clientClose(void)
{
    if (fValid && fProvider) {
        if (fStreamMask) {
            fProvider->stopCapture();
        }
    }
    
    fValid = false;
    
    terminate();
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CameraUserClient::clientDied(void)
{
    return clientClose();
}

IOReturn AppleH1CameraUserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)
{
    return kIOReturnUnsupported;
}

IOReturn AppleH1CameraUserClient::externalMethod(uint32_t selector,
                                                  IOExternalMethodArguments* arguments,
                                                  IOExternalMethodDispatch* dispatch,
                                                  OSObject* target,
                                                  void* reference)
{
    if (selector < kMethodCount) {
        dispatch = &sMethods[selector];
        if (!dispatch) {
            return kIOReturnBadArgument;
        }
        
        return dispatch->function(this, reference, arguments);
    }
    
    return kIOReturnBadArgument;
}

/* External method implementations */

IOReturn AppleH1CameraUserClient::sGetInfo(AppleH1CameraUserClient* target,
                                            void* reference,
                                            IOExternalMethodArguments* args)
{
    struct {
        uint32_t version;
        uint32_t sensorCount;
        char     name[64];
        uint32_t secureCapable;
    } info;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    bzero(&info, sizeof(info));
    
    info.version = APPLE_H1_CAMERA_VERSION;
    target->fProvider->getSensorCount(&info.sensorCount);
    strlcpy(info.name, "Apple H1 Camera Interface", sizeof(info.name));
    info.secureCapable = target->fProvider->isSecureModeEnabled() ? 1 : 0;
    
    /* Copy to user */
    if (args->structureOutputDescriptor) {
        args->structureOutputDescriptor->writeBytes(0, &info, sizeof(info));
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CameraUserClient::sGetSensorCaps(AppleH1CameraUserClient* target,
                                                  void* reference,
                                                  IOExternalMethodArguments* args)
{
    uint32_t sensorId = (uint32_t)args->scalarInput[0];
    h1_sensor_caps_t caps;
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ret = target->fProvider->getSensorCaps(sensorId, &caps);
    
    if (ret == kH1Success && args->structureOutputDescriptor) {
        args->structureOutputDescriptor->writeBytes(0, &caps, sizeof(caps));
    }
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH1CameraUserClient::sConfigureSensor(AppleH1CameraUserClient* target,
                                                    void* reference,
                                                    IOExternalMethodArguments* args)
{
    uint32_t sensorId = (uint32_t)args->scalarInput[0];
    h1_sensor_config_t config;
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Read config from user */
    if (args->structureInputDescriptor) {
        args->structureInputDescriptor->readBytes(0, &config, sizeof(config));
    }
    
    ret = target->fProvider->setSensorConfig(sensorId, &config);
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH1CameraUserClient::sStartCapture(AppleH1CameraUserClient* target,
                                                  void* reference,
                                                  IOExternalMethodArguments* args)
{
    uint32_t streamMask = (uint32_t)args->scalarInput[0];
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ret = target->fProvider->startCapture(streamMask);
    
    if (ret == kH1Success) {
        target->fStreamMask = streamMask;
    }
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH1CameraUserClient::sStopCapture(AppleH1CameraUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args)
{
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ret = target->fProvider->stopCapture();
    
    if (ret == kH1Success) {
        target->fStreamMask = 0;
    }
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH1CameraUserClient::sCaptureFrame(AppleH1CameraUserClient* target,
                                                  void* reference,
                                                  IOExternalMethodArguments* args)
{
    uint32_t timeout = (uint32_t)args->scalarInput[0];
    h1_frame_t frame;
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Prepare frame buffer */
    if (args->structureOutputDescriptor) {
        /* Use descriptor for frame data */
        frame.data = NULL;  /* Will be handled by descriptor */
        frame.data_size = args->structureOutputDescriptor->getLength();
    }
    
    ret = target->fProvider->captureFrame(&frame, timeout);
    
    /* Write metadata to user */
    if (ret == kH1Success && args->structureOutputDescriptor) {
        /* Would write frame metadata */
    }
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH1CameraUserClient::sSetFocus(AppleH1CameraUserClient* target,
                                             void* reference,
                                             IOExternalMethodArguments* args)
{
    uint32_t sensorId = (uint32_t)args->scalarInput[0];
    uint32_t position = (uint32_t)args->scalarInput[1];
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ret = target->fProvider->setFocus(sensorId, position);
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH1CameraUserClient::sSetExposure(AppleH1CameraUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args)
{
    uint32_t sensorId = (uint32_t)args->scalarInput[0];
    uint32_t exposure = (uint32_t)args->scalarInput[1];
    uint32_t gain = (uint32_t)args->scalarInput[2];
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ret = target->fProvider->setExposure(sensorId, exposure, gain);
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH1CameraUserClient::sSetWhiteBalance(AppleH1CameraUserClient* target,
                                                     void* reference,
                                                     IOExternalMethodArguments* args)
{
    uint32_t sensorId = (uint32_t)args->scalarInput[0];
    uint32_t red = (uint32_t)args->scalarInput[1];
    uint32_t green = (uint32_t)args->scalarInput[2];
    uint32_t blue = (uint32_t)args->scalarInput[3];
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ret = target->fProvider->setWhiteBalance(sensorId, red, green, blue);
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH1CameraUserClient::sSetISPParams(AppleH1CameraUserClient* target,
                                                  void* reference,
                                                  IOExternalMethodArguments* args)
{
    h1_isp_params_t params;
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Read params from user */
    if (args->structureInputDescriptor) {
        args->structureInputDescriptor->readBytes(0, &params, sizeof(params));
    }
    
    ret = target->fProvider->setISPParams(&params);
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH1CameraUserClient::sEnterSecureMode(AppleH1CameraUserClient* target,
                                                     void* reference,
                                                     IOExternalMethodArguments* args)
{
    uint64_t sessionId = args->scalarInput[0];
    uint32_t keyLen = (uint32_t)args->scalarInput[1];
    uint8_t key[32];
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Read key from user */
    if (args->structureInputDescriptor && keyLen <= 32) {
        args->structureInputDescriptor->readBytes(0, key, keyLen);
    }
    
    ret = target->fProvider->enterSecureMode(sessionId, key, keyLen);
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH1CameraUserClient::sExitSecureMode(AppleH1CameraUserClient* target,
                                                    void* reference,
                                                    IOExternalMethodArguments* args)
{
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ret = target->fProvider->exitSecureMode();
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}

IOReturn AppleH1CameraUserClient::sGetStatistics(AppleH1CameraUserClient* target,
                                                   void* reference,
                                                   IOExternalMethodArguments* args)
{
    h1_camera_stats_t stats;
    int ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ret = target->fProvider->getStatistics(&stats);
    
    if (ret == kH1Success && args->structureOutputDescriptor) {
        args->structureOutputDescriptor->writeBytes(0, &stats, sizeof(stats));
    }
    
    return (ret == kH1Success) ? kIOReturnSuccess : kIOReturnError;
}
