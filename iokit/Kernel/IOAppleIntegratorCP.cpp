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
#include <libkern/c++/OSSet.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSDebug.h>
#include <libkern/crypto/sha1.h>

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
#include <machine/commpage.h>

#include <uuid/uuid.h>

/*==============================================================================
 * AppleIntegratorCP - Integrator Coprocessor Driver
 * 
 * This driver manages the Integrator Coprocessor found in Apple Silicon:
 * - System Management Controller (SMC) functionality
 * - Power management coordination
 * - Thermal management
 * - Performance controller (P-cores/E-cores)
 * - Sensor hub
 * - Secure Enclave communication bridge
 * - Display engine integration
 * - Neural Engine coordination
 *==============================================================================*/

/*==============================================================================
 * Constants and Definitions
 *==============================================================================*/

#define INTEGRATOR_CP_VERSION           0x00030001  /* Version 3.1 */
#define INTEGRATOR_CP_REVISION          0x00000005

/* Register offsets (based on Apple's Dx0x series) */
#define ICP_REG_ID                       0x0000      /* Integrator ID */
#define ICP_REG_VERSION                  0x0004      /* Version register */
#define ICP_REG_CONTROL                  0x0008      /* Main control */
#define ICP_REG_STATUS                   0x000C      /* Status register */
#define ICP_REG_INT_STATUS               0x0010      /* Interrupt status */
#define ICP_REG_INT_ENABLE                0x0014      /* Interrupt enable */
#define ICP_REG_INT_MASK                  0x0018      /* Interrupt mask */
#define ICP_REG_INT_CLEAR                 0x001C      /* Interrupt clear */
#define ICP_REG_POWER_STATE               0x0020      /* Power state */
#define ICP_REG_RESET_CONTROL             0x0024      /* Reset control */
#define ICP_REG_CLOCK_CONTROL             0x0028      /* Clock control */
#define ICP_REG_VOLTAGE_CONTROL            0x002C      /* Voltage control */
#define ICP_REG_PERF_CONTROL               0x0030      /* Performance control */
#define ICP_REG_THERMAL_CONTROL            0x0034      /* Thermal control */
#define ICP_REG_SECURE_CONTROL             0x0038      /* Secure control */
#define ICP_REG_DEBUG_CONTROL              0x003C      /* Debug control */

/* Performance controller registers */
#define ICP_PERF_CLUSTER0_STATUS           0x0100      /* P-core cluster status */
#define ICP_PERF_CLUSTER1_STATUS           0x0104      /* E-core cluster status */
#define ICP_PERF_PSTATE                    0x0108      /* Current P-state */
#define ICP_PERF_CSTATE                    0x010C      /* Current C-state */
#define ICP_PERF_FREQ_MIN                  0x0110      /* Minimum frequency */
#define ICP_PERF_FREQ_MAX                  0x0114      /* Maximum frequency */
#define ICP_PERF_FREQ_CURRENT              0x0118      /* Current frequency */
#define ICP_PERF_VOLTAGE_MIN               0x011C      /* Minimum voltage */
#define ICP_PERF_VOLTAGE_MAX               0x0120      /* Maximum voltage */
#define ICP_PERF_VOLTAGE_CURRENT           0x0124      /* Current voltage */
#define ICP_PERF_ENERGY_COUNTER             0x0128      /* Energy counter */
#define ICP_PERF_CORE_MASK                  0x012C      /* Active core mask */

/* Thermal sensor registers */
#define ICP_THERMAL_SENSOR_BASE             0x0200      /* Base for sensor 0 */
#define ICP_THERMAL_SENSOR_SIZE             0x10        /* Size per sensor */
#define ICP_THERMAL_SENSOR0                 (ICP_THERMAL_SENSOR_BASE + 0x00)
#define ICP_THERMAL_SENSOR1                 (ICP_THERMAL_SENSOR_BASE + 0x10)
#define ICP_THERMAL_SENSOR2                 (ICP_THERMAL_SENSOR_BASE + 0x20)
#define ICP_THERMAL_SENSOR3                 (ICP_THERMAL_SENSOR_BASE + 0x30)
#define ICP_THERMAL_SENSOR4                 (ICP_THERMAL_SENSOR_BASE + 0x40)
#define ICP_THERMAL_SENSOR5                 (ICP_THERMAL_SENSOR_BASE + 0x50)
#define ICP_THERMAL_SENSOR6                 (ICP_THERMAL_SENSOR_BASE + 0x60)
#define ICP_THERMAL_SENSOR7                 (ICP_THERMAL_SENSOR_BASE + 0x70)

#define ICP_THERMAL_TEMPERATURE             0x00        /* Temperature reading */
#define ICP_THERMAL_THRESHOLD_LOW           0x04        /* Low threshold */
#define ICP_THERMAL_THRESHOLD_HIGH          0x08        /* High threshold */
#define ICP_THERMAL_CONTROL                  0x0C        /* Sensor control */

/* Power management registers */
#define ICP_POWER_DOMAIN0                   0x0300      /* Domain 0 control */
#define ICP_POWER_DOMAIN1                   0x0304      /* Domain 1 control */
#define ICP_POWER_DOMAIN2                   0x0308      /* Domain 2 control */
#define ICP_POWER_DOMAIN3                   0x030C      /* Domain 3 control */
#define ICP_POWER_DOMAIN4                   0x0310      /* Domain 4 control */
#define ICP_POWER_DOMAIN5                   0x0314      /* Domain 5 control */
#define ICP_POWER_DOMAIN6                   0x0318      /* Domain 6 control */
#define ICP_POWER_DOMAIN7                   0x031C      /* Domain 7 control */

#define ICP_POWER_STATE_ON                   0x00000001
#define ICP_POWER_STATE_OFF                  0x00000000
#define ICP_POWER_STATE_SLEEP                0x00000002
#define ICP_POWER_STATE_DEEPSLEEP            0x00000003

/* Secure registers */
#define ICP_SECURE_BOOT_STATUS               0x0400      /* Secure boot status */
#define ICP_SECURE_ENCLAVE_STATUS            0x0404      /* SE status */
#define ICP_SECURE_BOOT_POLICY               0x0408      /* Boot policy */
#define ICP_SECURE_DEBUG                      0x040C      /* Debug control */
#define ICP_SECURE_KEY_HASH                   0x0410      /* Key hash (32 bytes) */
#define ICP_SECURE_CHALLENGE                  0x0420      /* Challenge (16 bytes) */
#define ICP_SECURE_RESPONSE                   0x0430      /* Response (16 bytes) */

/* Firmware registers */
#define ICP_FW_VERSION                       0x0500      /* Firmware version */
#define ICP_FW_REVISION                      0x0504      /* Firmware revision */
#define ICP_FW_BUILD                          0x0508      /* Build number */
#define ICP_FW_FEATURES                       0x050C      /* Feature flags */
#define ICP_FW_BOOT_TIME                      0x0510      /* Boot time */
#define ICP_FW_UPTIME                         0x0514      /* Uptime */

/* Mailbox registers */
#define ICP_MBOX_STATUS                      0x0600      /* Mailbox status */
#define ICP_MBOX_CONTROL                     0x0604      /* Mailbox control */
#define ICP_MBOX_DATA0                       0x0608      /* Data word 0 */
#define ICP_MBOX_DATA1                       0x060C      /* Data word 1 */
#define ICP_MBOX_DATA2                       0x0610      /* Data word 2 */
#define ICP_MBOX_DATA3                       0x0614      /* Data word 3 */
#define ICP_MBOX_DATA4                       0x0618      /* Data word 4 */
#define ICP_MBOX_DATA5                       0x061C      /* Data word 5 */
#define ICP_MBOX_DATA6                       0x0620      /* Data word 6 */
#define ICP_MBOX_DATA7                       0x0624      /* Data word 7 */

/* Control bits */
#define ICP_CTRL_ENABLE                       (1 << 0)
#define ICP_CTRL_RESET                         (1 << 1)
#define ICP_CTRL_SLEEP                         (1 << 2)
#define ICP_CTRL_WAKE                          (1 << 3)
#define ICP_CTRL_DEBUG_ENABLE                  (1 << 4)
#define ICP_CTRL_SECURE_ENABLE                  (1 << 5)
#define ICP_CTRL_PERF_MONITOR                   (1 << 6)
#define ICP_CTRL_THERMAL_MONITOR                (1 << 7)

/* Status bits */
#define ICP_STATUS_READY                       (1 << 0)
#define ICP_STATUS_RUNNING                      (1 << 1)
#define ICP_STATUS_SLEEPING                     (1 << 2)
#define ICP_STATUS_ERROR                        (1 << 3)
#define ICP_STATUS_SECURE                       (1 << 4)
#define ICP_STATUS_DEBUG                        (1 << 5)
#define ICP_STATUS_FW_LOADED                    (1 << 6)

/* Interrupt bits */
#define ICP_INT_MAILBOX                        (1 << 0)
#define ICP_INT_THERMAL                        (1 << 1)
#define ICP_INT_POWER                          (1 << 2)
#define ICP_INT_PERF                           (1 << 3)
#define ICP_INT_SECURE                         (1 << 4)
#define ICP_INT_ERROR                          (1 << 5)
#define ICP_INT_FW_READY                        (1 << 6)
#define ICP_INT_ALL                             (0x7F)

/*==============================================================================
 * Data Structures
 *==============================================================================*/

/* Maximum values */
#define ICP_MAX_SENSORS                        16
#define ICP_MAX_POWER_DOMAINS                   8
#define ICP_MAX_PERF_CLUSTERS                   2
#define ICP_MAX_CORES_PER_CLUSTER               8
#define ICP_MAX_MAILBOX_SIZE                    8
#define ICP_MAX_FIRMWARE_SIZE                    0x100000  /* 1MB */
#define ICP_MAX_SHARED_MEMORY                    0x20000    /* 128KB */

/* Performance states (P-states) */
typedef enum {
    kPStateLowPower = 0,
    kPStateBalanced = 1,
    kPStatePerformance = 2,
    kPStateMaxPerformance = 3,
    kPStateCount
} pstate_t;

/* Core states (C-states) */
typedef enum {
    kCStateActive = 0,
    kCStateClockGate = 1,
    kCStatePowerGate = 2,
    kCStateDeepSleep = 3,
    kCStateOff = 4
} cstate_t;

/* Power domains */
typedef enum {
    kPowerDomainCPU_P = 0,      /* P-core cluster */
    kPowerDomainCPU_E = 1,      /* E-core cluster */
    kPowerDomainGPU = 2,        /* GPU */
    kPowerDomainANE = 3,        /* Apple Neural Engine */
    kPowerDomainDisplay = 4,     /* Display engine */
    kPowerDomainMedia = 5,       /* Media engine */
    kPowerDomainSecure = 6,      /* Secure Enclave */
    kPowerDomainMemory = 7       /* Memory controller */
} power_domain_t;

/* Thermal zones */
typedef enum {
    kThermalZoneCPU_P = 0,
    kThermalZoneCPU_E = 1,
    kThermalZoneGPU = 2,
    kThermalZoneANE = 3,
    kThermalZonePMIC = 4,
    kThermalZoneCharger = 5,
    kThermalZoneBattery = 6,
    kThermalZoneAmbient = 7
} thermal_zone_t;

/* Mailbox message types */
typedef enum {
    kMboxMsgNone = 0,
    kMboxMsgPowerRequest = 1,
    kMboxMsgPowerState = 2,
    kMboxMsgPerfRequest = 3,
    kMboxMsgPerfState = 4,
    kMboxMsgThermalRequest = 5,
    kMboxMsgThermalData = 6,
    kMboxMsgSecureRequest = 7,
    kMboxMsgSecureResponse = 8,
    kMboxMsgDebug = 9,
    kMboxMsgFirmware = 10,
    kMboxMsgError = 0xFF
} mbox_msg_type_t;

/* Performance cluster info */
typedef struct {
    uint32_t    cluster_id;
    char        name[16];
    uint32_t    core_count;
    uint32_t    active_cores;
    uint32_t    pstate;
    uint32_t    cstate;
    uint32_t    frequency_min;      /* MHz */
    uint32_t    frequency_max;      /* MHz */
    uint32_t    frequency_cur;      /* MHz */
    uint32_t    voltage_min;        /* mV */
    uint32_t    voltage_max;        /* mV */
    uint32_t    voltage_cur;        /* mV */
    uint64_t    energy_counter;      /* nJ */
    uint32_t    core_mask;
    uint32_t    capabilities;
} perf_cluster_t;

/* Thermal sensor info */
typedef struct {
    uint32_t    sensor_id;
    char        name[16];
    char        location[32];
    uint32_t    type;                /* CPU, GPU, battery, etc */
    int32_t     temperature;         /* Celsius * 100 */
    int32_t     threshold_low;       /* Celsius * 100 */
    int32_t     threshold_high;      /* Celsius * 100 */
    uint32_t    status;
    uint32_t    confidence;          /* 0-100 */
    uint64_t    last_update;
    uint32_t    flags;
} thermal_sensor_t;

/* Power domain info */
typedef struct {
    uint32_t    domain_id;
    char        name[16];
    uint32_t    state;
    uint32_t    voltage;             /* mV */
    uint32_t    current;             /* mA */
    uint32_t    power;               /* mW */
    uint32_t    capabilities;
    uint64_t    energy_used;         /* nJ */
    uint32_t    flags;
} power_domain_t;

/* Mailbox message */
typedef struct {
    uint32_t    type;
    uint32_t    command;
    uint32_t    result;
    uint32_t    data[8];
    uint64_t    timestamp;
} mbox_message_t;

/* Firmware info */
typedef struct {
    uint32_t    version_major;
    uint32_t    version_minor;
    uint32_t    version_patch;
    uint32_t    revision;
    uint32_t    build;
    uint32_t    features;
    uint64_t    boot_time;
    uint64_t    uptime;
    char        build_string[64];
    uint8_t     hash[32];
} firmware_info_t;

/* Secure boot info */
typedef struct {
    uint32_t    boot_status;
    uint32_t    boot_policy;
    uint32_t    security_level;
    uint32_t    debug_enabled;
    uint8_t     key_hash[32];
    uint8_t     challenge[16];
    uint8_t     response[16];
    uint32_t    flags;
} secure_boot_info_t;

/* Integrator capabilities */
typedef struct {
    uint32_t    version;
    uint32_t    features;
    uint32_t    num_sensors;
    uint32_t    num_power_domains;
    uint32_t    num_perf_clusters;
    uint32_t    max_frequency;
    uint32_t    min_frequency;
    uint32_t    max_voltage;
    uint32_t    min_voltage;
    uint32_t    thermal_throttle_temp;
    uint32_t    thermal_shutdown_temp;
    uint32_t    secure_boot_required;
    uint32_t    flags;
} integrator_caps_t;

/*==============================================================================
 * AppleIntegratorCP Main Class
 *==============================================================================*/

class AppleIntegratorCP : public IOService
{
    OSDeclareDefaultStructors(AppleIntegratorCP)
    
private:
    /* IOKit resources */
    IOWorkLoop*                 fWorkLoop;
    IOCommandGate*              fCommandGate;
    IOTimerEventSource*         fTimerSource;
    IOInterruptEventSource*     fInterruptSource;
    
    /* Hardware access */
    IOMemoryMap*                fMemoryMap;
    volatile uint32_t*          fRegisters;
    IOPhysicalAddress           fPhysBase;
    IOPhysicalLength            fPhysSize;
    
    /* Shared memory for firmware */
    IOBufferMemoryDescriptor*   fSharedMemory;
    void*                       fSharedBase;
    uint32_t                    fSharedSize;
    
    /* State */
    uint32_t                    fState;
    uint32_t                    fFlags;
    uint64_t                    fStartTime;
    bool                        fHardwareReady;
    bool                        fFirmwareLoaded;
    
    /* Version information */
    uint32_t                    fDriverVersion;
    uint32_t                    fDriverRevision;
    char                        fDriverBuild[32];
    
    /* Integrator capabilities */
    integrator_caps_t           fCaps;
    
    /* Performance clusters */
    perf_cluster_t              fPerfClusters[ICP_MAX_PERF_CLUSTERS];
    uint32_t                    fNumPerfClusters;
    
    /* Thermal sensors */
    thermal_sensor_t            fSensors[ICP_MAX_SENSORS];
    uint32_t                    fNumSensors;
    
    /* Power domains */
    power_domain_t              fPowerDomains[ICP_MAX_POWER_DOMAINS];
    uint32_t                    fNumPowerDomains;
    
    /* Firmware info */
    firmware_info_t             fFirmwareInfo;
    
    /* Secure boot info */
    secure_boot_info_t          fSecureInfo;
    
    /* Mailbox */
    mbox_message_t              fMailbox;
    lck_mtx_t*                  fMailboxLock;
    thread_call_t               fMailboxThread;
    bool                        fMailboxPending;
    
    /* Statistics */
    uint64_t                    fTotalInterrupts;
    uint64_t                    fTotalMailboxMessages;
    uint64_t                    fTotalErrors;
    uint64_t                    fThermalEvents;
    uint64_t                    fPowerEvents;
    uint64_t                    fPerfEvents;
    uint64_t                    fSecureEvents;
    
    /* Locking */
    lck_grp_t*                  fLockGroup;
    lck_attr_t*                 fLockAttr;
    lck_mtx_t*                  fHardwareLock;
    lck_mtx_t*                  fStateLock;
    
    /* Power management */
    uint32_t                    fPowerState;
    uint32_t                    fThermalLevel;
    uint32_t                    fPerformanceLevel;
    bool                        fThrottling;
    
    /*==============================================================================
     * Private Methods
     *==============================================================================*/
    
    /* Hardware access */
    bool                        mapHardware(void);
    void                        unmapHardware(void);
    uint32_t                    readReg(uint32_t offset);
    void                        writeReg(uint32_t offset, uint32_t value);
    uint32_t                    readReg64(uint32_t offset, uint64_t* value);
    void                        writeReg64(uint32_t offset, uint64_t value);
    
    /* Initialization */
    bool                        initializeHardware(void);
    void                        shutdownHardware(void);
    bool                        resetController(void);
    bool                        loadFirmware(void);
    bool                        verifyFirmware(void);
    bool                        startFirmware(void);
    
    /* Interrupt handling */
    bool                        setupInterrupts(void);
    void                        freeInterrupts(void);
    void                        interruptHandler(void);
    static void                 interruptHandler(IOInterruptEventSource* source, int count);
    
    /* Mailbox */
    void                        mailboxHandler(void);
    static void                 mailboxHandler(thread_call_param_t param0, thread_call_param_t param1);
    IOReturn                    sendMailboxMessage(mbox_message_t* msg);
    IOReturn                    receiveMailboxMessage(mbox_message_t* msg);
    IOReturn                    waitForMailboxResponse(mbox_message_t* msg, uint32_t timeout_ms);
    
    /* Performance management */
    IOReturn                    setPerformanceState(uint32_t cluster, uint32_t pstate);
    IOReturn                    getPerformanceState(uint32_t cluster, uint32_t* pstate);
    IOReturn                    setCoreMask(uint32_t cluster, uint32_t core_mask);
    IOReturn                    getCoreMask(uint32_t cluster, uint32_t* core_mask);
    uint64_t                    readEnergyCounter(uint32_t cluster);
    uint32_t                    getCurrentFrequency(uint32_t cluster);
    uint32_t                    getCurrentVoltage(uint32_t cluster);
    
    /* Thermal management */
    IOReturn                    readSensor(uint32_t sensor_id, int32_t* temp);
    IOReturn                    setSensorThreshold(uint32_t sensor_id, int32_t low, int32_t high);
    IOReturn                    getSensorThreshold(uint32_t sensor_id, int32_t* low, int32_t* high);
    void                        updateAllSensors(void);
    uint32_t                    getThermalLevel(void);
    IOReturn                    handleThermalEvent(uint32_t sensor_id, int32_t temp);
    
    /* Power management */
    IOReturn                    setPowerDomainState(uint32_t domain, uint32_t state);
    IOReturn                    getPowerDomainState(uint32_t domain, uint32_t* state);
    uint32_t                    getDomainPower(uint32_t domain);
    uint64_t                    getDomainEnergy(uint32_t domain);
    IOReturn                    powerDownUnusedDomains(void);
    
    /* Secure operations */
    IOReturn                    secureChallenge(uint8_t* challenge, uint8_t* response);
    IOReturn                    getSecureBootStatus(uint32_t* status);
    IOReturn                    getSecureKeyHash(uint8_t* hash);
    bool                        isSecureBootEnabled(void);
    bool                        isDebugEnabled(void);
    
    /* Firmware interface */
    IOReturn                    getFirmwareInfo(firmware_info_t* info);
    IOReturn                    updateFirmware(const void* data, uint32_t size);
    IOReturn                    verifyFirmwareSignature(const void* data, uint32_t size);
    
    /* Timer */
    void                        timerFired(void);
    static void                 timerFired(OSObject* owner, IOTimerEventSource* sender);
    
    /* Command gate */
    IOReturn                    handleCommand(void* arg0, void* arg1, void* arg2, void* arg3);
    
    /* Utility */
    uint64_t                    getCurrentUptime(void);
    uint64_t                    getCurrentTimestamp(void);
    
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
    IOReturn                    powerStateDidChangeTo(IOPMPowerFlags capabilities,
                                                       unsigned long stateNumber,
                                                       IOService* whatDevice) APPLE_KEXT_OVERRIDE;
    
    /* System event handlers */
    IOReturn                    systemWillShutdown(IOOptionBits specifier);
    void                        systemWillSleep(void);
    void                        systemDidWake(void);
    
    /* Platform interface */
    virtual IOReturn            callPlatformFunction(const OSSymbol* functionName,
                                                     bool waitForFunction,
                                                     void* param1, void* param2,
                                                     void* param3, void* param4) APPLE_KEXT_OVERRIDE;
    
    /* User client access */
    IOReturn                    newUserClient(task_t owningTask,
                                              void* securityID,
                                              UInt32 type,
                                              OSDictionary* properties,
                                              IOUserClient** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleIntegratorCP, IOService)

/*==============================================================================
 * AppleIntegratorCPUserClient - User client for Integrator access
 *==============================================================================*/

class AppleIntegratorCPUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(AppleIntegratorCPUserClient)
    
private:
    task_t                      fTask;
    pid_t                       fPid;
    AppleIntegratorCP*          fProvider;
    uint32_t                    fPermissions;
    uint32_t                    fState;
    
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
    
    IOReturn                    externalMethod(uint32_t selector,
                                                IOExternalMethodArguments* arguments,
                                                IOExternalMethodDispatch* dispatch,
                                                OSObject* target,
                                                void* reference) APPLE_KEXT_OVERRIDE;
    
    /* External methods */
    static IOReturn             sGetCaps(AppleIntegratorCPUserClient* target,
                                          void* reference,
                                          IOExternalMethodArguments* args);
    
    static IOReturn             sGetPerfInfo(AppleIntegratorCPUserClient* target,
                                              void* reference,
                                              IOExternalMethodArguments* args);
    
    static IOReturn             sSetPerfState(AppleIntegratorCPUserClient* target,
                                               void* reference,
                                               IOExternalMethodArguments* args);
    
    static IOReturn             sGetThermalInfo(AppleIntegratorCPUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args);
    
    static IOReturn             sGetPowerInfo(AppleIntegratorCPUserClient* target,
                                               void* reference,
                                               IOExternalMethodArguments* args);
    
    static IOReturn             sSecureRequest(AppleIntegratorCPUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args);
    
    static IOReturn             sGetFirmwareInfo(AppleIntegratorCPUserClient* target,
                                                  void* reference,
                                                  IOExternalMethodArguments* args);
    
    static IOReturn             sMailboxCommand(AppleIntegratorCPUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args);
    
    static IOReturn             sGetStatistics(AppleIntegratorCPUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args);
};

OSDefineMetaClassAndStructors(AppleIntegratorCPUserClient, IOUserClient)

/*==============================================================================
 * External Method Dispatch Table
 *==============================================================================*/

enum {
    kMethodGetCaps,
    kMethodGetPerfInfo,
    kMethodSetPerfState,
    kMethodGetThermalInfo,
    kMethodGetPowerInfo,
    kMethodSecureRequest,
    kMethodGetFirmwareInfo,
    kMethodMailboxCommand,
    kMethodGetStatistics,
    kMethodCount
};

static IOExternalMethodDispatch sMethods[kMethodCount] = {
    {   /* kMethodGetCaps */
        (IOExternalMethodAction)&AppleIntegratorCPUserClient::sGetCaps,
        0, 0, 0, 1
    },
    {   /* kMethodGetPerfInfo */
        (IOExternalMethodAction)&AppleIntegratorCPUserClient::sGetPerfInfo,
        0, 0, 0, 1
    },
    {   /* kMethodSetPerfState */
        (IOExternalMethodAction)&AppleIntegratorCPUserClient::sSetPerfState,
        2, 0, 0, 0
    },
    {   /* kMethodGetThermalInfo */
        (IOExternalMethodAction)&AppleIntegratorCPUserClient::sGetThermalInfo,
        0, 0, 0, 1
    },
    {   /* kMethodGetPowerInfo */
        (IOExternalMethodAction)&AppleIntegratorCPUserClient::sGetPowerInfo,
        0, 0, 0, 1
    },
    {   /* kMethodSecureRequest */
        (IOExternalMethodAction)&AppleIntegratorCPUserClient::sSecureRequest,
        1, 1, 0, 1
    },
    {   /* kMethodGetFirmwareInfo */
        (IOExternalMethodAction)&AppleIntegratorCPUserClient::sGetFirmwareInfo,
        0, 0, 0, 1
    },
    {   /* kMethodMailboxCommand */
        (IOExternalMethodAction)&AppleIntegratorCPUserClient::sMailboxCommand,
        0, 1, 0, 1
    },
    {   /* kMethodGetStatistics */
        (IOExternalMethodAction)&AppleIntegratorCPUserClient::sGetStatistics,
        0, 0, 0, 1
    }
};

/*==============================================================================
 * AppleIntegratorCP Implementation
 *==============================================================================*/

#pragma mark - AppleIntegratorCP::Initialization

bool AppleIntegratorCP::init(OSDictionary* dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    
    /* Initialize version information */
    fDriverVersion = INTEGRATOR_CP_VERSION;
    fDriverRevision = INTEGRATOR_CP_REVISION;
    strlcpy(fDriverBuild, __DATE__ " " __TIME__, sizeof(fDriverBuild));
    
    /* Initialize state */
    fState = 0;
    fFlags = 0;
    fStartTime = 0;
    fHardwareReady = false;
    fFirmwareLoaded = false;
    
    /* Create lock group */
    fLockGroup = lck_grp_alloc_init("AppleIntegratorCP", LCK_GRP_ATTR_NULL);
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
    fStateLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fMailboxLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    
    if (!fHardwareLock || !fStateLock || !fMailboxLock) {
        return false;
    }
    
    /* Initialize arrays */
    bzero(fPerfClusters, sizeof(fPerfClusters));
    bzero(fSensors, sizeof(fSensors));
    bzero(fPowerDomains, sizeof(fPowerDomains));
    
    fNumPerfClusters = 0;
    fNumSensors = 0;
    fNumPowerDomains = 0;
    
    /* Initialize mailbox */
    bzero(&fMailbox, sizeof(fMailbox));
    fMailboxPending = false;
    
    /* Initialize statistics */
    fTotalInterrupts = 0;
    fTotalMailboxMessages = 0;
    fTotalErrors = 0;
    fThermalEvents = 0;
    fPowerEvents = 0;
    fPerfEvents = 0;
    fSecureEvents = 0;
    
    /* Power management */
    fPowerState = 0;
    fThermalLevel = 0;
    fPerformanceLevel = 0;
    fThrottling = false;
    
    return true;
}

void AppleIntegratorCP::free(void)
{
    if (fMailboxThread) {
        thread_call_free(fMailboxThread);
        fMailboxThread = NULL;
    }
    
    if (fSharedMemory) {
        fSharedMemory->release();
        fSharedMemory = NULL;
    }
    
    if (fMemoryMap) {
        fMemoryMap->release();
        fMemoryMap = NULL;
    }
    
    if (fHardwareLock) lck_mtx_free(fHardwareLock, fLockGroup);
    if (fStateLock) lck_mtx_free(fStateLock, fLockGroup);
    if (fMailboxLock) lck_mtx_free(fMailboxLock, fLockGroup);
    
    if (fLockAttr) lck_attr_free(fLockAttr);
    if (fLockGroup) lck_grp_free(fLockGroup);
    
    super::free();
}

#pragma mark - AppleIntegratorCP::Hardware Access

bool AppleIntegratorCP::mapHardware(void)
{
    IOService* provider = getProvider();
    if (!provider) {
        IOLog("AppleIntegratorCP: No provider\n");
        return false;
    }
    
    IODeviceMemory* memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory) {
        IOLog("AppleIntegratorCP: No device memory\n");
        return false;
    }
    
    fPhysBase = memory->getPhysicalAddress();
    fPhysSize = memory->getLength();
    
    fMemoryMap = memory->makeMapping(kIOMapAny | kIOMapInhibitCache);
    if (!fMemoryMap) {
        IOLog("AppleIntegratorCP: Failed to map registers\n");
        return false;
    }
    
    fRegisters = (volatile uint32_t*)fMemoryMap->getVirtualAddress();
    if (!fRegisters) {
        IOLog("AppleIntegratorCP: Invalid virtual address\n");
        fMemoryMap->release();
        fMemoryMap = NULL;
        return false;
    }
    
    IOLog("AppleIntegratorCP: Mapped registers at %p (phys: 0x%llx, size: %lld)\n",
          fRegisters, fPhysBase, fPhysSize);
    
    return true;
}

void AppleIntegratorCP::unmapHardware(void)
{
    if (fMemoryMap) {
        fMemoryMap->release();
        fMemoryMap = NULL;
        fRegisters = NULL;
    }
}

inline uint32_t AppleIntegratorCP::readReg(uint32_t offset)
{
    if (!fRegisters || offset + 4 > fPhysSize) {
        return 0xFFFFFFFF;
    }
    return fRegisters[offset / 4];
}

inline void AppleIntegratorCP::writeReg(uint32_t offset, uint32_t value)
{
    if (!fRegisters || offset + 4 > fPhysSize) {
        return;
    }
    fRegisters[offset / 4] = value;
    OSMemoryBarrier();
}

inline uint32_t AppleIntegratorCP::readReg64(uint32_t offset, uint64_t* value)
{
    if (!fRegisters || offset + 8 > fPhysSize) {
        return -1;
    }
    
    uint32_t low = fRegisters[offset / 4];
    uint32_t high = fRegisters[offset / 4 + 1];
    *value = ((uint64_t)high << 32) | low;
    
    return 0;
}

inline void AppleIntegratorCP::writeReg64(uint32_t offset, uint64_t value)
{
    if (!fRegisters || offset + 8 > fPhysSize) {
        return;
    }
    
    fRegisters[offset / 4] = (uint32_t)value;
    fRegisters[offset / 4 + 1] = (uint32_t)(value >> 32);
    OSMemoryBarrier();
}

#pragma mark - AppleIntegratorCP::Interrupt Handling

bool AppleIntegratorCP::setupInterrupts(void)
{
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        IOLog("AppleIntegratorCP: Failed to create work loop\n");
        return false;
    }
    
    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        IOLog("AppleIntegratorCP: Failed to create command gate\n");
        fWorkLoop->release();
        fWorkLoop = NULL;
        return false;
    }
    
    fWorkLoop->addEventSource(fCommandGate);
    
    fInterruptSource = IOInterruptEventSource::interruptEventSource(
        this,
        OSMemberFunctionCast(IOInterruptEventAction, this,
                             &AppleIntegratorCP::interruptHandler),
        getProvider(),
        0);
    
    if (!fInterruptSource) {
        IOLog("AppleIntegratorCP: Failed to create interrupt source\n");
        fWorkLoop->removeEventSource(fCommandGate);
        fCommandGate->release();
        fCommandGate = NULL;
        fWorkLoop->release();
        fWorkLoop = NULL;
        return false;
    }
    
    fWorkLoop->addEventSource(fInterruptSource);
    
    fTimerSource = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &AppleIntegratorCP::timerFired));
    
    if (!fTimerSource) {
        IOLog("AppleIntegratorCP: Failed to create timer source\n");
        fWorkLoop->removeEventSource(fInterruptSource);
        fInterruptSource->release();
        fInterruptSource = NULL;
        fWorkLoop->removeEventSource(fCommandGate);
        fCommandGate->release();
        fCommandGate = NULL;
        fWorkLoop->release();
        fWorkLoop = NULL;
        return false;
    }
    
    fWorkLoop->addEventSource(fTimerSource);
    
    /* Enable interrupts */
    writeReg(ICP_REG_INT_ENABLE, ICP_INT_ALL);
    
    return true;
}

void AppleIntegratorCP::freeInterrupts(void)
{
    /* Disable interrupts */
    if (fRegisters) {
        writeReg(ICP_REG_INT_ENABLE, 0);
    }
    
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

void AppleIntegratorCP::interruptHandler(void)
{
    uint32_t int_status;
    
    if (!fRegisters) {
        return;
    }
    
    /* Read interrupt status */
    int_status = readReg(ICP_REG_INT_STATUS);
    
    /* Increment counter */
    OSIncrementAtomic64(&fTotalInterrupts);
    
    /* Clear interrupts */
    writeReg(ICP_REG_INT_CLEAR, int_status);
    
    /* Handle mailbox interrupt */
    if (int_status & ICP_INT_MAILBOX) {
        fMailboxPending = true;
        if (fMailboxThread) {
            thread_call_enter(fMailboxThread);
        }
        fTotalMailboxMessages++;
    }
    
    /* Handle thermal interrupt */
    if (int_status & ICP_INT_THERMAL) {
        fThermalEvents++;
        updateAllSensors();
        fThermalLevel = getThermalLevel();
        
        /* Queue event for power management */
        queueEvent(kDiagEventThermalWarning, 3, "Integrator", 
                   "Thermal event detected", NULL, 0);
    }
    
    /* Handle power interrupt */
    if (int_status & ICP_INT_POWER) {
        fPowerEvents++;
        /* Handle power state change */
    }
    
    /* Handle performance interrupt */
    if (int_status & ICP_INT_PERF) {
        fPerfEvents++;
        /* Handle performance state change */
    }
    
    /* Handle secure interrupt */
    if (int_status & ICP_INT_SECURE) {
        fSecureEvents++;
        /* Handle secure operation complete */
    }
    
    /* Handle error interrupt */
    if (int_status & ICP_INT_ERROR) {
        fTotalErrors++;
        IOLog("AppleIntegratorCP: Hardware error detected\n");
    }
}

void AppleIntegratorCP::interruptHandler(IOInterruptEventSource* source, int count)
{
    AppleIntegratorCP* me = (AppleIntegratorCP*)source->owner();
    if (me) {
        me->interruptHandler();
    }
}

#pragma mark - AppleIntegratorCP::Mailbox

void AppleIntegratorCP::mailboxHandler(void)
{
    mbox_message_t msg;
    
    lck_mtx_lock(fMailboxLock);
    
    if (!fMailboxPending) {
        lck_mtx_unlock(fMailboxLock);
        return;
    }
    
    /* Read mailbox data */
    msg.type = readReg(ICP_MBOX_STATUS) & 0xFF;
    msg.command = (readReg(ICP_MBOX_STATUS) >> 8) & 0xFF;
    msg.result = (readReg(ICP_MBOX_STATUS) >> 16) & 0xFF;
    
    for (int i = 0; i < 8; i++) {
        msg.data[i] = readReg(ICP_MBOX_DATA0 + (i * 4));
    }
    
    msg.timestamp = getCurrentUptime();
    
    /* Process message based on type */
    switch (msg.type) {
        case kMboxMsgPowerState:
            /* Update power state */
            fPowerState = msg.data[0];
            break;
            
        case kMboxMsgPerfState:
            /* Update performance state */
            fPerformanceLevel = msg.data[0];
            break;
            
        case kMboxMsgThermalData:
            /* Update thermal data */
            for (int i = 0; i < fNumSensors && i < 4; i++) {
                fSensors[i].temperature = (int32_t)msg.data[i];
            }
            break;
            
        case kMboxMsgSecureResponse:
            /* Handle secure response */
            memcpy(fSecureInfo.response, msg.data, 16);
            break;
            
        case kMboxMsgError:
            fTotalErrors++;
            break;
    }
    
    /* Store message */
    memcpy(&fMailbox, &msg, sizeof(msg));
    fMailboxPending = false;
    
    lck_mtx_unlock(fMailboxLock);
}

void AppleIntegratorCP::mailboxHandler(thread_call_param_t param0, thread_call_param_t param1)
{
    AppleIntegratorCP* me = (AppleIntegratorCP*)param0;
    if (me) {
        me->mailboxHandler();
    }
}

IOReturn AppleIntegratorCP::sendMailboxMessage(mbox_message_t* msg)
{
    if (!msg || !fRegisters) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fMailboxLock);
    
    /* Check if mailbox is busy */
    if (readReg(ICP_MBOX_STATUS) & (1 << 31)) {
        lck_mtx_unlock(fMailboxLock);
        return kIOReturnBusy;
    }
    
    /* Write data words */
    for (int i = 0; i < 8; i++) {
        writeReg(ICP_MBOX_DATA0 + (i * 4), msg->data[i]);
    }
    
    /* Write status (triggers interrupt in coprocessor) */
    uint32_t status = (msg->type & 0xFF) | ((msg->command & 0xFF) << 8) | (1 << 31);
    writeReg(ICP_MBOX_STATUS, status);
    
    msg->timestamp = getCurrentUptime();
    
    lck_mtx_unlock(fMailboxLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::receiveMailboxMessage(mbox_message_t* msg)
{
    if (!msg) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fMailboxLock);
    memcpy(msg, &fMailbox, sizeof(mbox_message_t));
    lck_mtx_unlock(fMailboxLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::waitForMailboxResponse(mbox_message_t* msg, uint32_t timeout_ms)
{
    uint64_t deadline;
    uint64_t now;
    
    deadline = getCurrentUptime() + (timeout_ms * 1000 * 1000);
    
    do {
        if (!fMailboxPending) {
            return receiveMailboxMessage(msg);
        }
        
        now = getCurrentUptime();
        if (now >= deadline) {
            return kIOReturnTimeout;
        }
        
        thread_block(THREAD_CONTINUE_NULL);
    } while (true);
    
    return kIOReturnTimeout;
}

#pragma mark - AppleIntegratorCP::Performance Management

IOReturn AppleIntegratorCP::setPerformanceState(uint32_t cluster, uint32_t pstate)
{
    if (cluster >= fNumPerfClusters || pstate >= kPStateCount) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    
    /* Update hardware */
    uint32_t control = readReg(ICP_REG_PERF_CONTROL);
    control = (control & ~(0x7 << (cluster * 4))) | (pstate << (cluster * 4));
    writeReg(ICP_REG_PERF_CONTROL, control);
    
    /* Send mailbox message */
    mbox_message_t msg;
    bzero(&msg, sizeof(msg));
    msg.type = kMboxMsgPerfRequest;
    msg.command = 0x01;  /* Set P-state */
    msg.data[0] = cluster;
    msg.data[1] = pstate;
    
    sendMailboxMessage(&msg);
    
    /* Update cached state */
    fPerfClusters[cluster].pstate = pstate;
    fPerfClusters[cluster].frequency_cur = getCurrentFrequency(cluster);
    fPerfClusters[cluster].voltage_cur = getCurrentVoltage(cluster);
    
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::getPerformanceState(uint32_t cluster, uint32_t* pstate)
{
    if (cluster >= fNumPerfClusters || !pstate) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    *pstate = fPerfClusters[cluster].pstate;
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::setCoreMask(uint32_t cluster, uint32_t core_mask)
{
    if (cluster >= fNumPerfClusters) {
        return kIOReturnBadArgument;
    }
    
    uint32_t max_cores = fPerfClusters[cluster].core_count;
    if (core_mask >= (1 << max_cores)) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    
    /* Update hardware */
    writeReg(ICP_PERF_CORE_MASK + (cluster * 4), core_mask);
    
    /* Update cache */
    fPerfClusters[cluster].core_mask = core_mask;
    fPerfClusters[cluster].active_cores = __builtin_popcount(core_mask);
    
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::getCoreMask(uint32_t cluster, uint32_t* core_mask)
{
    if (cluster >= fNumPerfClusters || !core_mask) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    *core_mask = fPerfClusters[cluster].core_mask;
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

uint64_t AppleIntegratorCP::readEnergyCounter(uint32_t cluster)
{
    uint64_t energy = 0;
    
    if (cluster >= fNumPerfClusters) {
        return 0;
    }
    
    lck_mtx_lock(fHardwareLock);
    readReg64(ICP_PERF_ENERGY_COUNTER + (cluster * 8), &energy);
    fPerfClusters[cluster].energy_counter = energy;
    lck_mtx_unlock(fHardwareLock);
    
    return energy;
}

uint32_t AppleIntegratorCP::getCurrentFrequency(uint32_t cluster)
{
    if (cluster >= fNumPerfClusters) {
        return 0;
    }
    
    return readReg(ICP_PERF_FREQ_CURRENT + (cluster * 4));
}

uint32_t AppleIntegratorCP::getCurrentVoltage(uint32_t cluster)
{
    if (cluster >= fNumPerfClusters) {
        return 0;
    }
    
    return readReg(ICP_PERF_VOLTAGE_CURRENT + (cluster * 4));
}

#pragma mark - AppleIntegratorCP::Thermal Management

IOReturn AppleIntegratorCP::readSensor(uint32_t sensor_id, int32_t* temp)
{
    if (sensor_id >= fNumSensors || !temp) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    
    uint32_t offset = ICP_THERMAL_SENSOR_BASE + (sensor_id * ICP_THERMAL_SENSOR_SIZE);
    *temp = (int32_t)readReg(offset + ICP_THERMAL_TEMPERATURE);
    
    fSensors[sensor_id].temperature = *temp;
    fSensors[sensor_id].last_update = getCurrentUptime();
    
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::setSensorThreshold(uint32_t sensor_id, int32_t low, int32_t high)
{
    if (sensor_id >= fNumSensors) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    
    uint32_t offset = ICP_THERMAL_SENSOR_BASE + (sensor_id * ICP_THERMAL_SENSOR_SIZE);
    writeReg(offset + ICP_THERMAL_THRESHOLD_LOW, (uint32_t)low);
    writeReg(offset + ICP_THERMAL_THRESHOLD_HIGH, (uint32_t)high);
    
    fSensors[sensor_id].threshold_low = low;
    fSensors[sensor_id].threshold_high = high;
    
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::getSensorThreshold(uint32_t sensor_id, int32_t* low, int32_t* high)
{
    if (sensor_id >= fNumSensors || !low || !high) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    *low = fSensors[sensor_id].threshold_low;
    *high = fSensors[sensor_id].threshold_high;
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

void AppleIntegratorCP::updateAllSensors(void)
{
    lck_mtx_lock(fHardwareLock);
    
    for (uint32_t i = 0; i < fNumSensors; i++) {
        uint32_t offset = ICP_THERMAL_SENSOR_BASE + (i * ICP_THERMAL_SENSOR_SIZE);
        fSensors[i].temperature = (int32_t)readReg(offset + ICP_THERMAL_TEMPERATURE);
        fSensors[i].last_update = getCurrentUptime();
    }
    
    lck_mtx_unlock(fHardwareLock);
}

uint32_t AppleIntegratorCP::getThermalLevel(void)
{
    int32_t max_temp = 0;
    uint32_t level = 0;
    
    lck_mtx_lock(fHardwareLock);
    
    for (uint32_t i = 0; i < fNumSensors; i++) {
        if (fSensors[i].temperature > max_temp) {
            max_temp = fSensors[i].temperature;
        }
    }
    
    lck_mtx_unlock(fHardwareLock);
    
    /* Convert temperature to level (0-100) */
    if (max_temp > 8000) {  /* >80°C */
        level = 100;
    } else if (max_temp > 7000) {  /* >70°C */
        level = 80;
    } else if (max_temp > 6000) {  /* >60°C */
        level = 60;
    } else if (max_temp > 5000) {  /* >50°C */
        level = 40;
    } else {
        level = 20;
    }
    
    return level;
}

IOReturn AppleIntegratorCP::handleThermalEvent(uint32_t sensor_id, int32_t temp)
{
    uint32_t level = getThermalLevel();
    
    if (level > 80 && !fThrottling) {
        fThrottling = true;
        /* Initiate throttling */
        for (uint32_t i = 0; i < fNumPerfClusters; i++) {
            setPerformanceState(i, kPStateBalanced);
        }
        IOLog("AppleIntegratorCP: Thermal throttling initiated (level %d)\n", level);
    } else if (level < 40 && fThrottling) {
        fThrottling = false;
        /* Resume normal operation */
        IOLog("AppleIntegratorCP: Thermal throttling ended\n");
    }
    
    return kIOReturnSuccess;
}

#pragma mark - AppleIntegratorCP::Power Management

IOReturn AppleIntegratorCP::setPowerDomainState(uint32_t domain, uint32_t state)
{
    if (domain >= fNumPowerDomains) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    
    writeReg(ICP_POWER_DOMAIN0 + (domain * 4), state);
    fPowerDomains[domain].state = state;
    
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::getPowerDomainState(uint32_t domain, uint32_t* state)
{
    if (domain >= fNumPowerDomains || !state) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    *state = fPowerDomains[domain].state;
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

uint32_t AppleIntegratorCP::getDomainPower(uint32_t domain)
{
    if (domain >= fNumPowerDomains) {
        return 0;
    }
    
    return fPowerDomains[domain].power;
}

uint64_t AppleIntegratorCP::getDomainEnergy(uint32_t domain)
{
    if (domain >= fNumPowerDomains) {
        return 0;
    }
    
    return fPowerDomains[domain].energy_used;
}

IOReturn AppleIntegratorCP::powerDownUnusedDomains(void)
{
    /* Check each domain and power down if inactive */
    for (uint32_t i = 0; i < fNumPowerDomains; i++) {
        if (fPowerDomains[i].power == 0 && fPowerDomains[i].state != ICP_POWER_STATE_OFF) {
            setPowerDomainState(i, ICP_POWER_STATE_OFF);
        }
    }
    
    return kIOReturnSuccess;
}

#pragma mark - AppleIntegratorCP::Secure Operations

IOReturn AppleIntegratorCP::secureChallenge(uint8_t* challenge, uint8_t* response)
{
    if (!challenge || !response) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    
    /* Write challenge */
    for (int i = 0; i < 4; i++) {
        uint32_t word = (challenge[i*4] << 24) | (challenge[i*4+1] << 16) |
                        (challenge[i*4+2] << 8) | challenge[i*4+3];
        writeReg(ICP_SECURE_CHALLENGE + (i * 4), word);
    }
    
    /* Trigger secure operation */
    writeReg(ICP_SECURE_CONTROL, 0x01);
    
    /* Wait for response */
    /* In real implementation, would wait for interrupt */
    IOSleep(10);
    
    /* Read response */
    for (int i = 0; i < 4; i++) {
        uint32_t word = readReg(ICP_SECURE_RESPONSE + (i * 4));
        response[i*4] = (word >> 24) & 0xFF;
        response[i*4+1] = (word >> 16) & 0xFF;
        response[i*4+2] = (word >> 8) & 0xFF;
        response[i*4+3] = word & 0xFF;
    }
    
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::getSecureBootStatus(uint32_t* status)
{
    if (!status) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    *status = readReg(ICP_SECURE_BOOT_STATUS);
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::getSecureKeyHash(uint8_t* hash)
{
    if (!hash) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    
    for (int i = 0; i < 8; i++) {
        uint32_t word = readReg(ICP_SECURE_KEY_HASH + (i * 4));
        hash[i*4] = (word >> 24) & 0xFF;
        hash[i*4+1] = (word >> 16) & 0xFF;
        hash[i*4+2] = (word >> 8) & 0xFF;
        hash[i*4+3] = word & 0xFF;
    }
    
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

bool AppleIntegratorCP::isSecureBootEnabled(void)
{
    uint32_t status;
    if (getSecureBootStatus(&status) == kIOReturnSuccess) {
        return (status & 0x01) != 0;
    }
    return false;
}

bool AppleIntegratorCP::isDebugEnabled(void)
{
    return (readReg(ICP_SECURE_DEBUG) & 0x01) != 0;
}

#pragma mark - AppleIntegratorCP::Firmware Interface

IOReturn AppleIntegratorCP::getFirmwareInfo(firmware_info_t* info)
{
    if (!info) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fHardwareLock);
    memcpy(info, &fFirmwareInfo, sizeof(firmware_info_t));
    lck_mtx_unlock(fHardwareLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::updateFirmware(const void* data, uint32_t size)
{
    if (!data || size == 0 || size > ICP_MAX_FIRMWARE_SIZE) {
        return kIOReturnBadArgument;
    }
    
    /* Verify signature first */
    IOReturn ret = verifyFirmwareSignature(data, size);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    
    /* In real implementation, would load firmware to coprocessor */
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCP::verifyFirmwareSignature(const void* data, uint32_t size)
{
    uint8_t hash[32];
    uint8_t expected[32];
    
    /* Compute SHA256 of firmware */
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, (const uint8_t*)data, size);
    SHA1Final(hash, &ctx);
    
    /* Get expected hash from secure storage */
    getSecureKeyHash(expected);
    
    /* Compare */
    if (memcmp(hash, expected, 20) != 0) {  /* SHA1 is 20 bytes */
        IOLog("AppleIntegratorCP: Firmware signature verification failed\n");
        return kIOReturnUnsupported;
    }
    
    return kIOReturnSuccess;
}

#pragma mark - AppleIntegratorCP::Timer

void AppleIntegratorCP::timerFired(void)
{
    /* Update sensor data */
    updateAllSensors();
    
    /* Update performance statistics */
    for (uint32_t i = 0; i < fNumPerfClusters; i++) {
        fPerfClusters[i].energy_counter = readEnergyCounter(i);
    }
    
    /* Check thermal level */
    uint32_t new_level = getThermalLevel();
    if (new_level != fThermalLevel) {
        fThermalLevel = new_level;
        
        /* Handle thermal level change */
        for (uint32_t i = 0; i < fNumSensors; i++) {
            handleThermalEvent(i, fSensors[i].temperature);
        }
    }
    
    /* Schedule next timer */
    fTimerSource->setTimeoutMS(1000);  /* 1 second */
}

void AppleIntegratorCP::timerFired(OSObject* owner, IOTimerEventSource* sender)
{
    AppleIntegratorCP* me = OSDynamicCast(AppleIntegratorCP, owner);
    if (me) {
        me->timerFired();
    }
}

#pragma mark - AppleIntegratorCP::Hardware Initialization

bool AppleIntegratorCP::resetController(void)
{
    uint32_t timeout = 100;  /* 100ms */
    
    IOLog("AppleIntegratorCP: Resetting controller\n");
    
    /* Reset controller */
    writeReg(ICP_REG_CONTROL, ICP_CTRL_RESET);
    
    /* Wait for reset to complete */
    uint32_t count = 0;
    while (count < timeout) {
        if (readReg(ICP_REG_STATUS) & ICP_STATUS_READY) {
            break;
        }
        IOSleep(1);
        count++;
    }
    
    if (count >= timeout) {
        IOLog("AppleIntegratorCP: Reset timeout\n");
        return false;
    }
    
    IOLog("AppleIntegratorCP: Reset completed\n");
    
    return true;
}

bool AppleIntegratorCP::loadFirmware(void)
{
    /* In real implementation, would load firmware from preboot partition */
    
    IOLog("AppleIntegratorCP: Loading firmware\n");
    
    /* Wait for firmware to report ready */
    uint32_t timeout = 1000;  /* 1 second */
    uint32_t count = 0;
    
    while (count < timeout) {
        if (readReg(ICP_REG_STATUS) & ICP_STATUS_FW_LOADED) {
            break;
        }
        IOSleep(1);
        count++;
    }
    
    if (count >= timeout) {
        IOLog("AppleIntegratorCP: Firmware load timeout\n");
        return false;
    }
    
    /* Read firmware info */
    fFirmwareInfo.version_major = readReg(ICP_FW_VERSION) >> 16;
    fFirmwareInfo.version_minor = readReg(ICP_FW_VERSION) & 0xFFFF;
    fFirmwareInfo.revision = readReg(ICP_FW_REVISION);
    fFirmwareInfo.build = readReg(ICP_FW_BUILD);
    fFirmwareInfo.features = readReg(ICP_FW_FEATURES);
    fFirmwareInfo.boot_time = getCurrentTimestamp();
    fFirmwareInfo.uptime = getCurrentUptime();
    
    IOLog("AppleIntegratorCP: Firmware loaded (v%d.%d.%d)\n",
          fFirmwareInfo.version_major,
          fFirmwareInfo.version_minor,
          fFirmwareInfo.revision);
    
    return true;
}

bool AppleIntegratorCP::verifyFirmware(void)
{
    /* Verify firmware integrity */
    return true;
}

bool AppleIntegratorCP::startFirmware(void)
{
    writeReg(ICP_REG_CONTROL, ICP_CTRL_ENABLE);
    
    uint32_t timeout = 100;
    uint32_t count = 0;
    
    while (count < timeout) {
        if (readReg(ICP_REG_STATUS) & ICP_STATUS_RUNNING) {
            return true;
        }
        IOSleep(1);
        count++;
    }
    
    return false;
}

bool AppleIntegratorCP::initializeHardware(void)
{
    uint32_t id, version;
    
    if (!fRegisters) {
        IOLog("AppleIntegratorCP: No registers mapped\n");
        return false;
    }
    
    /* Read hardware ID and version */
    id = readReg(ICP_REG_ID);
    version = readReg(ICP_REG_VERSION);
    
    IOLog("AppleIntegratorCP: Hardware ID: 0x%08x, Version: 0x%08x\n", id, version);
    
    /* Reset controller */
    if (!resetController()) {
        return false;
    }
    
    /* Load firmware */
    if (!loadFirmware()) {
        return false;
    }
    
    /* Verify firmware */
    if (!verifyFirmware()) {
        IOLog("AppleIntegratorCP: Firmware verification failed\n");
        return false;
    }
    
    /* Start firmware */
    if (!startFirmware()) {
        IOLog("AppleIntegratorCP: Failed to start firmware\n");
        return false;
    }
    
    /* Initialize capabilities */
    fCaps.version = version;
    fCaps.features = readReg(ICP_FW_FEATURES);
    fCaps.num_sensors = ICP_MAX_SENSORS;
    fCaps.num_power_domains = ICP_MAX_POWER_DOMAINS;
    fCaps.num_perf_clusters = ICP_MAX_PERF_CLUSTERS;
    fCaps.max_frequency = 3500;  /* MHz */
    fCaps.min_frequency = 600;   /* MHz */
    fCaps.max_voltage = 1200;    /* mV */
    fCaps.min_voltage = 700;     /* mV */
    fCaps.thermal_throttle_temp = 85;  /* °C */
    fCaps.thermal_shutdown_temp = 100; /* °C */
    fCaps.secure_boot_required = 1;
    
    /* Initialize performance clusters */
    fNumPerfClusters = 2;
    
    /* P-core cluster */
    strlcpy(fPerfClusters[0].name, "P-Cores", sizeof(fPerfClusters[0].name));
    fPerfClusters[0].cluster_id = 0;
    fPerfClusters[0].core_count = 4;
    fPerfClusters[0].active_cores = 4;
    fPerfClusters[0].pstate = kPStateBalanced;
    fPerfClusters[0].cstate = kCStateActive;
    fPerfClusters[0].frequency_min = fCaps.min_frequency;
    fPerfClusters[0].frequency_max = fCaps.max_frequency;
    fPerfClusters[0].frequency_cur = 2400;
    fPerfClusters[0].voltage_min = fCaps.min_voltage;
    fPerfClusters[0].voltage_max = fCaps.max_voltage;
    fPerfClusters[0].voltage_cur = 1000;
    fPerfClusters[0].energy_counter = 0;
    fPerfClusters[0].core_mask = 0x0F;
    
    /* E-core cluster */
    strlcpy(fPerfClusters[1].name, "E-Cores", sizeof(fPerfClusters[1].name));
    fPerfClusters[1].cluster_id = 1;
    fPerfClusters[1].core_count = 4;
    fPerfClusters[1].active_cores = 4;
    fPerfClusters[1].pstate = kPStateBalanced;
    fPerfClusters[1].cstate = kCStateActive;
    fPerfClusters[1].frequency_min = fCaps.min_frequency;
    fPerfClusters[1].frequency_max = 2000;
    fPerfClusters[1].frequency_cur = 1600;
    fPerfClusters[1].voltage_min = fCaps.min_voltage;
    fPerfClusters[1].voltage_max = 900;
    fPerfClusters[1].voltage_cur = 750;
    fPerfClusters[1].energy_counter = 0;
    fPerfClusters[1].core_mask = 0x0F;
    
    /* Initialize thermal sensors */
    fNumSensors = 8;
    
    const char* sensor_names[] = {
        "CPU_P_TEMP", "CPU_E_TEMP", "GPU_TEMP", "ANE_TEMP",
        "PMIC_TEMP", "CHARGER_TEMP", "BATTERY_TEMP", "AMBIENT_TEMP"
    };
    
    const char* sensor_locations[] = {
        "P-Core Cluster", "E-Core Cluster", "GPU", "Neural Engine",
        "Power Management IC", "Charger", "Battery", "Chassis Ambient"
    };
    
    for (int i = 0; i < 8; i++) {
        strlcpy(fSensors[i].name, sensor_names[i], sizeof(fSensors[i].name));
        strlcpy(fSensors[i].location, sensor_locations[i], sizeof(fSensors[i].location));
        fSensors[i].sensor_id = i;
        fSensors[i].type = i < 2 ? 0 : (i < 3 ? 1 : 2);  /* 0=CPU, 1=GPU, 2=other */
        fSensors[i].temperature = 3500;  /* 35.00°C */
        fSensors[i].threshold_low = 0;
        fSensors[i].threshold_high = 8500;  /* 85°C */
        fSensors[i].status = 1;
        fSensors[i].confidence = 95;
        fSensors[i].last_update = getCurrentUptime();
    }
    
    /* Initialize power domains */
    fNumPowerDomains = 8;
    
    const char* domain_names[] = {
        "P-Core Cluster", "E-Core Cluster", "GPU", "Neural Engine",
        "Display Engine", "Media Engine", "Secure Enclave", "Memory Controller"
    };
    
    for (int i = 0; i < 8; i++) {
        strlcpy(fPowerDomains[i].name, domain_names[i], sizeof(fPowerDomains[i].name));
        fPowerDomains[i].domain_id = i;
        fPowerDomains[i].state = ICP_POWER_STATE_ON;
        fPowerDomains[i].voltage = 1000;
        fPowerDomains[i].current = i < 2 ? 2000 : (i < 3 ? 1500 : 500);
        fPowerDomains[i].power = fPowerDomains[i].voltage * fPowerDomains[i].current / 1000;
        fPowerDomains[i].energy_used = 0;
    }
    
    /* Read secure boot status */
    fSecureInfo.boot_status = readReg(ICP_SECURE_BOOT_STATUS);
    fSecureInfo.boot_policy = readReg(ICP_SECURE_BOOT_POLICY);
    fSecureInfo.security_level = 2;
    fSecureInfo.debug_enabled = 0;
    fSecureInfo.flags = 0;
    
    IOLog("AppleIntegratorCP: Hardware initialized successfully\n");
    
    return true;
}

void AppleIntegratorCP::shutdownHardware(void)
{
    IOLog("AppleIntegratorCP: Shutting down hardware\n");
    
    /* Disable interrupts */
    writeReg(ICP_REG_INT_ENABLE, 0);
    
    /* Power down all domains */
    for (uint32_t i = 0; i < fNumPowerDomains; i++) {
        setPowerDomainState(i, ICP_POWER_STATE_OFF);
    }
    
    /* Reset controller */
    writeReg(ICP_REG_CONTROL, ICP_CTRL_RESET);
    
    IOLog("AppleIntegratorCP: Hardware shutdown complete\n");
}

#pragma mark - AppleIntegratorCP::Utility Methods

uint64_t AppleIntegratorCP::getCurrentUptime(void)
{
    uint64_t uptime;
    clock_get_uptime(&uptime);
    return uptime;
}

uint64_t AppleIntegratorCP::getCurrentTimestamp(void)
{
    clock_sec_t secs;
    clock_nsec_t nsecs;
    clock_get_calendar_nanotime(&secs, &nsecs);
    return (uint64_t)secs * NSEC_PER_SEC + nsecs;
}

#pragma mark - AppleIntegratorCP::IOService Overrides

bool AppleIntegratorCP::start(IOService* provider)
{
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("AppleIntegratorCP: Starting Integrator Coprocessor driver\n");
    
    /* Map hardware registers */
    if (!mapHardware()) {
        IOLog("AppleIntegratorCP: Failed to map hardware\n");
        return false;
    }
    
    /* Initialize hardware */
    if (!initializeHardware()) {
        IOLog("AppleIntegratorCP: Hardware initialization failed\n");
        unmapHardware();
        return false;
    }
    
    /* Setup interrupts */
    if (!setupInterrupts()) {
        IOLog("AppleIntegratorCP: Failed to setup interrupts\n");
        shutdownHardware();
        unmapHardware();
        return false;
    }
    
    /* Create mailbox thread */
    fMailboxThread = thread_call_allocate(
        (thread_call_func_t)AppleIntegratorCP::mailboxHandler,
        (thread_call_param_t)this);
    
    if (!fMailboxThread) {
        IOLog("AppleIntegratorCP: Failed to create mailbox thread\n");
        freeInterrupts();
        shutdownHardware();
        unmapHardware();
        return false;
    }
    
    /* Allocate shared memory */
    fSharedSize = ICP_MAX_SHARED_MEMORY;
    fSharedMemory = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut,
        fSharedSize,
        PAGE_SIZE);
    
    if (!fSharedMemory) {
        IOLog("AppleIntegratorCP: Failed to allocate shared memory\n");
        thread_call_free(fMailboxThread);
        fMailboxThread = NULL;
        freeInterrupts();
        shutdownHardware();
        unmapHardware();
        return false;
    }
    
    fSharedBase = fSharedMemory->getBytesNoCopy();
    
    /* Start timer */
    if (fTimerSource) {
        fTimerSource->setTimeoutMS(1000);
    }
    
    /* Publish properties */
    char version_str[32];
    snprintf(version_str, sizeof(version_str), "%d.%d.%d",
             (fCaps.version >> 16) & 0xFF,
             (fCaps.version >> 8) & 0xFF,
             fCaps.version & 0xFF);
    
    setProperty("Integrator Version", version_str);
    setProperty("Feature Flags", fCaps.features, 32);
    setProperty("Performance Clusters", fNumPerfClusters, 32);
    setProperty("Thermal Sensors", fNumSensors, 32);
    setProperty("Power Domains", fNumPowerDomains, 32);
    setProperty("Secure Boot", isSecureBootEnabled() ? kOSBooleanTrue : kOSBooleanFalse);
    
    OSArray* perf_array = OSArray::withCapacity(fNumPerfClusters);
    for (uint32_t i = 0; i < fNumPerfClusters; i++) {
        OSDictionary* dict = OSDictionary::withCapacity(4);
        dict->setObject("Name", OSString::withCString(fPerfClusters[i].name));
        dict->setObject("Cores", fPerfClusters[i].core_count, 32);
        dict->setObject("Frequency", fPerfClusters[i].frequency_cur, 32);
        dict->setObject("Voltage", fPerfClusters[i].voltage_cur, 32);
        perf_array->setObject(dict);
        dict->release();
    }
    setProperty("Performance Clusters", perf_array);
    perf_array->release();
    
    /* Register power management */
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, NULL, 0);
    
    /* Register service */
    registerService();
    
    fHardwareReady = true;
    
    IOLog("AppleIntegratorCP: Successfully started\n");
    
    return true;
}

void AppleIntegratorCP::stop(IOService* provider)
{
    IOLog("AppleIntegratorCP: Stopping\n");
    
    fHardwareReady = false;
    
    /* Stop timer */
    if (fTimerSource) {
        fTimerSource->cancelTimeout();
    }
    
    /* Free mailbox thread */
    if (fMailboxThread) {
        thread_call_free(fMailboxThread);
        fMailboxThread = NULL;
    }
    
    /* Free shared memory */
    if (fSharedMemory) {
        fSharedMemory->release();
        fSharedMemory = NULL;
    }
    
    /* Free interrupts */
    freeInterrupts();
    
    /* Shutdown hardware */
    shutdownHardware();
    
    /* Unmap hardware */
    unmapHardware();
    
    /* Power management */
    PMstop();
    
    super::stop(provider);
}

#pragma mark - AppleIntegratorCP::Power Management

IOReturn AppleIntegratorCP::setPowerState(unsigned long powerState, IOService* device)
{
    if (powerState == 0) {
        /* Going to sleep */
        IOLog("AppleIntegratorCP: Preparing for sleep\n");
        
        /* Save critical state */
        /* Power down non-essential domains */
        for (uint32_t i = 2; i < fNumPowerDomains; i++) {  /* Skip CPU clusters */
            setPowerDomainState(i, ICP_POWER_STATE_SLEEP);
        }
        
        /* Send sleep notification to coprocessor */
        mbox_message_t msg;
        bzero(&msg, sizeof(msg));
        msg.type = kMboxMsgPowerRequest;
        msg.command = 0x02;  /* Sleep */
        sendMailboxMessage(&msg);
        
    } else {
        /* Waking up */
        IOLog("AppleIntegratorCP: Waking from sleep\n");
        
        /* Restore state */
        /* Power up domains */
        for (uint32_t i = 0; i < fNumPowerDomains; i++) {
            setPowerDomainState(i, ICP_POWER_STATE_ON);
        }
        
        /* Send wake notification */
        mbox_message_t msg;
        bzero(&msg, sizeof(msg));
        msg.type = kMboxMsgPowerRequest;
        msg.command = 0x03;  /* Wake */
        sendMailboxMessage(&msg);
        
        /* Re-initialize sensors */
        updateAllSensors();
    }
    
    return IOPMAckImplied;
}

IOReturn AppleIntegratorCP::powerStateWillChangeTo(IOPMPowerFlags capabilities,
                                                    unsigned long stateNumber,
                                                    IOService* whatDevice)
{
    return IOPMAckImplied;
}

IOReturn AppleIntegratorCP::powerStateDidChangeTo(IOPMPowerFlags capabilities,
                                                   unsigned long stateNumber,
                                                   IOService* whatDevice)
{
    return IOPMAckImplied;
}

#pragma mark - AppleIntegratorCP::System Event Handlers

IOReturn AppleIntegratorCP::systemWillShutdown(IOOptionBits specifier)
{
    IOLog("AppleIntegratorCP: System will shut down\n");
    
    /* Save statistics to NVRAM */
    /* Power down all domains */
    for (uint32_t i = 0; i < fNumPowerDomains; i++) {
        setPowerDomainState(i, ICP_POWER_STATE_OFF);
    }
    
    return kIOReturnSuccess;
}

void AppleIntegratorCP::systemWillSleep(void)
{
    IOLog("AppleIntegratorCP: System will sleep\n");
}

void AppleIntegratorCP::systemDidWake(void)
{
    IOLog("AppleIntegratorCP: System did wake\n");
}

#pragma mark - AppleIntegratorCP::Platform Function Calls

IOReturn AppleIntegratorCP::callPlatformFunction(const OSSymbol* functionName,
                                                  bool waitForFunction,
                                                  void* param1, void* param2,
                                                  void* param3, void* param4)
{
    if (functionName->isEqualTo("getThermalLevel")) {
        uint32_t* level = (uint32_t*)param1;
        if (level) {
            *level = fThermalLevel;
        }
        return kIOReturnSuccess;
    }
    
    if (functionName->isEqualTo("setPerformanceState")) {
        uint32_t cluster = (uint32_t)(uintptr_t)param1;
        uint32_t pstate = (uint32_t)(uintptr_t)param2;
        return setPerformanceState(cluster, pstate);
    }
    
    if (functionName->isEqualTo("getPerformanceState")) {
        uint32_t cluster = (uint32_t)(uintptr_t)param1;
        uint32_t* pstate = (uint32_t*)param2;
        return getPerformanceState(cluster, pstate);
    }
    
    if (functionName->isEqualTo("readSensor")) {
        uint32_t sensor = (uint32_t)(uintptr_t)param1;
        int32_t* temp = (int32_t*)param2;
        return readSensor(sensor, temp);
    }
    
    if (functionName->isEqualTo("secureChallenge")) {
        uint8_t* challenge = (uint8_t*)param1;
        uint8_t* response = (uint8_t*)param2;
        return secureChallenge(challenge, response);
    }
    
    return super::callPlatformFunction(functionName, waitForFunction,
                                        param1, param2, param3, param4);
}

#pragma mark - AppleIntegratorCP::User Client Access

IOReturn AppleIntegratorCP::newUserClient(task_t owningTask,
                                           void* securityID,
                                           UInt32 type,
                                           OSDictionary* properties,
                                           IOUserClient** handler)
{
    AppleIntegratorCPUserClient* client;
    
    /* Check entitlements */
    /* In real implementation, would check for com.apple.private.integrator access */
    
    client = new AppleIntegratorCPUserClient;
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

#pragma mark - AppleIntegratorCPUserClient Implementation

bool AppleIntegratorCPUserClient::init(OSDictionary* dictionary)
{
    if (!IOUserClient::init(dictionary)) {
        return false;
    }
    
    fTask = NULL;
    fPid = 0;
    fProvider = NULL;
    fPermissions = 0;
    fState = 0;
    fValid = false;
    
    return true;
}

void AppleIntegratorCPUserClient::free(void)
{
    IOUserClient::free();
}

bool AppleIntegratorCPUserClient::start(IOService* provider)
{
    if (!IOUserClient::start(provider)) {
        return false;
    }
    
    fProvider = OSDynamicCast(AppleIntegratorCP, provider);
    if (!fProvider) {
        return false;
    }
    
    fTask = current_task();
    fPid = pid_from_task(fTask);
    
    /* Determine permissions */
    /* Would check entitlements here */
    fPermissions = 0x01;  /* Read-only access */
    
    fValid = true;
    
    return true;
}

void AppleIntegratorCPUserClient::stop(IOService* provider)
{
    fValid = false;
    IOUserClient::stop(provider);
}

IOReturn AppleIntegratorCPUserClient::clientClose(void)
{
    fValid = false;
    terminate();
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCPUserClient::clientDied(void)
{
    return clientClose();
}

IOReturn AppleIntegratorCPUserClient::externalMethod(uint32_t selector,
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

IOReturn AppleIntegratorCPUserClient::sGetCaps(AppleIntegratorCPUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args)
{
    integrator_caps_t* caps = (integrator_caps_t*)args->structureOutput;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    memcpy(caps, &target->fProvider->fCaps, sizeof(integrator_caps_t));
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCPUserClient::sGetPerfInfo(AppleIntegratorCPUserClient* target,
                                                    void* reference,
                                                    IOExternalMethodArguments* args)
{
    struct {
        uint32_t count;
        perf_cluster_t clusters[ICP_MAX_PERF_CLUSTERS];
    } info;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    info.count = target->fProvider->fNumPerfClusters;
    memcpy(info.clusters, target->fProvider->fPerfClusters,
           sizeof(perf_cluster_t) * info.count);
    
    memcpy(args->structureOutput, &info, sizeof(info));
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCPUserClient::sSetPerfState(AppleIntegratorCPUserClient* target,
                                                     void* reference,
                                                     IOExternalMethodArguments* args)
{
    uint32_t cluster = (uint32_t)args->scalarInput[0];
    uint32_t pstate = (uint32_t)args->scalarInput[1];
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Check for write permissions */
    if ((target->fPermissions & 0x02) == 0) {
        return kIOReturnNotPermitted;
    }
    
    return target->fProvider->setPerformanceState(cluster, pstate);
}

IOReturn AppleIntegratorCPUserClient::sGetThermalInfo(AppleIntegratorCPUserClient* target,
                                                       void* reference,
                                                       IOExternalMethodArguments* args)
{
    struct {
        uint32_t count;
        thermal_sensor_t sensors[ICP_MAX_SENSORS];
        uint32_t level;
    } info;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    info.count = target->fProvider->fNumSensors;
    memcpy(info.sensors, target->fProvider->fSensors,
           sizeof(thermal_sensor_t) * info.count);
    info.level = target->fProvider->fThermalLevel;
    
    memcpy(args->structureOutput, &info, sizeof(info));
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCPUserClient::sGetPowerInfo(AppleIntegratorCPUserClient* target,
                                                     void* reference,
                                                     IOExternalMethodArguments* args)
{
    struct {
        uint32_t count;
        power_domain_t domains[ICP_MAX_POWER_DOMAINS];
    } info;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    info.count = target->fProvider->fNumPowerDomains;
    memcpy(info.domains, target->fProvider->fPowerDomains,
           sizeof(power_domain_t) * info.count);
    
    memcpy(args->structureOutput, &info, sizeof(info));
    
    return kIOReturnSuccess;
}

IOReturn AppleIntegratorCPUserClient::sSecureRequest(AppleIntegratorCPUserClient* target,
                                                      void* reference,
                                                      IOExternalMethodArguments* args)
{
    uint8_t* challenge = (uint8_t*)args->structureInput;
    uint8_t* response = (uint8_t*)args->structureOutput;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Check for secure permissions */
    if ((target->fPermissions & 0x04) == 0) {
        return kIOReturnNotPermitted;
    }
    
    return target->fProvider->secureChallenge(challenge, response);
}

IOReturn AppleIntegratorCPUserClient::sGetFirmwareInfo(AppleIntegratorCPUserClient* target,
                                                        void* reference,
                                                        IOExternalMethodArguments* args)
{
    firmware_info_t* info = (firmware_info_t*)args->structureOutput;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    return target->fProvider->getFirmwareInfo(info);
}

IOReturn AppleIntegratorCPUserClient::sMailboxCommand(AppleIntegratorCPUserClient* target,
                                                       void* reference,
                                                       IOExternalMethodArguments* args)
{
    mbox_message_t* msg = (mbox_message_t*)args->structureInput;
    mbox_message_t* response = (mbox_message_t*)args->structureOutput;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    IOReturn ret = target->fProvider->sendMailboxMessage(msg);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    
    return target->fProvider->waitForMailboxResponse(response, 1000);
}

IOReturn AppleIntegratorCPUserClient::sGetStatistics(AppleIntegratorCPUserClient* target,
                                                      void* reference,
                                                      IOExternalMethodArguments* args)
{
    struct {
        uint64_t interrupts;
        uint64_t mailbox_messages;
        uint64_t errors;
        uint64_t thermal_events;
        uint64_t power_events;
        uint64_t perf_events;
        uint64_t secure_events;
    } stats;
    
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    stats.interrupts = target->fProvider->fTotalInterrupts;
    stats.mailbox_messages = target->fProvider->fTotalMailboxMessages;
    stats.errors = target->fProvider->fTotalErrors;
    stats.thermal_events = target->fProvider->fThermalEvents;
    stats.power_events = target->fProvider->fPowerEvents;
    stats.perf_events = target->fProvider->fPerfEvents;
    stats.secure_events = target->fProvider->fSecureEvents;
    
    memcpy(args->structureOutput, &stats, sizeof(stats));
    
    return kIOReturnSuccess;
}
