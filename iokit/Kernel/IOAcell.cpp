/*
 * Copyright (c) 2010-2022 Apple Inc. All rights reserved.
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
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOPlatformExpert.h>

#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSDebug.h>

#include <machine/machine_routines.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/kauth.h>

/*==============================================================================
 * AppleEmbeddedAccelerometer - Motion Sensing Driver
 * 
 * Supports multiple accelerometer hardware:
 * - Bosch Sensortec BMI160
 * - STMicroelectronics LSM6DSL
 * - InvenSense MPU-6050/6500/9250
 * - Apple custom motion coprocessors
 *==============================================================================*/

/*==============================================================================
 * Constants and Definitions
 *==============================================================================*/

#define ACCELEROMETER_VERSION          0x00020001  /* Version 2.1 */
#define ACCELEROMETER_REVISION         0x00000001

/* Maximum values */
#define ACCEL_MAX_CLIENTS               8
#define ACCEL_MAX_FIFO_ENTRIES          1024
#define ACCEL_MAX_SAMPLES_PER_SEC       1000
#define ACCEL_MAX_RANGE_G                16
#define ACCEL_MAX_ODR                    1600    /* Hz */
#define ACCEL_MAX_FIFO_SIZE              4096    /* bytes */

/* Default values */
#define ACCEL_DEFAULT_ODR                100     /* Hz */
#define ACCEL_DEFAULT_RANGE               2      /* g */
#define ACCEL_DEFAULT_FIFO_THRESHOLD      64     /* samples */
#define ACCEL_DEFAULT_TIMEOUT_MS          100    /* ms */

/* Hardware types */
enum {
    kAccelHWTypeUnknown         = 0,
    kAccelHWTypeBMI160          = 1,    /* Bosch BMI160 */
    kAccelHWTypeLSM6DSL         = 2,    /* ST LSM6DSL */
    kAccelHWTypeMPU6050         = 3,    /* InvenSense MPU-6050 */
    kAccelHWTypeMPU6500         = 4,    /* InvenSense MPU-6500 */
    kAccelHWTypeMPU9250         = 5,    /* InvenSense MPU-9250 (9-axis) */
    kAccelHWTypeAppleMotion     = 6,    /* Apple custom motion coprocessor */
    kAccelHWTypeAppleM7         = 7,    /* Apple M7 motion coprocessor */
    kAccelHWTypeAppleM8         = 8,    /* Apple M8 motion coprocessor */
    kAccelHWTypeAppleM9         = 9,    /* Apple M9 motion coprocessor */
    kAccelHWTypeAppleM10        = 10,   /* Apple M10 motion coprocessor */
};

/* Sensor types */
enum {
    kAccelSensorAccelerometer   = 0x01,
    kAccelSensorGyroscope       = 0x02,
    kAccelSensorMagnetometer    = 0x04,
    kAccelSensorAll             = 0x07
};

/* Operating modes */
enum {
    kAccelModePowerDown         = 0,
    kAccelModeLowPower          = 1,
    kAccelModeNormal            = 2,
    kAccelModeHighPerformance   = 3,
    kAccelModeFIFO              = 4
};

/* FIFO modes */
enum {
    kAccelFIFOModeBypass        = 0,
    kAccelFIFOModeFIFO          = 1,
    kAccelFIFOModeStream        = 2,
    kAccelFIFOModeTrigger       = 3
};

/* Data ready flags */
enum {
    kAccelDataAccelReady        = (1 << 0),
    kAccelDataGyroReady         = (1 << 1),
    kAccelDataMagReady          = (1 << 2),
    kAccelDataTempReady         = (1 << 3),
    kAccelDataAllReady          = 0x0F
};

/* Interrupt sources */
enum {
    kAccelIntDataReady          = (1 << 0),
    kAccelIntFIFOReady          = (1 << 1),
    kAccelIntFIFOFull           = (1 << 2),
    kAccelIntFIFOOverrun        = (1 << 3),
    kAccelIntMotion             = (1 << 4),
    kAccelIntNoMotion           = (1 << 5),
    kAccelIntTap                = (1 << 6),
    kAccelIntDoubleTap          = (1 << 7),
    kAccelIntFreeFall           = (1 << 8),
    kAccelIntWakeup             = (1 << 9),
    kAccelIntStepDetector       = (1 << 10),
    kAccelIntSignificantMotion  = (1 << 11),
    kAccelIntTilt               = (1 << 12),
    kAccelIntPedometer          = (1 << 13),
    kAccelIntAll                = 0xFFFF
};

/* Error codes */
#define kAccelSuccess               0
#define kAccelErrGeneral            -1
#define kAccelErrNoMemory           -2
#define kAccelErrInvalidParam       -3
#define kAccelErrNotReady           -4
#define kAccelErrBusy               -5
#define kAccelErrTimeout            -6
#define kAccelErrAccessDenied       -7
#define kAccelErrNoData             -8
#define kAccelErrNotSupported       -9
#define kAccelErrBufferTooSmall     -10
#define kAccelErrI2CFailure         -11
#define kAccelErrSPIFailure         -12
#define kAccelErrCalibration        -13
#define kAccelErrFIFOFull           -14
#define kAccelErrFIFOEmpty          -15

/*==============================================================================
 * Data Structures
 *==============================================================================*/

/* 3D vector (raw) */
typedef struct {
    int16_t     x;
    int16_t     y;
    int16_t     z;
} __attribute__((packed)) accel_vector_raw_t;

/* 3D vector (float) */
typedef struct {
    float       x;
    float       y;
    float       z;
} accel_vector_float_t;

/* Sensor data sample */
typedef struct {
    uint64_t    timestamp;                  /* Timestamp (nanoseconds) */
    uint64_t    sequence;                   /* Sequence number */
    uint32_t    sensors;                     /* Sensors present */
    
    /* Accelerometer data (in g) */
    accel_vector_float_t accel;
    float       accel_temperature;           /* Accelerometer temperature */
    
    /* Gyroscope data (in degrees/sec) */
    accel_vector_float_t gyro;
    float       gyro_temperature;            /* Gyroscope temperature */
    
    /* Magnetometer data (in uT) */
    accel_vector_float_t mag;
    float       mag_temperature;             /* Magnetometer temperature */
    
    /* Status flags */
    uint32_t    status;                      /* Sample status */
    uint32_t    accuracy;                     /* Accuracy estimate (0-100) */
} accel_sample_t;

/* FIFO entry */
typedef struct {
    uint8_t     header;                      /* FIFO header */
    union {
        accel_vector_raw_t raw;
        uint8_t bytes[6];
    } data;
} accel_fifo_entry_t;

/* FIFO configuration */
typedef struct {
    uint32_t    mode;                         /* FIFO mode */
    uint32_t    threshold;                     /* Threshold in samples */
    uint32_t    watermark;                     /* Watermark level */
    uint32_t    max_size;                      /* Maximum FIFO size */
    uint32_t    sample_size;                   /* Size per sample */
    bool        time_enabled;                  /* Include timestamp */
    bool        header_enabled;                 /* Include header */
} accel_fifo_config_t;

/* Calibration data */
typedef struct {
    accel_vector_float_t accel_offset;         /* Accelerometer offset */
    accel_vector_float_t accel_scale;           /* Accelerometer scale */
    accel_vector_float_t gyro_offset;           /* Gyroscope offset */
    accel_vector_float_t gyro_scale;            /* Gyroscope scale */
    accel_vector_float_t mag_offset;            /* Magnetometer offset */
    accel_vector_float_t mag_scale;             /* Magnetometer scale */
    accel_vector_float_t hard_iron;             /* Hard iron correction */
    accel_vector_float_t soft_iron;             /* Soft iron correction */
    uint64_t            calibration_time;       /* Calibration timestamp */
    uint32_t            calibration_flags;      /* Calibration flags */
} accel_calibration_t;

/* Pedometer data */
typedef struct {
    uint64_t    timestamp;                      /* Timestamp */
    uint32_t    step_count;                      /* Total steps */
    uint32_t    step_rate;                       /* Steps per minute */
    float       distance;                        /* Distance in meters */
    float       calories;                        /* Calories burned */
    uint32_t    cadence;                         /* Stride cadence */
    uint32_t    confidence;                      /* Confidence (0-100) */
} accel_pedometer_t;

/* Motion event */
typedef struct {
    uint64_t    timestamp;                      /* Timestamp */
    uint32_t    event_type;                      /* Event type */
    uint32_t    event_flags;                     /* Event flags */
    union {
        struct {
            uint32_t    direction;               /* Tap direction */
            uint32_t    count;                    /* Tap count */
        } tap;
        struct {
            float       magnitude;                /* Free fall magnitude */
        } freefall;
        struct {
            float       angle;                     /* Tilt angle */
        } tilt;
        struct {
            uint32_t    steps;                     /* Step count */
        } step;
    } data;
} accel_motion_event_t;

/* Device configuration */
typedef struct {
    uint32_t    odr;                            /* Output data rate (Hz) */
    uint32_t    accel_range;                     /* Accelerometer range (g) */
    uint32_t    gyro_range;                       /* Gyroscope range (dps) */
    uint32_t    mag_range;                        /* Magnetometer range (uT) */
    uint32_t    bandwidth;                        /* Filter bandwidth */
    uint32_t    power_mode;                        /* Power mode */
    uint32_t    fifo_mode;                         /* FIFO mode */
    uint32_t    fifo_threshold;                    /* FIFO threshold */
    uint32_t    interrupt_enable;                   /* Enabled interrupts */
    uint32_t    sensors_enable;                     /* Enabled sensors */
    bool        auto_sleep;                         /* Auto sleep enabled */
    bool        wake_on_motion;                      /* Wake on motion */
} accel_config_t;

/* Statistics */
typedef struct {
    uint64_t    samples_collected;                  /* Total samples */
    uint64_t    samples_dropped;                     /* Dropped samples */
    uint64_t    fifo_overruns;                       /* FIFO overruns */
    uint64_t    interrupts;                           /* Interrupt count */
    uint64_t    wake_events;                          /* Wake events */
    uint64_t    errors;                               /* Error count */
    uint64_t    calibration_attempts;                 /* Calibration attempts */
    uint64_t    calibration_success;                  /* Successful calibrations */
} accel_statistics_t;

/*==============================================================================
 * Hardware Register Definitions
 *==============================================================================*/

/* Common register offsets (vendor-specific) */
struct accel_registers {
    /* Chip identification */
    uint8_t     who_am_i;                    /* Who Am I register */
    uint8_t     chip_id;                      /* Chip ID */
    uint8_t     revision;                      /* Revision */
    
    /* Power management */
    uint8_t     pwr_mgmt;                      /* Power management */
    uint8_t     pwr_mode;                      /* Power mode */
    uint8_t     sleep;                          /* Sleep control */
    
    /* Configuration */
    uint8_t     odr_config;                    /* ODR configuration */
    uint8_t     accel_range;                    /* Accelerometer range */
    uint8_t     gyro_range;                      /* Gyroscope range */
    uint8_t     filter_config;                   /* Filter configuration */
    
    /* FIFO control */
    uint8_t     fifo_config;                     /* FIFO configuration */
    uint8_t     fifo_threshold;                  /* FIFO threshold */
    uint8_t     fifo_status;                     /* FIFO status */
    uint8_t     fifo_data;                        /* FIFO data */
    
    /* Interrupts */
    uint8_t     int_config;                       /* Interrupt configuration */
    uint8_t     int_enable;                       /* Interrupt enable */
    uint8_t     int_status;                       /* Interrupt status */
    uint8_t     int_pin;                           /* Interrupt pin control */
    
    /* Data registers */
    uint8_t     accel_x_l;                         /* Accelerometer X low */
    uint8_t     accel_x_h;                         /* Accelerometer X high */
    uint8_t     accel_y_l;                         /* Accelerometer Y low */
    uint8_t     accel_y_h;                         /* Accelerometer Y high */
    uint8_t     accel_z_l;                         /* Accelerometer Z low */
    uint8_t     accel_z_h;                         /* Accelerometer Z high */
    
    uint8_t     gyro_x_l;                          /* Gyroscope X low */
    uint8_t     gyro_x_h;                          /* Gyroscope X high */
    uint8_t     gyro_y_l;                          /* Gyroscope Y low */
    uint8_t     gyro_y_h;                          /* Gyroscope Y high */
    uint8_t     gyro_z_l;                          /* Gyroscope Z low */
    uint8_t     gyro_z_h;                          /* Gyroscope Z high */
    
    uint8_t     mag_x_l;                           /* Magnetometer X low */
    uint8_t     mag_x_h;                           /* Magnetometer X high */
    uint8_t     mag_y_l;                           /* Magnetometer Y low */
    uint8_t     mag_y_h;                           /* Magnetometer Y high */
    uint8_t     mag_z_l;                           /* Magnetometer Z low */
    uint8_t     mag_z_h;                           /* Magnetometer Z high */
    
    uint8_t     temperature_l;                      /* Temperature low */
    uint8_t     temperature_h;                      /* Temperature high */
    
    /* Motion detection */
    uint8_t     motion_threshold;                   /* Motion threshold */
    uint8_t     motion_duration;                    /* Motion duration */
    uint8_t     tap_threshold;                      /* Tap threshold */
    uint8_t     tap_duration;                       /* Tap duration */
    
    /* Self test */
    uint8_t     self_test;                          /* Self test control */
    uint8_t     status;                             /* Status register */
};

/*==============================================================================
 * AppleEmbeddedAccelerometer Main Class
 *==============================================================================*/

class AppleEmbeddedAccelerometer : public IOService
{
    OSDeclareDefaultStructors(AppleEmbeddedAccelerometer)
    
private:
    /* IOKit resources */
    IOWorkLoop*                 fWorkLoop;
    IOCommandGate*              fCommandGate;
    IOTimerEventSource*         fTimerSource;
    IOInterruptEventSource*     fInterruptSource;
    
    /* Hardware interface */
    IOMemoryMap*                fMemoryMap;
    volatile uint8_t*           fRegisters;
    IOPhysicalAddress           fPhysicalAddress;
    IOPhysicalLength            fPhysicalLength;
    uint32_t                    fHWType;
    uint32_t                    fHWRevision;
    uint32_t                    fChipID;
    
    /* I2C/SPI transport */
    IOService*                  fTransportProvider;
    uint32_t                    fTransportType;     /* 0=I2C, 1=SPI */
    uint32_t                    fTransportAddress;
    
    /* Device state */
    uint32_t                    fState;
    uint32_t                    fPowerState;
    uint64_t                    fStartTime;
    uint64_t                    fLastSampleTime;
    uint64_t                    fSampleCount;
    
    /* Configuration */
    accel_config_t              fConfig;
    accel_fifo_config_t         fFIFOConfig;
    accel_calibration_t         fCalibration;
    
    /* FIFO buffer */
    IOBufferMemoryDescriptor*   fFIFOBuffer;
    uint32_t                    fFIFOPosition;
    uint32_t                    fFIFOCount;
    
    /* Sample queue */
    accel_sample_t*             fSampleQueue;
    uint32_t                    fQueueHead;
    uint32_t                    fQueueTail;
    uint32_t                    fQueueSize;
    uint32_t                    fQueueMaxSize;
    lck_mtx_t*                  fQueueLock;
    
    /* Motion events */
    accel_motion_event_t*       fEventQueue;
    uint32_t                    fEventHead;
    uint32_t                    fEventTail;
    uint32_t                    fEventQueueSize;
    lck_mtx_t*                  fEventLock;
    
    /* Pedometer data */
    accel_pedometer_t           fPedometer;
    bool                        fPedometerEnabled;
    lck_mtx_t*                  fPedometerLock;
    
    /* Client management */
    struct AccelClient {
        task_t      task;
        pid_t       pid;
        uint32_t    clientId;
        uint32_t    permissions;
        uint32_t    sensorMask;
        uint32_t    eventMask;
        uint32_t    sampleRate;
        uint32_t    queueSize;
        bool        active;
        void*       callback;
        uint64_t    lastRead;
    } fClients[ACCEL_MAX_CLIENTS];
    uint32_t                    fNumClients;
    uint32_t                    fNextClientId;
    lck_mtx_t*                  fClientLock;
    
    /* Statistics */
    accel_statistics_t          fStats;
    lck_mtx_t*                  fStatsLock;
    
    /* Locking */
    lck_grp_t*                  fLockGroup;
    lck_attr_t*                 fLockAttr;
    lck_mtx_t*                  fHardwareLock;
    
    /*==============================================================================
     * Private Methods
     *==============================================================================*/
    
    /* Hardware initialization */
    bool                        probeHardware(void);
    bool                        initializeHardware(void);
    void                        shutdownHardware(void);
    bool                        resetHardware(void);
    
    /* Register access */
    uint8_t                     readRegister(uint8_t reg);
    void                        writeRegister(uint8_t reg, uint8_t value);
    uint16_t                    readRegister16(uint8_t reg);
    void                        writeRegister16(uint8_t reg, uint16_t value);
    bool                        waitForBit(uint8_t reg, uint8_t bit, bool set, uint32_t timeout);
    
    /* Hardware configuration */
    int                         setODR(uint32_t odr);
    int                         setAccelRange(uint32_t range);
    int                         setGyroRange(uint32_t range);
    int                         setPowerMode(uint32_t mode);
    int                         configureFIFO(void);
    int                         configureInterrupts(void);
    
    /* Data reading */
    int                         readAccelerometer(accel_vector_raw_t* raw);
    int                         readGyroscope(accel_vector_raw_t* raw);
    int                         readMagnetometer(accel_vector_raw_t* raw);
    int                         readTemperature(int16_t* temp);
    int                         readAllSensors(accel_sample_t* sample);
    
    /* FIFO operations */
    int                         readFIFO(void);
    int                         flushFIFO(void);
    int                         processFIFOEntry(accel_fifo_entry_t* entry);
    
    /* Data processing */
    void                        convertRawToFloat(accel_vector_raw_t* raw, 
                                                   accel_vector_float_t* flt,
                                                   float scale);
    void                        applyCalibration(accel_sample_t* sample);
    void                        computeOrientation(accel_sample_t* sample);
    
    /* Motion detection */
    int                         detectMotion(accel_sample_t* sample);
    int                         detectTap(accel_sample_t* sample);
    int                         detectFreeFall(accel_sample_t* sample);
    int                         detectTilt(accel_sample_t* sample);
    
    /* Pedometer */
    int                         updatePedometer(accel_sample_t* sample);
    int                         resetPedometer(void);
    
    /* Calibration */
    int                         calibrateAccelerometer(void);
    int                         calibrateGyroscope(void);
    int                         calibrateMagnetometer(void);
    int                         loadCalibration(void);
    int                         saveCalibration(void);
    
    /* Sample queue */
    int                         queueSample(accel_sample_t* sample);
    int                         dequeueSample(accel_sample_t* sample);
    int                         notifyClients(void);
    
    /* Event queue */
    int                         queueEvent(accel_motion_event_t* event);
    int                         dequeueEvent(accel_motion_event_t* event);
    
    /* Work loop */
    bool                        createWorkLoop(void);
    void                        destroyWorkLoop(void);
    
    /* Timer and interrupt handlers */
    void                        timerFired(void);
    static void                 timerFired(OSObject* owner, IOTimerEventSource* sender);
    
    void                        interruptHandler(void);
    static void                 interruptHandler(OSObject* owner, IOInterruptEventSource* src, int count);
    
    IOReturn                    handleCommand(void* arg0, void* arg1, void* arg2, void* arg3);
    
    /* Power management */
    IOReturn                    powerStateChange(void);
    
    /* Utility */
    uint64_t                    getCurrentTimestamp(void);
    uint64_t                    getCurrentUptime(void);
    float                       convertRawToG(int16_t raw, uint32_t range);
    float                       convertRawToDPS(int16_t raw, uint32_t range);
    float                       convertRawToUT(int16_t raw, uint32_t range);
    
    /* Security */
    bool                        isEntitled(task_t task, const char* entitlement);
    int                         validateClient(uint32_t clientId, uint32_t requiredPerms);
    
protected:
    bool                        init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                        free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                        start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                        stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    /* Power management */
    IOReturn                    setPowerState(unsigned long powerState, 
                                              IOService* device) APPLE_KEXT_OVERRIDE;
    IOReturn                    powerStateWillChangeTo(IOPMPowerFlags capabilities,
                                                        unsigned long stateNumber,
                                                        IOService* whatDevice) APPLE_KEXT_OVERRIDE;
    
    /* User client access */
    IOReturn                    newUserClient(task_t owningTask,
                                              void* securityID,
                                              UInt32 type,
                                              OSDictionary* properties,
                                              IOUserClient** handler) APPLE_KEXT_OVERRIDE;
    
    /* Platform interface */
    virtual bool                didTerminate(IOService* provider, IOOptionBits options,
                                              bool* defer) APPLE_KEXT_OVERRIDE;
    
    /* Client API */
    int                         registerClient(task_t task, uint32_t permissions, uint32_t* clientId);
    int                         unregisterClient(uint32_t clientId);
    int                         setClientConfig(uint32_t clientId, uint32_t sensorMask,
                                                 uint32_t eventMask, uint32_t sampleRate);
    int                         getSample(uint32_t clientId, accel_sample_t* sample);
    int                         getEvent(uint32_t clientId, accel_motion_event_t* event);
    int                         getPedometerData(uint32_t clientId, accel_pedometer_t* ped);
    
    /* Sensor API */
    int                         getLatestSample(accel_sample_t* sample);
    int                         getSamples(accel_sample_t* samples, uint32_t* count);
    int                         getMotionEvents(accel_motion_event_t* events, uint32_t* count);
    
    /* Configuration API */
    int                         setConfig(accel_config_t* config);
    int                         getConfig(accel_config_t* config);
    int                         setCalibration(accel_calibration_t* cal);
    int                         getCalibration(accel_calibration_t* cal);
    
    /* Statistics API */
    int                         getStatistics(accel_statistics_t* stats);
    int                         resetStatistics(void);
    
    /* Test API */
    int                         runSelfTest(void);
    int                         runFactoryCalibration(void);
};

OSDefineMetaClassAndStructors(AppleEmbeddedAccelerometer, IOService)

/*==============================================================================
 * AppleAccelerometerUserClient - User client for accelerometer access
 *==============================================================================*/

class AppleAccelerometerUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(AppleAccelerometerUserClient)
    
private:
    task_t                      fTask;
    pid_t                       fPid;
    uint32_t                    fClientId;
    AppleEmbeddedAccelerometer* fProvider;
    uint32_t                    fPermissions;
    uint32_t                    fState;
    
    mach_port_t                 fNotificationPort;
    uint32_t                    fNotificationRef;
    
    IOMemoryDescriptor*         fSharedMemory;
    void*                       fSharedBuffer;
    uint32_t                    fSharedSize;
    
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
    
    IOReturn                    externalMethod(uint32_t selector, 
                                                IOExternalMethodArguments* arguments,
                                                IOExternalMethodDispatch* dispatch,
                                                OSObject* target,
                                                void* reference) APPLE_KEXT_OVERRIDE;
    
    /* External methods */
    static IOReturn             sOpenSession(AppleAccelerometerUserClient* target,
                                              void* reference,
                                              IOExternalMethodArguments* args);
    
    static IOReturn             sCloseSession(AppleAccelerometerUserClient* target,
                                               void* reference,
                                               IOExternalMethodArguments* args);
    
    static IOReturn             sSetConfig(AppleAccelerometerUserClient* target,
                                            void* reference,
                                            IOExternalMethodArguments* args);
    
    static IOReturn             sGetConfig(AppleAccelerometerUserClient* target,
                                            void* reference,
                                            IOExternalMethodArguments* args);
    
    static IOReturn             sGetSample(AppleAccelerometerUserClient* target,
                                            void* reference,
                                            IOExternalMethodArguments* args);
    
    static IOReturn             sGetSamples(AppleAccelerometerUserClient* target,
                                             void* reference,
                                             IOExternalMethodArguments* args);
    
    static IOReturn             sGetEvent(AppleAccelerometerUserClient* target,
                                           void* reference,
                                           IOExternalMethodArguments* args);
    
    static IOReturn             sGetPedometer(AppleAccelerometerUserClient* target,
                                               void* reference,
                                               IOExternalMethodArguments* args);
    
    static IOReturn             sSetCalibration(AppleAccelerometerUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args);
    
    static IOReturn             sGetCalibration(AppleAccelerometerUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args);
    
    static IOReturn             sRunSelfTest(AppleAccelerometerUserClient* target,
                                              void* reference,
                                              IOExternalMethodArguments* args);
    
    static IOReturn             sGetStatistics(AppleAccelerometerUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args);
};

OSDefineMetaClassAndStructors(AppleAccelerometerUserClient, IOUserClient)

/*==============================================================================
 * External Method Dispatch Table
 *==============================================================================*/

enum {
    kMethodOpenSession,
    kMethodCloseSession,
    kMethodSetConfig,
    kMethodGetConfig,
    kMethodGetSample,
    kMethodGetSamples,
    kMethodGetEvent,
    kMethodGetPedometer,
    kMethodSetCalibration,
    kMethodGetCalibration,
    kMethodRunSelfTest,
    kMethodGetStatistics,
    kMethodCount
};

static IOExternalMethodDispatch sMethods[kMethodCount] = {
    {   /* kMethodOpenSession */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sOpenSession,
        3,                          /* Number of scalar inputs */
        0,                          /* Number of struct inputs */
        1,                          /* Number of scalar outputs */
        0                           /* Number of struct outputs */
    },
    {   /* kMethodCloseSession */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sCloseSession,
        1, 0, 0, 0
    },
    {   /* kMethodSetConfig */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sSetConfig,
        6, 0, 0, 0
    },
    {   /* kMethodGetConfig */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sGetConfig,
        0, 0, 0, 1
    },
    {   /* kMethodGetSample */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sGetSample,
        0, 0, 0, 1
    },
    {   /* kMethodGetSamples */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sGetSamples,
        1, 0, 0, 1
    },
    {   /* kMethodGetEvent */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sGetEvent,
        0, 0, 0, 1
    },
    {   /* kMethodGetPedometer */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sGetPedometer,
        0, 0, 0, 1
    },
    {   /* kMethodSetCalibration */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sSetCalibration,
        0, 1, 0, 0
    },
    {   /* kMethodGetCalibration */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sGetCalibration,
        0, 0, 0, 1
    },
    {   /* kMethodRunSelfTest */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sRunSelfTest,
        0, 0, 1, 0
    },
    {   /* kMethodGetStatistics */
        (IOExternalMethodAction)&AppleAccelerometerUserClient::sGetStatistics,
        0, 0, 0, 1
    }
};

/*==============================================================================
 * AppleEmbeddedAccelerometer Implementation
 *==============================================================================*/

#pragma mark - Initialization

bool AppleEmbeddedAccelerometer::init(OSDictionary* dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    
    /* Initialize version */
    fHWType = kAccelHWTypeUnknown;
    fHWRevision = 0;
    fChipID = 0;
    
    /* Initialize state */
    fState = 0;
    fPowerState = 0;
    fStartTime = 0;
    fLastSampleTime = 0;
    fSampleCount = 0;
    
    /* Create lock group */
    fLockGroup = lck_grp_alloc_init("AppleAccelerometer", LCK_GRP_ATTR_NULL);
    if (!fLockGroup) {
        return false;
    }
    
    fLockAttr = lck_attr_alloc_init();
    if (!fLockAttr) {
        lck_grp_free(fLockGroup);
        return false;
    }
    
    /* Initialize locks */
    fHardwareLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fQueueLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fEventLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fPedometerLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fClientLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fStatsLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    
    if (!fHardwareLock || !fQueueLock || !fEventLock || 
        !fPedometerLock || !fClientLock || !fStatsLock) {
        return false;
    }
    
    /* Initialize client array */
    for (int i = 0; i < ACCEL_MAX_CLIENTS; i++) {
        bzero(&fClients[i], sizeof(fClients[i]));
    }
    fNumClients = 0;
    fNextClientId = 1;
    
    /* Initialize sample queue */
    fQueueMaxSize = ACCEL_MAX_FIFO_ENTRIES;
    fSampleQueue = (accel_sample_t*)IOMalloc(sizeof(accel_sample_t) * fQueueMaxSize);
    if (!fSampleQueue) {
        return false;
    }
    bzero(fSampleQueue, sizeof(accel_sample_t) * fQueueMaxSize);
    fQueueHead = 0;
    fQueueTail = 0;
    fQueueSize = 0;
    
    /* Initialize event queue */
    fEventQueueSize = 64;
    fEventQueue = (accel_motion_event_t*)IOMalloc(sizeof(accel_motion_event_t) * fEventQueueSize);
    if (!fEventQueue) {
        return false;
    }
    bzero(fEventQueue, sizeof(accel_motion_event_t) * fEventQueueSize);
    fEventHead = 0;
    fEventTail = 0;
    
    /* Initialize FIFO buffer */
    fFIFOBuffer = IOBufferMemoryDescriptor::inTask(kernel_task,
                                                     kIODirectionIn | kIODirectionOut,
                                                     ACCEL_MAX_FIFO_SIZE);
    if (!fFIFOBuffer) {
        return false;
    }
    fFIFOPosition = 0;
    fFIFOCount = 0;
    
    /* Initialize statistics */
    bzero(&fStats, sizeof(fStats));
    
    /* Initialize pedometer */
    bzero(&fPedometer, sizeof(fPedometer));
    fPedometerEnabled = false;
    
    /* Initialize calibration */
    bzero(&fCalibration, sizeof(fCalibration));
    fCalibration.accel_scale.x = 1.0f;
    fCalibration.accel_scale.y = 1.0f;
    fCalibration.accel_scale.z = 1.0f;
    fCalibration.gyro_scale.x = 1.0f;
    fCalibration.gyro_scale.y = 1.0f;
    fCalibration.gyro_scale.z = 1.0f;
    fCalibration.mag_scale.x = 1.0f;
    fCalibration.mag_scale.y = 1.0f;
    fCalibration.mag_scale.z = 1.0f;
    
    /* Initialize configuration */
    fConfig.odr = ACCEL_DEFAULT_ODR;
    fConfig.accel_range = ACCEL_DEFAULT_RANGE;
    fConfig.gyro_range = 250;       /* 250 dps */
    fConfig.mag_range = 1200;       /* 1200 uT */
    fConfig.bandwidth = 50;         /* 50 Hz */
    fConfig.power_mode = kAccelModeNormal;
    fConfig.fifo_mode = kAccelFIFOModeFIFO;
    fConfig.fifo_threshold = ACCEL_DEFAULT_FIFO_THRESHOLD;
    fConfig.interrupt_enable = kAccelIntDataReady | kAccelIntFIFOReady;
    fConfig.sensors_enable = kAccelSensorAccelerometer | kAccelSensorGyroscope;
    fConfig.auto_sleep = true;
    fConfig.wake_on_motion = false;
    
    return true;
}

void AppleEmbeddedAccelerometer::free(void)
{
    /* Free sample queue */
    if (fSampleQueue) {
        IOFree(fSampleQueue, sizeof(accel_sample_t) * fQueueMaxSize);
        fSampleQueue = NULL;
    }
    
    /* Free event queue */
    if (fEventQueue) {
        IOFree(fEventQueue, sizeof(accel_motion_event_t) * fEventQueueSize);
        fEventQueue = NULL;
    }
    
    /* Free FIFO buffer */
    if (fFIFOBuffer) {
        fFIFOBuffer->release();
        fFIFOBuffer = NULL;
    }
    
    /* Free locks */
    if (fHardwareLock) lck_mtx_free(fHardwareLock, fLockGroup);
    if (fQueueLock) lck_mtx_free(fQueueLock, fLockGroup);
    if (fEventLock) lck_mtx_free(fEventLock, fLockGroup);
    if (fPedometerLock) lck_mtx_free(fPedometerLock, fLockGroup);
    if (fClientLock) lck_mtx_free(fClientLock, fLockGroup);
    if (fStatsLock) lck_mtx_free(fStatsLock, fLockGroup);
    
    if (fLockAttr) lck_attr_free(fLockAttr);
    if (fLockGroup) lck_grp_free(fLockGroup);
    
    super::free();
}

bool AppleEmbeddedAccelerometer::createWorkLoop(void)
{
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        IOLog("AppleAccelerometer: Failed to create work loop\n");
        return false;
    }
    
    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        IOLog("AppleAccelerometer: Failed to create command gate\n");
        fWorkLoop->release();
        fWorkLoop = NULL;
        return false;
    }
    
    fWorkLoop->addEventSource(fCommandGate);
    
    fTimerSource = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &AppleEmbeddedAccelerometer::timerFired));
    
    if (!fTimerSource) {
        IOLog("AppleAccelerometer: Failed to create timer source\n");
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

void AppleEmbeddedAccelerometer::destroyWorkLoop(void)
{
    if (fWorkLoop) {
        if (fTimerSource) {
            fTimerSource->cancelTimeout();
            fWorkLoop->removeEventSource(fTimerSource);
            fTimerSource->release();
            fTimerSource = NULL;
        }
        
        if (fInterruptSource) {
            fWorkLoop->removeEventSource(fInterruptSource);
            fInterruptSource->release();
            fInterruptSource = NULL;
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

#pragma mark - Hardware Access

uint8_t AppleEmbeddedAccelerometer::readRegister(uint8_t reg)
{
    if (!fRegisters) {
        return 0xFF;
    }
    
    /* In real implementation, would do I2C/SPI read */
    /* For now, just read from memory-mapped registers */
    return fRegisters[reg];
}

void AppleEmbeddedAccelerometer::writeRegister(uint8_t reg, uint8_t value)
{
    if (!fRegisters) {
        return;
    }
    
    /* In real implementation, would do I2C/SPI write */
    fRegisters[reg] = value;
}

uint16_t AppleEmbeddedAccelerometer::readRegister16(uint8_t reg)
{
    uint16_t value;
    value = readRegister(reg);
    value |= (readRegister(reg + 1) << 8);
    return value;
}

void AppleEmbeddedAccelerometer::writeRegister16(uint8_t reg, uint16_t value)
{
    writeRegister(reg, value & 0xFF);
    writeRegister(reg + 1, (value >> 8) & 0xFF);
}

bool AppleEmbeddedAccelerometer::waitForBit(uint8_t reg, uint8_t bit, bool set, uint32_t timeout)
{
    uint64_t deadline;
    uint64_t now;
    
    deadline = getCurrentUptime() + timeout * 1000 * 1000; /* convert to nanoseconds */
    
    do {
        uint8_t value = readRegister(reg);
        if (set && (value & bit)) {
            return true;
        }
        if (!set && !(value & bit)) {
            return true;
        }
        IODelay(10); /* 10 microseconds */
        now = getCurrentUptime();
    } while (now < deadline);
    
    return false;
}

#pragma mark - Hardware Initialization

bool AppleEmbeddedAccelerometer::probeHardware(void)
{
    /* Read WHO_AM_I register to identify chip */
    uint8_t who_am_i = readRegister(0x0F);  /* Common WHO_AM_I address */
    
    switch (who_am_i) {
        case 0xD1:  /* BMI160 */
            fHWType = kAccelHWTypeBMI160;
            fHWRevision = readRegister(0x00);
            break;
            
        case 0x6A:  /* LSM6DSL */
            fHWType = kAccelHWTypeLSM6DSL;
            fHWRevision = readRegister(0x01);
            break;
            
        case 0x68:  /* MPU-6050/6500/9250 */
            fHWType = kAccelHWTypeMPU6500;
            fHWRevision = readRegister(0x00);
            break;
            
        case 0x71:  /* Apple M7/M8/M9/M10 */
            fHWType = kAccelHWTypeAppleMotion;
            fHWRevision = readRegister(0x01);
            break;
            
        default:
            IOLog("AppleAccelerometer: Unknown chip ID: 0x%02x\n", who_am_i);
            return false;
    }
    
    fChipID = who_am_i;
    
    IOLog("AppleAccelerometer: Found %s (ID: 0x%02x, Rev: 0x%02x)\n",
          (fHWType == kAccelHWTypeBMI160) ? "BMI160" :
          (fHWType == kAccelHWTypeLSM6DSL) ? "LSM6DSL" :
          (fHWType == kAccelHWTypeMPU6500) ? "MPU6500" : "Apple Motion",
          fChipID, fHWRevision);
    
    return true;
}

bool AppleEmbeddedAccelerometer::resetHardware(void)
{
    lck_mtx_lock(fHardwareLock);
    
    /* Soft reset sequence (vendor-specific) */
    switch (fHWType) {
        case kAccelHWTypeBMI160:
            writeRegister(0x7E, 0xB6);  /* Soft reset */
            break;
            
        case kAccelHWTypeLSM6DSL:
            writeRegister(0x12, 0x01);  /* Soft reset */
            break;
            
        case kAccelHWTypeMPU6500:
            writeRegister(0x6B, 0x80);  /* Device reset */
            break;
            
        default:
            writeRegister(0x40, 0x80);  /* Generic reset */
            break;
    }
    
    /* Wait for reset to complete */
    IOSleep(10);
    
    lck_mtx_unlock(fHardwareLock);
    
    return true;
}

bool AppleEmbeddedAccelerometer::initializeHardware(void)
{
    lck_mtx_lock(fHardwareLock);
    
    /* Reset device */
    resetHardware();
    
    /* Configure power management */
    writeRegister(0x7D, 0x04);  /* Auto sleep disabled, normal mode */
    
    /* Set output data rate */
    setODR(fConfig.odr);
    
    /* Set accelerometer range */
    setAccelRange(fConfig.accel_range);
    
    /* Set gyroscope range if enabled */
    if (fConfig.sensors_enable & kAccelSensorGyroscope) {
        setGyroRange(fConfig.gyro_range);
    }
    
    /* Configure FIFO */
    configureFIFO();
    
    /* Configure interrupts */
    configureInterrupts();
    
    lck_mtx_unlock(fHardwareLock);
    
    return true;
}

void AppleEmbeddedAccelerometer::shutdownHardware(void)
{
    lck_mtx_lock(fHardwareLock);
    
    /* Disable all interrupts */
    writeRegister(0x50, 0x00);  /* INT_ENABLE */
    
    /* Enter sleep mode */
    writeRegister(0x7D, 0x03);  /* Sleep mode */
    
    lck_mtx_unlock(fHardwareLock);
}

#pragma mark - Hardware Configuration

int AppleEmbeddedAccelerometer::setODR(uint32_t odr)
{
    uint8_t odr_reg = 0;
    
    lck_mtx_lock(fHardwareLock);
    
    /* Convert ODR to register value (vendor-specific) */
    if (odr <= 12) {
        odr_reg = 0x01;  /* 12.5 Hz */
    } else if (odr <= 25) {
        odr_reg = 0x02;  /* 25 Hz */
    } else if (odr <= 50) {
        odr_reg = 0x03;  /* 50 Hz */
    } else if (odr <= 100) {
        odr_reg = 0x04;  /* 100 Hz */
    } else if (odr <= 200) {
        odr_reg = 0x05;  /* 200 Hz */
    } else if (odr <= 400) {
        odr_reg = 0x06;  /* 400 Hz */
    } else if (odr <= 800) {
        odr_reg = 0x07;  /* 800 Hz */
    } else {
        odr_reg = 0x08;  /* 1600 Hz */
    }
    
    writeRegister(0x10, odr_reg);  /* ODR_CONFIG */
    
    fConfig.odr = odr;
    
    lck_mtx_unlock(fHardwareLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::setAccelRange(uint32_t range)
{
    uint8_t range_reg = 0;
    
    lck_mtx_lock(fHardwareLock);
    
    /* Convert range to register value */
    switch (range) {
        case 2:  range_reg = 0x00; break;
        case 4:  range_reg = 0x01; break;
        case 8:  range_reg = 0x02; break;
        case 16: range_reg = 0x03; break;
        default: range = 2; range_reg = 0x00; break;
    }
    
    writeRegister(0x1F, range_reg);  /* ACCEL_RANGE */
    
    fConfig.accel_range = range;
    
    lck_mtx_unlock(fHardwareLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::setGyroRange(uint32_t range)
{
    uint8_t range_reg = 0;
    
    lck_mtx_lock(fHardwareLock);
    
    /* Convert range to register value */
    switch (range) {
        case 250:  range_reg = 0x00; break;
        case 500:  range_reg = 0x01; break;
        case 1000: range_reg = 0x02; break;
        case 2000: range_reg = 0x03; break;
        default:   range = 250; range_reg = 0x00; break;
    }
    
    writeRegister(0x20, range_reg);  /* GYRO_RANGE */
    
    fConfig.gyro_range = range;
    
    lck_mtx_unlock(fHardwareLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::setPowerMode(uint32_t mode)
{
    lck_mtx_lock(fHardwareLock);
    
    switch (mode) {
        case kAccelModePowerDown:
            writeRegister(0x7D, 0x00);  /* Power down */
            break;
            
        case kAccelModeLowPower:
            writeRegister(0x7D, 0x01);  /* Low power */
            break;
            
        case kAccelModeNormal:
            writeRegister(0x7D, 0x04);  /* Normal mode */
            break;
            
        case kAccelModeHighPerformance:
            writeRegister(0x7D, 0x06);  /* High performance */
            break;
    }
    
    fConfig.power_mode = mode;
    
    lck_mtx_unlock(fHardwareLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::configureFIFO(void)
{
    lck_mtx_lock(fHardwareLock);
    
    /* Configure FIFO based on mode */
    switch (fConfig.fifo_mode) {
        case kAccelFIFOModeBypass:
            writeRegister(0x48, 0x00);  /* Bypass mode */
            break;
            
        case kAccelFIFOModeFIFO:
            writeRegister(0x48, 0x40);  /* FIFO mode */
            writeRegister(0x49, fConfig.fifo_threshold);
            break;
            
        case kAccelFIFOModeStream:
            writeRegister(0x48, 0x80);  /* Stream mode */
            break;
            
        case kAccelFIFOModeTrigger:
            writeRegister(0x48, 0xC0);  /* Trigger mode */
            break;
    }
    
    lck_mtx_unlock(fHardwareLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::configureInterrupts(void)
{
    lck_mtx_lock(fHardwareLock);
    
    /* Map interrupts to pins */
    writeRegister(0x50, fConfig.interrupt_enable & 0xFF);
    writeRegister(0x51, (fConfig.interrupt_enable >> 8) & 0xFF);
    
    /* Configure interrupt pin behavior */
    writeRegister(0x53, 0x01);  /* Active high, push-pull */
    
    lck_mtx_unlock(fHardwareLock);
    
    return kAccelSuccess;
}

#pragma mark - Data Reading

int AppleEmbeddedAccelerometer::readAccelerometer(accel_vector_raw_t* raw)
{
    if (!raw) {
        return kAccelErrInvalidParam;
    }
    
    lck_mtx_lock(fHardwareLock);
    
    /* Read accelerometer data (typically 6 bytes) */
    raw->x = readRegister16(0x12);  /* ACCEL_X */
    raw->y = readRegister16(0x14);  /* ACCEL_Y */
    raw->z = readRegister16(0x16);  /* ACCEL_Z */
    
    lck_mtx_unlock(fHardwareLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::readGyroscope(accel_vector_raw_t* raw)
{
    if (!raw) {
        return kAccelErrInvalidParam;
    }
    
    lck_mtx_lock(fHardwareLock);
    
    /* Read gyroscope data */
    raw->x = readRegister16(0x18);  /* GYRO_X */
    raw->y = readRegister16(0x1A);  /* GYRO_Y */
    raw->z = readRegister16(0x1C);  /* GYRO_Z */
    
    lck_mtx_unlock(fHardwareLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::readTemperature(int16_t* temp)
{
    if (!temp) {
        return kAccelErrInvalidParam;
    }
    
    lck_mtx_lock(fHardwareLock);
    
    *temp = readRegister16(0x22);  /* TEMPERATURE */
    
    lck_mtx_unlock(fHardwareLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::readAllSensors(accel_sample_t* sample)
{
    accel_vector_raw_t raw_accel, raw_gyro;
    int16_t temp;
    int ret;
    
    if (!sample) {
        return kAccelErrInvalidParam;
    }
    
    ret = readAccelerometer(&raw_accel);
    if (ret != kAccelSuccess) {
        return ret;
    }
    
    if (fConfig.sensors_enable & kAccelSensorGyroscope) {
        ret = readGyroscope(&raw_gyro);
        if (ret != kAccelSuccess) {
            return ret;
        }
    }
    
    ret = readTemperature(&temp);
    if (ret != kAccelSuccess) {
        return ret;
    }
    
    /* Convert raw values to physical units */
    sample->accel.x = convertRawToG(raw_accel.x, fConfig.accel_range);
    sample->accel.y = convertRawToG(raw_accel.y, fConfig.accel_range);
    sample->accel.z = convertRawToG(raw_accel.z, fConfig.accel_range);
    
    sample->gyro.x = convertRawToDPS(raw_gyro.x, fConfig.gyro_range);
    sample->gyro.y = convertRawToDPS(raw_gyro.y, fConfig.gyro_range);
    sample->gyro.z = convertRawToDPS(raw_gyro.z, fConfig.gyro_range);
    
    sample->accel_temperature = temp / 326.0f + 25.0f;  /* Approximate conversion */
    
    sample->timestamp = getCurrentTimestamp();
    sample->sequence = ++fSampleCount;
    sample->sensors = fConfig.sensors_enable;
    sample->status = 0;
    sample->accuracy = 95;
    
    /* Apply calibration */
    applyCalibration(sample);
    
    return kAccelSuccess;
}

#pragma mark - FIFO Operations

int AppleEmbeddedAccelerometer::readFIFO(void)
{
    uint8_t fifo_status;
    uint8_t fifo_entries;
    accel_fifo_entry_t entry;
    int i;
    
    lck_mtx_lock(fHardwareLock);
    
    fifo_status = readRegister(0x4A);  /* FIFO_STATUS */
    fifo_entries = readRegister(0x4B);  /* FIFO_COUNT */
    
    if (fifo_status & 0x80) {
        /* FIFO overrun */
        fStats.fifo_overruns++;
    }
    
    for (i = 0; i < fifo_entries && fFIFOCount < ACCEL_MAX_FIFO_ENTRIES; i++) {
        /* Read FIFO entry (6-8 bytes) */
        entry.header = readRegister(0x4C);  /* FIFO_DATA */
        
        /* Read sensor data based on header */
        if (entry.header & 0x80) {
            /* This is a sensor data frame */
            entry.data.bytes[0] = readRegister(0x4C);
            entry.data.bytes[1] = readRegister(0x4C);
            entry.data.bytes[2] = readRegister(0x4C);
            entry.data.bytes[3] = readRegister(0x4C);
            entry.data.bytes[4] = readRegister(0x4C);
            entry.data.bytes[5] = readRegister(0x4C);
            
            processFIFOEntry(&entry);
            fFIFOCount++;
        } else if (entry.header == 0x40) {
            /* This is a timestamp frame */
            /* Read 3-byte timestamp */
            readRegister(0x4C);
            readRegister(0x4C);
            readRegister(0x4C);
        }
    }
    
    lck_mtx_unlock(fHardwareLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::processFIFOEntry(accel_fifo_entry_t* entry)
{
    accel_sample_t sample;
    accel_vector_raw_t raw;
    
    /* Parse FIFO entry based on header */
    raw.x = (int16_t)((entry->data.bytes[1] << 8) | entry->data.bytes[0]);
    raw.y = (int16_t)((entry->data.bytes[3] << 8) | entry->data.bytes[2]);
    raw.z = (int16_t)((entry->data.bytes[5] << 8) | entry->data.bytes[4]);
    
    /* Determine which sensor this is from the header */
    if (entry->header & 0x01) {
        /* Accelerometer data */
        sample.accel.x = convertRawToG(raw.x, fConfig.accel_range);
        sample.accel.y = convertRawToG(raw.y, fConfig.accel_range);
        sample.accel.z = convertRawToG(raw.z, fConfig.accel_range);
        sample.sensors |= kAccelSensorAccelerometer;
    }
    
    if (entry->header & 0x02) {
        /* Gyroscope data */
        sample.gyro.x = convertRawToDPS(raw.x, fConfig.gyro_range);
        sample.gyro.y = convertRawToDPS(raw.y, fConfig.gyro_range);
        sample.gyro.z = convertRawToDPS(raw.z, fConfig.gyro_range);
        sample.sensors |= kAccelSensorGyroscope;
    }
    
    sample.timestamp = getCurrentTimestamp();
    sample.sequence = ++fSampleCount;
    
    /* Queue the sample */
    queueSample(&sample);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::flushFIFO(void)
{
    lck_mtx_lock(fHardwareLock);
    
    /* Reset FIFO */
    writeRegister(0x48, 0x80);  /* Stream mode */
    writeRegister(0x48, 0x40);  /* Back to FIFO mode */
    
    fFIFOPosition = 0;
    fFIFOCount = 0;
    
    lck_mtx_unlock(fHardwareLock);
    
    return kAccelSuccess;
}

#pragma mark - Data Processing

float AppleEmbeddedAccelerometer::convertRawToG(int16_t raw, uint32_t range)
{
    /* Convert raw ADC value to g-force */
    float lsb_per_g = 32768.0f / range;
    return raw / lsb_per_g;
}

float AppleEmbeddedAccelerometer::convertRawToDPS(int16_t raw, uint32_t range)
{
    /* Convert raw ADC value to degrees per second */
    float lsb_per_dps = 32768.0f / range;
    return raw / lsb_per_dps;
}

void AppleEmbeddedAccelerometer::applyCalibration(accel_sample_t* sample)
{
    if (!sample) {
        return;
    }
    
    /* Apply accelerometer calibration */
    sample->accel.x = (sample->accel.x - fCalibration.accel_offset.x) * fCalibration.accel_scale.x;
    sample->accel.y = (sample->accel.y - fCalibration.accel_offset.y) * fCalibration.accel_scale.y;
    sample->accel.z = (sample->accel.z - fCalibration.accel_offset.z) * fCalibration.accel_scale.z;
    
    /* Apply gyroscope calibration */
    sample->gyro.x = (sample->gyro.x - fCalibration.gyro_offset.x) * fCalibration.gyro_scale.x;
    sample->gyro.y = (sample->gyro.y - fCalibration.gyro_offset.y) * fCalibration.gyro_scale.y;
    sample->gyro.z = (sample->gyro.z - fCalibration.gyro_offset.z) * fCalibration.gyro_scale.z;
}

void AppleEmbeddedAccelerometer::computeOrientation(accel_sample_t* sample)
{
    /* Simplified orientation computation */
    /* Would compute pitch, roll, etc. in real implementation */
}

#pragma mark - Motion Detection

int AppleEmbeddedAccelerometer::detectMotion(accel_sample_t* sample)
{
    static float prev_x = 0, prev_y = 0, prev_z = 0;
    float delta_x, delta_y, delta_z;
    float threshold = 0.05f;  /* 50 mg threshold */
    
    delta_x = fabsf(sample->accel.x - prev_x);
    delta_y = fabsf(sample->accel.y - prev_y);
    delta_z = fabsf(sample->accel.z - prev_z);
    
    if (delta_x > threshold || delta_y > threshold || delta_z > threshold) {
        /* Motion detected */
        prev_x = sample->accel.x;
        prev_y = sample->accel.y;
        prev_z = sample->accel.z;
        return 1;
    }
    
    prev_x = sample->accel.x;
    prev_y = sample->accel.y;
    prev_z = sample->accel.z;
    
    return 0;
}

int AppleEmbeddedAccelerometer::detectFreeFall(accel_sample_t* sample)
{
    float magnitude;
    
    magnitude = sqrtf(sample->accel.x * sample->accel.x +
                      sample->accel.y * sample->accel.y +
                      sample->accel.z * sample->accel.z);
    
    /* Free fall when magnitude < 0.3g */
    if (magnitude < 0.3f) {
        accel_motion_event_t event;
        event.timestamp = sample->timestamp;
        event.event_type = kAccelIntFreeFall;
        event.data.freefall.magnitude = magnitude;
        queueEvent(&event);
        return 1;
    }
    
    return 0;
}

int AppleEmbeddedAccelerometer::detectTap(accel_sample_t* sample)
{
    static uint64_t last_tap_time = 0;
    static int tap_count = 0;
    uint64_t now = sample->timestamp;
    
    /* Simplified tap detection based on sudden acceleration change */
    /* In real implementation, would use hardware tap detection */
    
    return 0;
}

int AppleEmbeddedAccelerometer::detectTilt(accel_sample_t* sample)
{
    /* Detect significant tilt change */
    static float last_angle = 0;
    float angle;
    
    angle = atan2f(sample->accel.y, sample->accel.z) * 180.0f / M_PI;
    
    if (fabsf(angle - last_angle) > 30.0f) {
        accel_motion_event_t event;
        event.timestamp = sample->timestamp;
        event.event_type = kAccelIntTilt;
        event.data.tilt.angle = angle;
        queueEvent(&event);
        last_angle = angle;
        return 1;
    }
    
    last_angle = angle;
    return 0;
}

#pragma mark - Pedometer

int AppleEmbeddedAccelerometer::updatePedometer(accel_sample_t* sample)
{
    static uint64_t last_step_time = 0;
    static int step_cadence = 0;
    uint64_t now = sample->timestamp;
    
    lck_mtx_lock(fPedometerLock);
    
    if (!fPedometerEnabled) {
        lck_mtx_unlock(fPedometerLock);
        return kAccelSuccess;
    }
    
    /* Simplified step detection based on vertical acceleration peaks */
    float vertical = sample->accel.z;  /* Assuming z is vertical */
    
    if (vertical > 1.2f) {  /* Step detected */
        uint64_t step_interval = now - last_step_time;
        
        if (step_interval > 200 * 1000 * 1000) {  /* 200 ms minimum between steps */
            fPedometer.step_count++;
            last_step_time = now;
            
            /* Calculate cadence (steps per minute) */
            if (step_interval > 0) {
                step_cadence = (int)(60.0f * 1000000000.0f / step_interval);
                fPedometer.step_rate = (fPedometer.step_rate + step_cadence) / 2;
            }
            
            /* Estimate distance (average stride ~0.75m) */
            fPedometer.distance = fPedometer.step_count * 0.75f;
            
            /* Estimate calories (rough approximation) */
            fPedometer.calories = fPedometer.distance * 0.06f;  /* ~60 cal/km */
            
            fPedometer.cadence = fPedometer.step_rate;
            fPedometer.confidence = 95;
        }
    }
    
    lck_mtx_unlock(fPedometerLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::resetPedometer(void)
{
    lck_mtx_lock(fPedometerLock);
    
    bzero(&fPedometer, sizeof(fPedometer));
    fPedometer.timestamp = getCurrentTimestamp();
    
    lck_mtx_unlock(fPedometerLock);
    
    return kAccelSuccess;
}

#pragma mark - Calibration

int AppleEmbeddedAccelerometer::calibrateAccelerometer(void)
{
    accel_sample_t samples[100];
    int i;
    float sum_x = 0, sum_y = 0, sum_z = 0;
    
    fStats.calibration_attempts++;
    
    /* Collect samples while stationary */
    for (i = 0; i < 100; i++) {
        readAllSensors(&samples[i]);
        IOSleep(10);
    }
    
    /* Average samples to find offset */
    for (i = 0; i < 100; i++) {
        sum_x += samples[i].accel.x;
        sum_y += samples[i].accel.y;
        sum_z += samples[i].accel.z;
    }
    
    /* For stationary device, expected acceleration is (0, 0, 1g) */
    fCalibration.accel_offset.x = sum_x / 100;
    fCalibration.accel_offset.y = sum_y / 100;
    fCalibration.accel_offset.z = (sum_z / 100) - 1.0f;
    
    fCalibration.calibration_time = getCurrentTimestamp();
    
    fStats.calibration_success++;
    
    saveCalibration();
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::calibrateGyroscope(void)
{
    accel_sample_t samples[100];
    int i;
    float sum_x = 0, sum_y = 0, sum_z = 0;
    
    /* Collect samples while stationary */
    for (i = 0; i < 100; i++) {
        readAllSensors(&samples[i]);
        IOSleep(10);
    }
    
    /* Average samples to find offset (should be 0 when stationary) */
    for (i = 0; i < 100; i++) {
        sum_x += samples[i].gyro.x;
        sum_y += samples[i].gyro.y;
        sum_z += samples[i].gyro.z;
    }
    
    fCalibration.gyro_offset.x = sum_x / 100;
    fCalibration.gyro_offset.y = sum_y / 100;
    fCalibration.gyro_offset.z = sum_z / 100;
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::loadCalibration(void)
{
    /* Load calibration from NVRAM or storage */
    /* In real implementation, would read from IORegistry or NVRAM */
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::saveCalibration(void)
{
    /* Save calibration to NVRAM */
    /* In real implementation, would write to IORegistry property */
    
    return kAccelSuccess;
}

#pragma mark - Queue Management

int AppleEmbeddedAccelerometer::queueSample(accel_sample_t* sample)
{
    lck_mtx_lock(fQueueLock);
    
    if (fQueueSize >= fQueueMaxSize) {
        /* Queue full, drop oldest */
        fQueueHead = (fQueueHead + 1) % fQueueMaxSize;
        fQueueSize--;
        fStats.samples_dropped++;
    }
    
    /* Add sample to queue */
    memcpy(&fSampleQueue[fQueueTail], sample, sizeof(accel_sample_t));
    fQueueTail = (fQueueTail + 1) % fQueueMaxSize;
    fQueueSize++;
    fStats.samples_collected++;
    
    lck_mtx_unlock(fQueueLock);
    
    /* Notify waiting clients */
    notifyClients();
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::dequeueSample(accel_sample_t* sample)
{
    int ret = kAccelErrNoData;
    
    if (!sample) {
        return kAccelErrInvalidParam;
    }
    
    lck_mtx_lock(fQueueLock);
    
    if (fQueueSize > 0) {
        memcpy(sample, &fSampleQueue[fQueueHead], sizeof(accel_sample_t));
        fQueueHead = (fQueueHead + 1) % fQueueMaxSize;
        fQueueSize--;
        ret = kAccelSuccess;
    }
    
    lck_mtx_unlock(fQueueLock);
    
    return ret;
}

int AppleEmbeddedAccelerometer::queueEvent(accel_motion_event_t* event)
{
    lck_mtx_lock(fEventLock);
    
    if ((fEventTail + 1) % fEventQueueSize == fEventHead) {
        /* Queue full, drop oldest */
        fEventHead = (fEventHead + 1) % fEventQueueSize;
    }
    
    memcpy(&fEventQueue[fEventTail], event, sizeof(accel_motion_event_t));
    fEventTail = (fEventTail + 1) % fEventQueueSize;
    
    lck_mtx_unlock(fEventLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::dequeueEvent(accel_motion_event_t* event)
{
    int ret = kAccelErrNoData;
    
    if (!event) {
        return kAccelErrInvalidParam;
    }
    
    lck_mtx_lock(fEventLock);
    
    if (fEventHead != fEventTail) {
        memcpy(event, &fEventQueue[fEventHead], sizeof(accel_motion_event_t));
        fEventHead = (fEventHead + 1) % fEventQueueSize;
        ret = kAccelSuccess;
    }
    
    lck_mtx_unlock(fEventLock);
    
    return ret;
}

int AppleEmbeddedAccelerometer::notifyClients(void)
{
    /* Notify clients via async notifications */
    /* In real implementation, would send mach message or similar */
    
    return kAccelSuccess;
}

#pragma mark - Timer and Interrupt Handlers

void AppleEmbeddedAccelerometer::timerFired(void)
{
    accel_sample_t sample;
    
    /* Read sensors periodically */
    if (fPowerState > 0 && (fConfig.power_mode != kAccelModePowerDown)) {
        readAllSensors(&sample);
        queueSample(&sample);
        
        /* Update pedometer if enabled */
        if (fPedometerEnabled) {
            updatePedometer(&sample);
        }
        
        /* Check for motion events */
        detectFreeFall(&sample);
        detectTilt(&sample);
    }
    
    /* Schedule next timer based on ODR */
    uint32_t interval_ms = 1000 / fConfig.odr;
    fTimerSource->setTimeoutMS(interval_ms);
}

void AppleEmbeddedAccelerometer::timerFired(OSObject* owner, IOTimerEventSource* sender)
{
    AppleEmbeddedAccelerometer* me = OSDynamicCast(AppleEmbeddedAccelerometer, owner);
    if (me) {
        me->timerFired();
    }
}

void AppleEmbeddedAccelerometer::interruptHandler(void)
{
    uint8_t int_status;
    
    lck_mtx_lock(fHardwareLock);
    
    int_status = readRegister(0x52);  /* INT_STATUS */
    fStats.interrupts++;
    
    /* Handle different interrupt sources */
    if (int_status & kAccelIntDataReady) {
        /* New data available */
        accel_sample_t sample;
        readAllSensors(&sample);
        queueSample(&sample);
    }
    
    if (int_status & kAccelIntFIFOReady) {
        /* FIFO threshold reached */
        readFIFO();
    }
    
    if (int_status & kAccelIntMotion) {
        /* Motion detected */
        accel_motion_event_t event;
        event.timestamp = getCurrentTimestamp();
        event.event_type = kAccelIntMotion;
        queueEvent(&event);
    }
    
    if (int_status & kAccelIntFreeFall) {
        /* Free fall detected */
        accel_motion_event_t event;
        event.timestamp = getCurrentTimestamp();
        event.event_type = kAccelIntFreeFall;
        queueEvent(&event);
    }
    
    lck_mtx_unlock(fHardwareLock);
}

void AppleEmbeddedAccelerometer::interruptHandler(OSObject* owner, 
                                                   IOInterruptEventSource* src,
                                                   int count)
{
    AppleEmbeddedAccelerometer* me = OSDynamicCast(AppleEmbeddedAccelerometer, owner);
    if (me) {
        me->interruptHandler();
    }
}

#pragma mark - Client Management

int AppleEmbeddedAccelerometer::registerClient(task_t task, uint32_t permissions, uint32_t* clientId)
{
    int i;
    
    lck_mtx_lock(fClientLock);
    
    /* Find free slot */
    for (i = 0; i < ACCEL_MAX_CLIENTS; i++) {
        if (!fClients[i].active) {
            break;
        }
    }
    
    if (i >= ACCEL_MAX_CLIENTS) {
        lck_mtx_unlock(fClientLock);
        return kAccelErrBusy;
    }
    
    /* Initialize client */
    fClients[i].task = task;
    fClients[i].pid = pid_from_task(task);
    fClients[i].clientId = fNextClientId++;
    fClients[i].permissions = permissions;
    fClients[i].sensorMask = 0;
    fClients[i].eventMask = 0;
    fClients[i].sampleRate = 0;
    fClients[i].queueSize = 0;
    fClients[i].active = true;
    fClients[i].callback = NULL;
    fClients[i].lastRead = 0;
    
    *clientId = fClients[i].clientId;
    fNumClients++;
    
    lck_mtx_unlock(fClientLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::unregisterClient(uint32_t clientId)
{
    int i;
    
    lck_mtx_lock(fClientLock);
    
    for (i = 0; i < ACCEL_MAX_CLIENTS; i++) {
        if (fClients[i].active && fClients[i].clientId == clientId) {
            bzero(&fClients[i], sizeof(fClients[i]));
            fNumClients--;
            break;
        }
    }
    
    lck_mtx_unlock(fClientLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::setClientConfig(uint32_t clientId, uint32_t sensorMask,
                                                  uint32_t eventMask, uint32_t sampleRate)
{
    int i;
    
    lck_mtx_lock(fClientLock);
    
    for (i = 0; i < ACCEL_MAX_CLIENTS; i++) {
        if (fClients[i].active && fClients[i].clientId == clientId) {
            fClients[i].sensorMask = sensorMask;
            fClients[i].eventMask = eventMask;
            fClients[i].sampleRate = sampleRate;
            fClients[i].lastRead = getCurrentUptime();
            break;
        }
    }
    
    lck_mtx_unlock(fClientLock);
    
    return kAccelSuccess;
}

#pragma mark - Utility Methods

uint64_t AppleEmbeddedAccelerometer::getCurrentTimestamp(void)
{
    clock_sec_t secs;
    clock_nsec_t nsecs;
    clock_get_calendar_nanotime(&secs, &nsecs);
    return (uint64_t)secs * NSEC_PER_SEC + nsecs;
}

uint64_t AppleEmbeddedAccelerometer::getCurrentUptime(void)
{
    uint64_t uptime;
    clock_get_uptime(&uptime);
    return uptime;
}

bool AppleEmbeddedAccelerometer::isEntitled(task_t task, const char* entitlement)
{
    /* Check if task has entitlement */
    /* In real implementation, would query AMFI or MAC policy */
    return true;
}

int AppleEmbeddedAccelerometer::validateClient(uint32_t clientId, uint32_t requiredPerms)
{
    int i;
    
    lck_mtx_lock(fClientLock);
    
    for (i = 0; i < ACCEL_MAX_CLIENTS; i++) {
        if (fClients[i].active && fClients[i].clientId == clientId) {
            if ((fClients[i].permissions & requiredPerms) == requiredPerms) {
                lck_mtx_unlock(fClientLock);
                return kAccelSuccess;
            }
            break;
        }
    }
    
    lck_mtx_unlock(fClientLock);
    
    return kAccelErrAccessDenied;
}

#pragma mark - Public API

int AppleEmbeddedAccelerometer::getLatestSample(accel_sample_t* sample)
{
    return dequeueSample(sample);
}

int AppleEmbeddedAccelerometer::getSamples(accel_sample_t* samples, uint32_t* count)
{
    uint32_t max_count = *count;
    uint32_t i;
    
    for (i = 0; i < max_count; i++) {
        if (dequeueSample(&samples[i]) != kAccelSuccess) {
            break;
        }
    }
    
    *count = i;
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::getMotionEvents(accel_motion_event_t* events, uint32_t* count)
{
    uint32_t max_count = *count;
    uint32_t i;
    
    for (i = 0; i < max_count; i++) {
        if (dequeueEvent(&events[i]) != kAccelSuccess) {
            break;
        }
    }
    
    *count = i;
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::setConfig(accel_config_t* config)
{
    if (!config) {
        return kAccelErrInvalidParam;
    }
    
    memcpy(&fConfig, config, sizeof(accel_config_t));
    
    /* Apply configuration to hardware */
    setODR(fConfig.odr);
    setAccelRange(fConfig.accel_range);
    setGyroRange(fConfig.gyro_range);
    setPowerMode(fConfig.power_mode);
    configureFIFO();
    configureInterrupts();
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::getConfig(accel_config_t* config)
{
    if (!config) {
        return kAccelErrInvalidParam;
    }
    
    memcpy(config, &fConfig, sizeof(accel_config_t));
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::setCalibration(accel_calibration_t* cal)
{
    if (!cal) {
        return kAccelErrInvalidParam;
    }
    
    memcpy(&fCalibration, cal, sizeof(accel_calibration_t));
    saveCalibration();
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::getCalibration(accel_calibration_t* cal)
{
    if (!cal) {
        return kAccelErrInvalidParam;
    }
    
    memcpy(cal, &fCalibration, sizeof(accel_calibration_t));
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::getStatistics(accel_statistics_t* stats)
{
    if (!stats) {
        return kAccelErrInvalidParam;
    }
    
    lck_mtx_lock(fStatsLock);
    memcpy(stats, &fStats, sizeof(accel_statistics_t));
    lck_mtx_unlock(fStatsLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::resetStatistics(void)
{
    lck_mtx_lock(fStatsLock);
    bzero(&fStats, sizeof(fStats));
    lck_mtx_unlock(fStatsLock);
    
    return kAccelSuccess;
}

int AppleEmbeddedAccelerometer::runSelfTest(void)
{
    uint8_t result;
    
    lck_mtx_lock(fHardwareLock);
    
    /* Trigger self test */
    writeRegister(0x3D, 0x01);  /* SELF_TEST register */
    
    /* Wait for completion */
    waitForBit(0x3D, 0x01, false, 100);
    
    /* Read result */
    result = readRegister(0x3E);  /* SELF_TEST_RESULT */
    
    lck_mtx_unlock(fHardwareLock);
    
    return (result == 0) ? kAccelSuccess : kAccelErrGeneral;
}

int AppleEmbeddedAccelerometer::runFactoryCalibration(void)
{
    calibrateAccelerometer();
    calibrateGyroscope();
    
    return kAccelSuccess;
}

#pragma mark - IOService Overrides

bool AppleEmbeddedAccelerometer::start(IOService* provider)
{
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("AppleEmbeddedAccelerometer: Starting\n");
    
    /* Create work loop */
    if (!createWorkLoop()) {
        IOLog("AppleAccelerometer: Failed to create work loop\n");
        return false;
    }
    
    /* Get transport provider */
    fTransportProvider = provider;
    
    /* Get memory map from provider */
    IODeviceMemory* memory = provider->getDeviceMemoryWithIndex(0);
    if (memory) {
        fPhysicalAddress = memory->getPhysicalAddress();
        fPhysicalLength = memory->getLength();
        
        fMemoryMap = memory->makeMapping(kIOMapAny | kIOMapInhibitCache);
        if (fMemoryMap) {
            fRegisters = (volatile uint8_t*)fMemoryMap->getVirtualAddress();
        }
    }
    
    /* Probe hardware to identify chip */
    if (!probeHardware()) {
        IOLog("AppleAccelerometer: Hardware probe failed\n");
        destroyWorkLoop();
        return false;
    }
    
    /* Initialize hardware */
    if (!initializeHardware()) {
        IOLog("AppleAccelerometer: Hardware initialization failed\n");
        destroyWorkLoop();
        return false;
    }
    
    /* Load calibration data */
    loadCalibration();
    
    /* Register power management */
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, NULL, 0);
    
    /* Start timer */
    if (fTimerSource) {
        fTimerSource->setTimeoutMS(1000 / fConfig.odr);
    }
    
    /* Publish properties */
    char hw_str[32];
    snprintf(hw_str, sizeof(hw_str), "0x%02x Rev 0x%02x", fChipID, fHWRevision);
    setProperty("Hardware Version", hw_str);
    setProperty("Hardware Type", fHWType, 32);
    setProperty("ODR", fConfig.odr, 32);
    setProperty("Accel Range", fConfig.accel_range, 32);
    setProperty("Gyro Range", fConfig.gyro_range, 32);
    
    /* Register service */
    registerService();
    
    IOLog("AppleEmbeddedAccelerometer: Started successfully\n");
    
    return true;
}

void AppleEmbeddedAccelerometer::stop(IOService* provider)
{
    IOLog("AppleEmbeddedAccelerometer: Stopping\n");
    
    /* Stop timer */
    if (fTimerSource) {
        fTimerSource->cancelTimeout();
    }
    
    /* Shutdown hardware */
    shutdownHardware();
    
    /* Destroy work loop */
    destroyWorkLoop();
    
    /* Unmap registers */
    if (fMemoryMap) {
        fMemoryMap->release();
        fMemoryMap = NULL;
        fRegisters = NULL;
    }
    
    /* Power management */
    PMstop();
    
    super::stop(provider);
}

IOReturn AppleEmbeddedAccelerometer::setPowerState(unsigned long powerState,
                                                    IOService* device)
{
    if (powerState == 0) {
        /* Going to sleep */
        if (fConfig.auto_sleep) {
            setPowerMode(kAccelModeLowPower);
        }
        fPowerState = 0;
    } else {
        /* Waking up */
        setPowerMode(fConfig.power_mode);
        fPowerState = 1;
    }
    
    return IOPMAckImplied;
}

IOReturn AppleEmbeddedAccelerometer::powerStateWillChangeTo(IOPMPowerFlags capabilities,
                                                              unsigned long stateNumber,
                                                              IOService* whatDevice)
{
    return IOPMAckImplied;
}

bool AppleEmbeddedAccelerometer::didTerminate(IOService* provider, IOOptionBits options,
                                                bool* defer)
{
    *defer = false;
    return true;
}

IOReturn AppleEmbeddedAccelerometer::newUserClient(task_t owningTask,
                                                     void* securityID,
                                                     UInt32 type,
                                                     OSDictionary* properties,
                                                     IOUserClient** handler)
{
    AppleAccelerometerUserClient* client;
    IOReturn ret = kIOReturnSuccess;
    
    /* Check entitlements */
    if (!isEntitled(owningTask, "com.apple.private.accelerometer")) {
        IOLog("AppleAccelerometer: Unauthorized client attempted connection\n");
        return kIOReturnNotPermitted;
    }
    
    client = new AppleAccelerometerUserClient;
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
    
    return ret;
}

/*==============================================================================
 * AppleAccelerometerUserClient Implementation
 *==============================================================================*/

bool AppleAccelerometerUserClient::init(OSDictionary* dictionary)
{
    if (!IOUserClient::init(dictionary)) {
        return false;
    }
    
    fTask = NULL;
    fPid = 0;
    fClientId = 0;
    fProvider = NULL;
    fPermissions = 0;
    fState = 0;
    fNotificationPort = MACH_PORT_NULL;
    fNotificationRef = 0;
    fSharedMemory = NULL;
    fSharedBuffer = NULL;
    fSharedSize = 0;
    fValid = false;
    
    return true;
}

void AppleAccelerometerUserClient::free(void)
{
    if (fSharedMemory) {
        fSharedMemory->release();
        fSharedMemory = NULL;
    }
    
    IOUserClient::free();
}

bool AppleAccelerometerUserClient::start(IOService* provider)
{
    if (!IOUserClient::start(provider)) {
        return false;
    }
    
    fProvider = OSDynamicCast(AppleEmbeddedAccelerometer, provider);
    if (!fProvider) {
        return false;
    }
    
    fTask = current_task();
    fPid = pid_from_task(fTask);
    
    /* Determine permissions */
    fPermissions = 0x01;  /* Basic read */
    
    /* Register with provider */
    fProvider->registerClient(fTask, fPermissions, &fClientId);
    
    fValid = true;
    
    return true;
}

void AppleAccelerometerUserClient::stop(IOService* provider)
{
    if (fValid && fProvider && fClientId) {
        fProvider->unregisterClient(fClientId);
    }
    
    fValid = false;
    
    IOUserClient::stop(provider);
}

IOReturn AppleAccelerometerUserClient::clientClose(void)
{
    if (fValid && fProvider && fClientId) {
        fProvider->unregisterClient(fClientId);
    }
    
    fValid = false;
    
    terminate();
    
    return kIOReturnSuccess;
}

IOReturn AppleAccelerometerUserClient::clientDied(void)
{
    return clientClose();
}

IOReturn AppleAccelerometerUserClient::registerNotificationPort(mach_port_t port,
                                                                  UInt32 type,
                                                                  UInt32 refCon)
{
    fNotificationPort = port;
    fNotificationRef = refCon;
    
    return kIOReturnSuccess;
}

IOReturn AppleAccelerometerUserClient::externalMethod(uint32_t selector,
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

IOReturn AppleAccelerometerUserClient::sOpenSession(AppleAccelerometerUserClient* target,
                                                     void* reference,
                                                     IOExternalMethodArguments* args)
{
    uint32_t sensorMask = (uint32_t)args->scalarInput[0];
    uint32_t eventMask = (uint32_t)args->scalarInput[1];
    uint32_t sampleRate = (uint32_t)args->scalarInput[2];
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    target->fProvider->setClientConfig(target->fClientId, sensorMask, eventMask, sampleRate);
    
    args->scalarOutput[0] = target->fClientId;
    
    return kIOReturnSuccess;
}

IOReturn AppleAccelerometerUserClient::sCloseSession(AppleAccelerometerUserClient* target,
                                                      void* reference,
                                                      IOExternalMethodArguments* args)
{
    if (!target || !target->fValid) {
        return kIOReturnNotAttached;
    }
    
    /* Close session */
    
    return kIOReturnSuccess;
}

IOReturn AppleAccelerometerUserClient::sSetConfig(AppleAccelerometerUserClient* target,
                                                    void* reference,
                                                    IOExternalMethodArguments* args)
{
    accel_config_t config;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    config.odr = (uint32_t)args->scalarInput[0];
    config.accel_range = (uint32_t)args->scalarInput[1];
    config.gyro_range = (uint32_t)args->scalarInput[2];
    config.power_mode = (uint32_t)args->scalarInput[3];
    config.fifo_mode = (uint32_t)args->scalarInput[4];
    config.fifo_threshold = (uint32_t)args->scalarInput[5];
    
    return target->fProvider->setConfig(&config);
}

IOReturn AppleAccelerometerUserClient::sGetConfig(AppleAccelerometerUserClient* target,
                                                    void* reference,
                                                    IOExternalMethodArguments* args)
{
    accel_config_t* config = (accel_config_t*)args->structureOutput;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    return target->fProvider->getConfig(config);
}

IOReturn AppleAccelerometerUserClient::sGetSample(AppleAccelerometerUserClient* target,
                                                    void* reference,
                                                    IOExternalMethodArguments* args)
{
    accel_sample_t* sample = (accel_sample_t*)args->structureOutput;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    return target->fProvider->getLatestSample(sample);
}

IOReturn AppleAccelerometerUserClient::sGetSamples(AppleAccelerometerUserClient* target,
                                                     void* reference,
                                                     IOExternalMethodArguments* args)
{
    uint32_t maxCount = (uint32_t)args->scalarInput[0];
    uint32_t count = maxCount;
    IOReturn ret;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    ret = target->fProvider->getSamples((accel_sample_t*)args->structureOutput, &count);
    args->scalarOutput[0] = count;
    
    return ret;
}

IOReturn AppleAccelerometerUserClient::sGetEvent(AppleAccelerometerUserClient* target,
                                                   void* reference,
                                                   IOExternalMethodArguments* args)
{
    accel_motion_event_t* event = (accel_motion_event_t*)args->structureOutput;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    return target->fProvider->getMotionEvents(event, (uint32_t*)&args->scalarOutput[0]);
}

IOReturn AppleAccelerometerUserClient::sGetPedometer(AppleAccelerometerUserClient* target,
                                                       void* reference,
                                                       IOExternalMethodArguments* args)
{
    return kIOReturnUnsupported;
}

IOReturn AppleAccelerometerUserClient::sSetCalibration(AppleAccelerometerUserClient* target,
                                                         void* reference,
                                                         IOExternalMethodArguments* args)
{
    accel_calibration_t* cal = (accel_calibration_t*)args->structureInput;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    return target->fProvider->setCalibration(cal);
}

IOReturn AppleAccelerometerUserClient::sGetCalibration(AppleAccelerometerUserClient* target,
                                                         void* reference,
                                                         IOExternalMethodArguments* args)
{
    accel_calibration_t* cal = (accel_calibration_t*)args->structureOutput;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    return target->fProvider->getCalibration(cal);
}

IOReturn AppleAccelerometerUserClient::sRunSelfTest(AppleAccelerometerUserClient* target,
                                                      void* reference,
                                                      IOExternalMethodArguments* args)
{
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    args->scalarOutput[0] = target->fProvider->runSelfTest();
    
    return kIOReturnSuccess;
}

IOReturn AppleAccelerometerUserClient::sGetStatistics(AppleAccelerometerUserClient* target,
                                                        void* reference,
                                                        IOExternalMethodArguments* args)
{
    accel_statistics_t* stats = (accel_statistics_t*)args->structureOutput;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    return target->fProvider->getStatistics(stats);
}
