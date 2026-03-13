/*
 * Copyright (c) 2007-2022 Apple Inc. All rights reserved.
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
#include <IOKit/hidsystem/IOHIDevice.h>
#include <IOKit/hidsystem/IOLLEvent.h>

#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/OSAtomic.h>

#include <machine/machine_routines.h>

/*==============================================================================
 * AppleM68Buttons - Buttons Controller for M68K-based Apple Devices
 * 
 * This driver handles physical buttons on Apple devices including:
 * - Power button
 * - Volume up/down buttons
 * - Ring/Silent switch
 * - Home button (iOS devices)
 * - Hold switch (iPod)
 * - Play/Pause button
 * - Menu button (iPod)
 * - Click wheel (iPod)
 *==============================================================================*/

/*==============================================================================
 * Constants and Definitions
 *==============================================================================*/

#define APPLE_M68_BUTTONS_VERSION       0x00010001  /* Version 1.1 */
#define APPLE_M68_BUTTONS_REVISION      0x00000001

/* Maximum values */
#define MAX_BUTTONS                     32
#define MAX_DEBOUNCE_SAMPLES             8
#define MAX_MULTI_TAP_INTERVAL           500  /* ms */
#define MAX_LONG_PRESS_DURATION          2000 /* ms */
#define MAX_VERY_LONG_PRESS_DURATION     5000 /* ms */
#define MAX_CLICK_WHEEL_POSITIONS         256
#define MAX_CLICK_WHEEL_VELOCITY          100

/* Debounce settings */
#define DEBOUNCE_DEFAULT_TIME            20   /* ms */
#define DEBOUNCE_FAST_TIME                5   /* ms for click wheel */
#define DEBOUNCE_SLOW_TIME                50  /* ms for noisy buttons */

/* Click wheel sensitivity */
#define CLICK_WHEEL_SENSITIVITY_LOW       0
#define CLICK_WHEEL_SENSITIVITY_MEDIUM    1
#define CLICK_WHEEL_SENSITIVITY_HIGH      2
#define CLICK_WHEEL_SENSITIVITY_MAX       3

/*==============================================================================
 * Button Types
 *==============================================================================*/

enum {
    kButtonTypePower               = 0x01,
    kButtonTypeVolumeUp             = 0x02,
    kButtonTypeVolumeDown           = 0x03,
    kButtonTypeHome                 = 0x04,
    kButtonTypeMenu                 = 0x05,
    kButtonTypePlayPause            = 0x06,
    kButtonTypeNextTrack            = 0x07,
    kButtonTypePreviousTrack        = 0x08,
    kButtonTypeHoldSwitch           = 0x09,
    kButtonTypeRingSilentSwitch     = 0x0A,
    kButtonTypeClickWheel           = 0x0B,
    kButtonTypeCenter                = 0x0C,
    kButtonTypeLock                  = 0x0D,
    kButtonTypeCamera                = 0x0E,
    kButtonTypeSiri                  = 0x0F,
    kButtonTypeAction                = 0x10,
    kButtonTypeUserDefined           = 0x80
};

/*==============================================================================
 * Button Events
 *==============================================================================*/

enum {
    kButtonEventDown                = 0x01,
    kButtonEventUp                  = 0x02,
    kButtonEventClick               = 0x03,
    kButtonEventDoubleClick         = 0x04,
    kButtonEventTripleClick         = 0x05,
    kButtonEventLongPress           = 0x06,
    kButtonEventVeryLongPress       = 0x07,
    kButtonEventMultiTap            = 0x08,
    kButtonEventHold                = 0x09,
    kButtonEventRotate              = 0x0A,
    kButtonEventRotateAccelerated   = 0x0B,
    kButtonEventRotateDecelerated   = 0x0C
};

/*==============================================================================
 * Button States
 *==============================================================================*/

enum {
    kButtonStateIdle                = 0,
    kButtonStateDebouncing          = 1,
    kButtonStateDown                = 2,
    kButtonStateHeld                = 3,
    kButtonStateLongPress           = 4,
    kButtonStateVeryLongPress       = 5,
    kButtonStateTapCount            = 6,
    kButtonStateWaitingForMulti     = 7,
    kButtonStateDisabled            = 8,
    kButtonStateFault               = 9
};

/*==============================================================================
 * Switch States
 *==============================================================================*/

enum {
    kSwitchStateOff                 = 0,
    kSwitchStateOn                  = 1,
    kSwitchStateUnknown             = 2
};

/*==============================================================================
 * Click Wheel Directions
 *==============================================================================*/

enum {
    kWheelDirectionNone             = 0,
    kWheelDirectionLeft             = 1,
    kWheelDirectionRight            = 2,
    kWheelDirectionUp               = 3,
    kWheelDirectionDown             = 4,
    kWheelDirectionClockwise        = 5,
    kWheelDirectionCounterClockwise = 6
};

/*==============================================================================
 * Hardware Register Definitions (M68K Buttons Controller)
 * 
 * Based on Apple's M68K series button controllers used in:
 * - iPod Classic (1st-6th gen)
 * - iPod Mini
 * - iPod Nano
 * - iPhone 2G/3G/3GS
 * - Various Apple accessories
 *==============================================================================*/

/* Register offsets */
#define M68_BTN_REG_ID              0x00   /* Controller ID */
#define M68_BTN_REG_VERSION          0x02   /* Controller version */
#define M68_BTN_REG_STATUS           0x04   /* Status register */
#define M68_BTN_REG_CONTROL          0x06   /* Control register */
#define M68_BTN_REG_INT_ENABLE       0x08   /* Interrupt enable */
#define M68_BTN_REG_INT_STATUS       0x0A   /* Interrupt status */
#define M68_BTN_REG_INT_CLEAR        0x0C   /* Interrupt clear */
#define M68_BTN_REG_BUTTON_STATE     0x10   /* Current button state (bitmap) */
#define M68_BTN_REG_BUTTON_EDGE      0x12   /* Button edge detect */
#define M68_BTN_REG_DEBOUNCE         0x14   /* Debounce control */
#define M68_BTN_REG_CLICK_WHEEL_POS  0x20   /* Click wheel position */
#define M68_BTN_REG_CLICK_WHEEL_DELTA 0x22  /* Click wheel delta */
#define M68_BTN_REG_CLICK_WHEEL_VEL  0x24   /* Click wheel velocity */
#define M68_BTN_REG_SWITCH_STATE     0x30   /* Switch states (hold/ring) */
#define M68_BTN_REG_GPIO_CTRL        0x40   /* GPIO control */
#define M68_BTN_REG_POWER_STATE      0x50   /* Power state */
#define M68_BTN_REG_DEBUG             0x60   /* Debug register */

/* Status bits */
#define M68_BTN_STATUS_READY          (1 << 0)
#define M68_BTN_STATUS_BUTTON_CHANGED (1 << 1)
#define M68_BTN_STATUS_WHEEL_MOVED    (1 << 2)
#define M68_BTN_STATUS_SWITCH_CHANGED (1 << 3)
#define M68_BTN_STATUS_ERROR          (1 << 7)

/* Control bits */
#define M68_BTN_CTRL_ENABLE           (1 << 0)
#define M68_BTN_CTRL_RESET            (1 << 1)
#define M68_BTN_CTRL_SLEEP             (1 << 2)
#define M68_BTN_CTRL_WAKE              (1 << 3)
#define M68_BTN_CTRL_DEBOUNCE_ENABLE   (1 << 4)

/* Interrupt bits */
#define M68_BTN_INT_BUTTON            (1 << 0)
#define M68_BTN_INT_WHEEL             (1 << 1)
#define M68_BTN_INT_SWITCH            (1 << 2)
#define M68_BTN_INT_ERROR             (1 << 3)

/* Button masks (varies by device) */
#define M68_BTN_MASK_POWER            (1 << 0)
#define M68_BTN_MASK_VOLUME_UP        (1 << 1)
#define M68_BTN_MASK_VOLUME_DOWN      (1 << 2)
#define M68_BTN_MASK_HOME             (1 << 3)
#define M68_BTN_MASK_MENU             (1 << 4)
#define M68_BTN_MASK_PLAY             (1 << 5)
#define M68_BTN_MASK_CENTER            (1 << 6)
#define M68_BTN_MASK_LOCK              (1 << 7)
#define M68_BTN_MASK_HOLD              (1 << 8)
#define M68_BTN_MASK_RING_SILENT      (1 << 9)

/*==============================================================================
 * Button Configuration Structure
 *==============================================================================*/

typedef struct {
    uint8_t     button_id;                  /* Button identifier */
    uint16_t    hardware_mask;              /* Hardware bit mask */
    uint8_t     type;                       /* Button type */
    uint8_t     default_state;              /* Default state (for switches) */
    uint8_t     invert;                     /* Invert logic */
    uint8_t     debounce_time;               /* Debounce time in ms */
    uint8_t     multi_tap_enabled;           /* Enable multi-tap detection */
    uint8_t     long_press_enabled;          /* Enable long press detection */
    uint16_t    long_press_time;             /* Long press time in ms */
    uint16_t    very_long_press_time;        /* Very long press time in ms */
    uint8_t     repeat_enabled;               /* Enable repeat when held */
    uint16_t    repeat_interval;              /* Repeat interval in ms */
    uint8_t     click_wheel;                   /* Is click wheel */
    uint8_t     sensitivity;                   /* Click wheel sensitivity */
    uint8_t     reserved[6];
} button_config_t;

/*==============================================================================
 * Button State Structure
 *==============================================================================*/

typedef struct {
    uint8_t     button_id;                  /* Button identifier */
    uint8_t     state;                       /* Current state */
    uint8_t     physical_state;               /* Raw physical state */
    uint8_t     debounced_state;               /* Debounced state */
    uint64_t    last_change_time;              /* Last state change time */
    uint64_t    down_time;                     /* Time when button went down */
    uint8_t     tap_count;                     /* Current tap count */
    uint8_t     pending_events;                 /* Pending events */
    uint32_t    repeat_count;                   /* Repeat count */
    int32_t     wheel_position;                  /* Current wheel position */
    int32_t     wheel_delta;                     /* Wheel delta */
    int32_t     wheel_velocity;                   /* Wheel velocity */
    uint8_t     wheel_direction;                  /* Wheel direction */
    uint8_t     reserved[3];
} button_state_t;

/*==============================================================================
 * AppleM68Buttons Main Class
 *==============================================================================*/

class AppleM68Buttons : public IOHIDevice
{
    OSDeclareDefaultStructors(AppleM68Buttons)
    
private:
    /* IOKit resources */
    IOWorkLoop*                 fWorkLoop;
    IOCommandGate*              fCommandGate;
    IOTimerEventSource*         fDebounceTimer;
    IOTimerEventSource*         fLongPressTimer;
    IOTimerEventSource*         fRepeatTimer;
    IOInterruptEventSource*     fInterruptSource;
    
    /* Hardware resources */
    IOMemoryMap*                fMemoryMap;
    volatile uint16_t*          fRegisters;
    IOPhysicalAddress           fPhysicalAddress;
    IOPhysicalLength            fPhysicalLength;
    
    /* Hardware identification */
    uint16_t                    fControllerID;
    uint16_t                    fControllerVersion;
    uint16_t                    fHardwareStatus;
    uint32_t                    fFeatures;
    
    /* Button configuration */
    button_config_t             fButtonConfigs[MAX_BUTTONS];
    uint32_t                    fButtonCount;
    
    /* Button state */
    button_state_t              fButtonStates[MAX_BUTTONS];
    lck_mtx_t*                  fStateLock;
    
    /* Event queue */
    struct {
        uint8_t     button_id;
        uint8_t     event_type;
        uint32_t    value;
        uint64_t    timestamp;
    } fEventQueue[32];
    uint32_t                    fEventQueueHead;
    uint32_t                    fEventQueueTail;
    lck_mtx_t*                  fEventLock;
    
    /* Click wheel state */
    int32_t                     fWheelPosition;
    int32_t                     fWheelDelta;
    int32_t                     fWheelVelocity;
    uint8_t                     fWheelSensitivity;
    uint64_t                    fLastWheelTime;
    
    /* Switch states */
    uint8_t                     fHoldSwitchState;
    uint8_t                     fRingSilentState;
    
    /* Power management */
    uint32_t                    fPowerState;
    uint32_t                    fWakeupEnabled;
    
    /* Statistics */
    uint64_t                    fTotalEvents;
    uint64_t                    fTotalInterrupts;
    uint64_t                    fDebouncedEvents;
    uint64_t                    fMultiTaps;
    uint64_t                    fLongPresses;
    uint64_t                    fWheelEvents;
    uint64_t                    fErrors;
    
    /* Configuration */
    uint32_t                    fDebounceTime;
    uint32_t                    fMultiTapInterval;
    uint32_t                    fLongPressTime;
    uint32_t                    fVeryLongPressTime;
    bool                        fTapTrackingEnabled;
    bool                        fHoldEnabled;
    
    /*==============================================================================
     * Private Methods
     *==============================================================================*/
    
    /* Hardware access */
    uint16_t                    readReg(uint32_t offset);
    void                        writeReg(uint32_t offset, uint16_t value);
    uint16_t                    readButtonState(void);
    int16_t                     readWheelDelta(void);
    uint16_t                    readSwitchState(void);
    
    /* Hardware initialization */
    bool                        mapRegisters(void);
    void                        unmapRegisters(void);
    bool                        resetController(void);
    bool                        readHardwareVersion(void);
    bool                        configureHardware(void);
    
    /* Button processing */
    void                        processButtons(uint16_t buttonState);
    void                        processButton(int buttonIndex, bool pressed);
    void                        processClickWheel(int16_t delta);
    void                        processSwitches(uint16_t switchState);
    
    /* Debounce handling */
    void                        startDebounce(int buttonIndex);
    void                        debounceComplete(void);
    static void                 debounceHandler(OSObject* owner, IOTimerEventSource* sender);
    
    /* Long press handling */
    void                        startLongPressTimer(int buttonIndex);
    void                        cancelLongPressTimer(void);
    void                        longPressComplete(void);
    static void                 longPressHandler(OSObject* owner, IOTimerEventSource* sender);
    
    /* Repeat handling */
    void                        startRepeatTimer(int buttonIndex);
    void                        cancelRepeatTimer(void);
    void                        repeatHandler(void);
    static void                 repeatHandler(OSObject* owner, IOTimerEventSource* sender);
    
    /* Interrupt handling */
    void                        interruptHandler(void);
    static void                 interruptHandler(OSObject* owner, IOInterruptEventSource* src, int count);
    
    /* Event management */
    void                        queueEvent(uint8_t buttonId, uint8_t eventType, uint32_t value);
    bool                        dequeueEvent(uint8_t* buttonId, uint8_t* eventType, uint32_t* value, uint64_t* timestamp);
    void                        dispatchEvent(uint8_t buttonId, uint8_t eventType, uint32_t value);
    
    /* Timer management */
    bool                        createTimers(void);
    void                        destroyTimers(void);
    
    /* Configuration */
    void                        loadPlatformConfig(void);
    void                        setDefaultConfig(void);
    
    /* Utility */
    uint64_t                    getCurrentTime(void);
    int                         findButtonByMask(uint16_t mask);
    int                         findButtonById(uint8_t buttonId);
    
protected:
    bool                        init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                        free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                        start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                        stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    /* IOHIDevice overrides */
    virtual IOReturn            setProperties(OSObject* properties) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            handleOpen(IOService* client, IOOptionBits options,
                                           void* arg) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            message(UInt32 type, IOService* provider,
                                        void* argument) APPLE_KEXT_OVERRIDE;
    
    /* Power management */
    virtual IOReturn            setPowerState(unsigned long powerState,
                                              IOService* device) APPLE_KEXT_OVERRIDE;
    
    /* User client access */
    virtual IOReturn            newUserClient(task_t owningTask,
                                              void* securityID,
                                              UInt32 type,
                                              OSDictionary* properties,
                                              IOUserClient** handler) APPLE_KEXT_OVERRIDE;
    
    /* Public API */
    virtual uint32_t            getButtonCount(void) { return fButtonCount; }
    virtual uint32_t            getWheelPosition(void) { return fWheelPosition; }
    virtual uint8_t             getHoldSwitchState(void) { return fHoldSwitchState; }
    virtual uint8_t             getRingSilentState(void) { return fRingSilentState; }
    
    virtual IOReturn            setDebounceTime(uint32_t ms);
    virtual IOReturn            setMultiTapInterval(uint32_t ms);
    virtual IOReturn            setLongPressTime(uint32_t ms);
    virtual IOReturn            setWheelSensitivity(uint8_t sensitivity);
};

OSDefineMetaClassAndStructors(AppleM68Buttons, IOHIDevice)

/*==============================================================================
 * AppleM68Buttons Implementation
 *==============================================================================*/

#pragma mark - AppleM68Buttons::Initialization

bool AppleM68Buttons::init(OSDictionary* dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    
    /* Initialize member variables */
    fWorkLoop = NULL;
    fCommandGate = NULL;
    fDebounceTimer = NULL;
    fLongPressTimer = NULL;
    fRepeatTimer = NULL;
    fInterruptSource = NULL;
    
    fMemoryMap = NULL;
    fRegisters = NULL;
    fPhysicalAddress = 0;
    fPhysicalLength = 0;
    
    fControllerID = 0;
    fControllerVersion = 0;
    fHardwareStatus = 0;
    fFeatures = 0;
    
    fButtonCount = 0;
    bzero(fButtonConfigs, sizeof(fButtonConfigs));
    bzero(fButtonStates, sizeof(fButtonStates));
    
    fEventQueueHead = 0;
    fEventQueueTail = 0;
    bzero(fEventQueue, sizeof(fEventQueue));
    
    fWheelPosition = 0;
    fWheelDelta = 0;
    fWheelVelocity = 0;
    fWheelSensitivity = CLICK_WHEEL_SENSITIVITY_MEDIUM;
    fLastWheelTime = 0;
    
    fHoldSwitchState = kSwitchStateUnknown;
    fRingSilentState = kSwitchStateUnknown;
    
    fPowerState = 0;
    fWakeupEnabled = 0;
    
    fTotalEvents = 0;
    fTotalInterrupts = 0;
    fDebouncedEvents = 0;
    fMultiTaps = 0;
    fLongPresses = 0;
    fWheelEvents = 0;
    fErrors = 0;
    
    fDebounceTime = DEBOUNCE_DEFAULT_TIME;
    fMultiTapInterval = MAX_MULTI_TAP_INTERVAL;
    fLongPressTime = MAX_LONG_PRESS_DURATION;
    fVeryLongPressTime = MAX_VERY_LONG_PRESS_DURATION;
    fTapTrackingEnabled = true;
    fHoldEnabled = true;
    
    /* Create lock group and locks */
    fStateLock = lck_mtx_alloc_init(IOService::getKernelLockGroup(), LCK_ATTR_NULL);
    fEventLock = lck_mtx_alloc_init(IOService::getKernelLockGroup(), LCK_ATTR_NULL);
    
    if (!fStateLock || !fEventLock) {
        return false;
    }
    
    return true;
}

void AppleM68Buttons::free(void)
{
    /* Free locks */
    if (fStateLock) {
        lck_mtx_free(fStateLock, IOService::getKernelLockGroup());
        fStateLock = NULL;
    }
    
    if (fEventLock) {
        lck_mtx_free(fEventLock, IOService::getKernelLockGroup());
        fEventLock = NULL;
    }
    
    /* Unmap registers */
    unmapRegisters();
    
    super::free();
}

#pragma mark - AppleM68Buttons::Hardware Access

uint16_t AppleM68Buttons::readReg(uint32_t offset)
{
    if (!fRegisters || offset >= fPhysicalLength) {
        return 0xFFFF;
    }
    
    uint16_t value = fRegisters[offset / 2];
    
    /* Add delay for slow hardware if needed */
    if (fControllerVersion < 0x0200) {
        IODelay(1);
    }
    
    return value;
}

void AppleM68Buttons::writeReg(uint32_t offset, uint16_t value)
{
    if (!fRegisters || offset >= fPhysicalLength) {
        return;
    }
    
    fRegisters[offset / 2] = value;
    
    /* Add delay for slow hardware if needed */
    if (fControllerVersion < 0x0200) {
        IODelay(1);
    }
}

uint16_t AppleM68Buttons::readButtonState(void)
{
    return readReg(M68_BTN_REG_BUTTON_STATE);
}

int16_t AppleM68Buttons::readWheelDelta(void)
{
    return (int16_t)readReg(M68_BTN_REG_CLICK_WHEEL_DELTA);
}

uint16_t AppleM68Buttons::readSwitchState(void)
{
    return readReg(M68_BTN_REG_SWITCH_STATE);
}

#pragma mark - AppleM68Buttons::Hardware Initialization

bool AppleM68Buttons::mapRegisters(void)
{
    IOService* provider = getProvider();
    if (!provider) {
        IOLog("AppleM68Buttons: No provider\n");
        return false;
    }
    
    IODeviceMemory* memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory) {
        IOLog("AppleM68Buttons: No device memory\n");
        return false;
    }
    
    fPhysicalAddress = memory->getPhysicalAddress();
    fPhysicalLength = memory->getLength();
    
    fMemoryMap = memory->makeMapping(kIOMapAny | kIOMapInhibitCache);
    if (!fMemoryMap) {
        IOLog("AppleM68Buttons: Failed to map registers\n");
        return false;
    }
    
    fRegisters = (volatile uint16_t*)fMemoryMap->getVirtualAddress();
    if (!fRegisters) {
        IOLog("AppleM68Buttons: Invalid virtual address\n");
        fMemoryMap->release();
        fMemoryMap = NULL;
        return false;
    }
    
    IOLog("AppleM68Buttons: Mapped registers at %p (phys: 0x%llx, size: %lld)\n",
          fRegisters, fPhysicalAddress, fPhysicalLength);
    
    return true;
}

void AppleM68Buttons::unmapRegisters(void)
{
    if (fMemoryMap) {
        fMemoryMap->release();
        fMemoryMap = NULL;
        fRegisters = NULL;
    }
}

bool AppleM68Buttons::resetController(void)
{
    /* Reset the controller */
    writeReg(M68_BTN_REG_CONTROL, M68_BTN_CTRL_RESET);
    
    /* Wait for reset to complete */
    for (int i = 0; i < 100; i++) {
        if ((readReg(M68_BTN_REG_STATUS) & M68_BTN_STATUS_READY) != 0) {
            break;
        }
        IODelay(1000);  /* 1ms */
    }
    
    /* Clear any pending interrupts */
    writeReg(M68_BTN_REG_INT_CLEAR, 0xFFFF);
    
    /* Enable interrupts */
    writeReg(M68_BTN_REG_INT_ENABLE, M68_BTN_INT_BUTTON | M68_BTN_INT_WHEEL | M68_BTN_INT_SWITCH);
    
    /* Enable controller */
    writeReg(M68_BTN_REG_CONTROL, M68_BTN_CTRL_ENABLE | M68_BTN_CTRL_DEBOUNCE_ENABLE);
    
    return true;
}

bool AppleM68Buttons::readHardwareVersion(void)
{
    fControllerID = readReg(M68_BTN_REG_ID);
    fControllerVersion = readReg(M68_BTN_REG_VERSION);
    
    IOLog("AppleM68Buttons: Controller ID: 0x%04x, Version: 0x%04x\n",
          fControllerID, fControllerVersion);
    
    return true;
}

bool AppleM68Buttons::configureHardware(void)
{
    /* Set debounce time */
    writeReg(M68_BTN_REG_DEBOUNCE, fDebounceTime);
    
    /* Configure GPIO if needed */
    writeReg(M68_BTN_REG_GPIO_CTRL, 0x0000);
    
    return true;
}

#pragma mark - AppleM68Buttons::Configuration

void AppleM68Buttons::setDefaultConfig(void)
{
    /* Default configuration for iPod/iPhone buttons */
    
    /* Power button */
    fButtonConfigs[0].button_id = kButtonTypePower;
    fButtonConfigs[0].hardware_mask = M68_BTN_MASK_POWER;
    fButtonConfigs[0].type = kButtonTypePower;
    fButtonConfigs[0].invert = 0;
    fButtonConfigs[0].debounce_time = DEBOUNCE_DEFAULT_TIME;
    fButtonConfigs[0].multi_tap_enabled = 0;
    fButtonConfigs[0].long_press_enabled = 1;
    fButtonConfigs[0].long_press_time = 2000;
    fButtonConfigs[0].very_long_press_time = 5000;
    fButtonConfigs[0].repeat_enabled = 0;
    
    /* Volume up */
    fButtonConfigs[1].button_id = kButtonTypeVolumeUp;
    fButtonConfigs[1].hardware_mask = M68_BTN_MASK_VOLUME_UP;
    fButtonConfigs[1].type = kButtonTypeVolumeUp;
    fButtonConfigs[1].invert = 0;
    fButtonConfigs[1].debounce_time = DEBOUNCE_DEFAULT_TIME;
    fButtonConfigs[1].multi_tap_enabled = 0;
    fButtonConfigs[1].long_press_enabled = 1;
    fButtonConfigs[1].long_press_time = 500;
    fButtonConfigs[1].repeat_enabled = 1;
    fButtonConfigs[1].repeat_interval = 100;
    
    /* Volume down */
    fButtonConfigs[2].button_id = kButtonTypeVolumeDown;
    fButtonConfigs[2].hardware_mask = M68_BTN_MASK_VOLUME_DOWN;
    fButtonConfigs[2].type = kButtonTypeVolumeDown;
    fButtonConfigs[2].invert = 0;
    fButtonConfigs[2].debounce_time = DEBOUNCE_DEFAULT_TIME;
    fButtonConfigs[2].multi_tap_enabled = 0;
    fButtonConfigs[2].long_press_enabled = 1;
    fButtonConfigs[2].long_press_time = 500;
    fButtonConfigs[2].repeat_enabled = 1;
    fButtonConfigs[2].repeat_interval = 100;
    
    /* Home button */
    fButtonConfigs[3].button_id = kButtonTypeHome;
    fButtonConfigs[3].hardware_mask = M68_BTN_MASK_HOME;
    fButtonConfigs[3].type = kButtonTypeHome;
    fButtonConfigs[3].invert = 0;
    fButtonConfigs[3].debounce_time = DEBOUNCE_DEFAULT_TIME;
    fButtonConfigs[3].multi_tap_enabled = 1;
    fButtonConfigs[3].long_press_enabled = 1;
    fButtonConfigs[3].long_press_time = 1000;
    
    /* Menu button (iPod) */
    fButtonConfigs[4].button_id = kButtonTypeMenu;
    fButtonConfigs[4].hardware_mask = M68_BTN_MASK_MENU;
    fButtonConfigs[4].type = kButtonTypeMenu;
    fButtonConfigs[4].invert = 0;
    fButtonConfigs[4].debounce_time = DEBOUNCE_DEFAULT_TIME;
    fButtonConfigs[4].multi_tap_enabled = 0;
    fButtonConfigs[4].long_press_enabled = 1;
    fButtonConfigs[4].long_press_time = 1000;
    
    /* Play/Pause button */
    fButtonConfigs[5].button_id = kButtonTypePlayPause;
    fButtonConfigs[5].hardware_mask = M68_BTN_MASK_PLAY;
    fButtonConfigs[5].type = kButtonTypePlayPause;
    fButtonConfigs[5].invert = 0;
    fButtonConfigs[5].debounce_time = DEBOUNCE_DEFAULT_TIME;
    fButtonConfigs[5].multi_tap_enabled = 1;
    fButtonConfigs[5].long_press_enabled = 1;
    fButtonConfigs[5].long_press_time = 500;
    
    /* Center button (click wheel) */
    fButtonConfigs[6].button_id = kButtonTypeCenter;
    fButtonConfigs[6].hardware_mask = M68_BTN_MASK_CENTER;
    fButtonConfigs[6].type = kButtonTypeCenter;
    fButtonConfigs[6].invert = 0;
    fButtonConfigs[6].debounce_time = DEBOUNCE_DEFAULT_TIME;
    fButtonConfigs[6].multi_tap_enabled = 1;
    fButtonConfigs[6].long_press_enabled = 1;
    fButtonConfigs[6].long_press_time = 500;
    
    /* Hold switch */
    fButtonConfigs[7].button_id = kButtonTypeHoldSwitch;
    fButtonConfigs[7].hardware_mask = M68_BTN_MASK_HOLD;
    fButtonConfigs[7].type = kButtonTypeHoldSwitch;
    fButtonConfigs[7].default_state = kSwitchStateOff;
    fButtonConfigs[7].invert = 1;  /* Many devices invert hold switch logic */
    fButtonConfigs[7].debounce_time = DEBOUNCE_SLOW_TIME;
    
    /* Ring/Silent switch */
    fButtonConfigs[8].button_id = kButtonTypeRingSilentSwitch;
    fButtonConfigs[8].hardware_mask = M68_BTN_MASK_RING_SILENT;
    fButtonConfigs[8].type = kButtonTypeRingSilentSwitch;
    fButtonConfigs[8].default_state = kSwitchStateOn;  /* Ring mode */
    fButtonConfigs[8].invert = 0;
    fButtonConfigs[8].debounce_time = DEBOUNCE_SLOW_TIME;
    
    /* Click wheel */
    fButtonConfigs[9].button_id = kButtonTypeClickWheel;
    fButtonConfigs[9].type = kButtonTypeClickWheel;
    fButtonConfigs[9].click_wheel = 1;
    fButtonConfigs[9].sensitivity = CLICK_WHEEL_SENSITIVITY_MEDIUM;
    
    fButtonCount = 10;
}

void AppleM68Buttons::loadPlatformConfig(void)
{
    /* Load platform-specific configuration from device tree */
    IORegistryEntry* entry = IORegistryEntry::fromPath("/", gIODTPlane);
    if (!entry) {
        setDefaultConfig();
        return;
    }
    
    OSData* data = NULL;
    
    /* Check for custom button mapping */
    data = OSDynamicCast(OSData, entry->getProperty("button-config"));
    if (data) {
        /* Would parse custom configuration */
    }
    
    /* Check for click wheel sensitivity */
    data = OSDynamicCast(OSData, entry->getProperty("wheel-sensitivity"));
    if (data && data->getLength() >= 1) {
        fWheelSensitivity = *(uint8_t*)data->getBytesNoCopy();
    }
    
    /* Check for debounce time */
    data = OSDynamicCast(OSData, entry->getProperty("debounce-time"));
    if (data && data->getLength() >= 2) {
        fDebounceTime = *(uint16_t*)data->getBytesNoCopy();
    }
    
    entry->release();
    
    /* If no config loaded, use defaults */
    if (fButtonCount == 0) {
        setDefaultConfig();
    }
}

#pragma mark - AppleM68Buttons::Timer Management

bool AppleM68Buttons::createTimers(void)
{
    fDebounceTimer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &AppleM68Buttons::debounceHandler));
    
    if (!fDebounceTimer) {
        return false;
    }
    
    fLongPressTimer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &AppleM68Buttons::longPressHandler));
    
    if (!fLongPressTimer) {
        return false;
    }
    
    fRepeatTimer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &AppleM68Buttons::repeatHandler));
    
    if (!fRepeatTimer) {
        return false;
    }
    
    fWorkLoop->addEventSource(fDebounceTimer);
    fWorkLoop->addEventSource(fLongPressTimer);
    fWorkLoop->addEventSource(fRepeatTimer);
    
    return true;
}

void AppleM68Buttons::destroyTimers(void)
{
    if (fWorkLoop) {
        if (fDebounceTimer) {
            fDebounceTimer->cancelTimeout();
            fWorkLoop->removeEventSource(fDebounceTimer);
            fDebounceTimer->release();
            fDebounceTimer = NULL;
        }
        
        if (fLongPressTimer) {
            fLongPressTimer->cancelTimeout();
            fWorkLoop->removeEventSource(fLongPressTimer);
            fLongPressTimer->release();
            fLongPressTimer = NULL;
        }
        
        if (fRepeatTimer) {
            fRepeatTimer->cancelTimeout();
            fWorkLoop->removeEventSource(fRepeatTimer);
            fRepeatTimer->release();
            fRepeatTimer = NULL;
        }
    }
}

#pragma mark - AppleM68Buttons::Debounce Handling

void AppleM68Buttons::startDebounce(int buttonIndex)
{
    if (buttonIndex < 0 || buttonIndex >= fButtonCount) {
        return;
    }
    
    button_state_t* state = &fButtonStates[buttonIndex];
    
    state->state = kButtonStateDebouncing;
    state->last_change_time = getCurrentTime();
    
    /* Start debounce timer */
    uint32_t debounceTime = fButtonConfigs[buttonIndex].debounce_time;
    if (debounceTime == 0) {
        debounceTime = fDebounceTime;
    }
    
    fDebounceTimer->setTimeoutMS(debounceTime);
}

void AppleM68Buttons::debounceComplete(void)
{
    uint64_t now = getCurrentTime();
    
    lck_mtx_lock(fStateLock);
    
    /* Check all debouncing buttons */
    for (int i = 0; i < fButtonCount; i++) {
        if (fButtonStates[i].state == kButtonStateDebouncing) {
            uint64_t elapsed = now - fButtonStates[i].last_change_time;
            
            if (elapsed >= (uint64_t)fDebounceTime * 1000000) {
                /* Debounce complete, process final state */
                bool pressed = (fButtonStates[i].physical_state != 0);
                
                if (fButtonConfigs[i].invert) {
                    pressed = !pressed;
                }
                
                fButtonStates[i].debounced_state = pressed ? 1 : 0;
                
                if (pressed) {
                    /* Button pressed */
                    fButtonStates[i].state = kButtonStateDown;
                    fButtonStates[i].down_time = now;
                    
                    /* Queue down event */
                    queueEvent(fButtonConfigs[i].button_id, kButtonEventDown, 0);
                    
                    /* Start long press timer if enabled */
                    if (fButtonConfigs[i].long_press_enabled) {
                        startLongPressTimer(i);
                    }
                } else {
                    /* Button released */
                    fButtonStates[i].state = kButtonStateIdle;
                    
                    /* Queue up event */
                    queueEvent(fButtonConfigs[i].button_id, kButtonEventUp, 0);
                    
                    /* Cancel timers */
                    cancelLongPressTimer();
                    cancelRepeatTimer();
                    
                    /* Handle tap counting */
                    if (fButtonConfigs[i].multi_tap_enabled && fTapTrackingEnabled) {
                        fButtonStates[i].tap_count++;
                        fButtonStates[i].state = kButtonStateWaitingForMulti;
                        
                        /* Start multi-tap timer */
                        fDebounceTimer->setTimeoutMS(fMultiTapInterval);
                    } else {
                        /* Single click */
                        queueEvent(fButtonConfigs[i].button_id, kButtonEventClick, 1);
                    }
                }
            }
        }
    }
    
    lck_mtx_unlock(fStateLock);
}

void AppleM68Buttons::debounceHandler(OSObject* owner, IOTimerEventSource* sender)
{
    AppleM68Buttons* me = (AppleM68Buttons*)owner;
    if (me) {
        me->debounceComplete();
    }
}

#pragma mark - AppleM68Buttons::Long Press Handling

void AppleM68Buttons::startLongPressTimer(int buttonIndex)
{
    if (buttonIndex < 0 || buttonIndex >= fButtonCount) {
        return;
    }
    
    uint32_t longPressTime = fButtonConfigs[buttonIndex].long_press_time;
    if (longPressTime == 0) {
        longPressTime = fLongPressTime;
    }
    
    fLongPressTimer->setTimeoutMS(longPressTime);
}

void AppleM68Buttons::cancelLongPressTimer(void)
{
    fLongPressTimer->cancelTimeout();
}

void AppleM68Buttons::longPressComplete(void)
{
    lck_mtx_lock(fStateLock);
    
    uint64_t now = getCurrentTime();
    
    for (int i = 0; i < fButtonCount; i++) {
        if (fButtonStates[i].state == kButtonStateDown) {
            uint64_t pressDuration = now - fButtonStates[i].down_time;
            
            if (pressDuration >= (uint64_t)fVeryLongPressTime * 1000000) {
                /* Very long press */
                fButtonStates[i].state = kButtonStateVeryLongPress;
                queueEvent(fButtonConfigs[i].button_id, kButtonEventVeryLongPress, 0);
                fLongPresses++;
            } else if (pressDuration >= (uint64_t)fLongPressTime * 1000000) {
                /* Long press */
                fButtonStates[i].state = kButtonStateLongPress;
                queueEvent(fButtonConfigs[i].button_id, kButtonEventLongPress, 0);
                fLongPresses++;
                
                /* Start repeat timer if enabled */
                if (fButtonConfigs[i].repeat_enabled) {
                    startRepeatTimer(i);
                }
            }
        }
    }
    
    lck_mtx_unlock(fStateLock);
}

void AppleM68Buttons::longPressHandler(OSObject* owner, IOTimerEventSource* sender)
{
    AppleM68Buttons* me = (AppleM68Buttons*)owner;
    if (me) {
        me->longPressComplete();
    }
}

#pragma mark - AppleM68Buttons::Repeat Handling

void AppleM68Buttons::startRepeatTimer(int buttonIndex)
{
    if (buttonIndex < 0 || buttonIndex >= fButtonCount) {
        return;
    }
    
    uint32_t repeatInterval = fButtonConfigs[buttonIndex].repeat_interval;
    if (repeatInterval == 0) {
        repeatInterval = 100;  /* Default 100ms */
    }
    
    fRepeatTimer->setTimeoutMS(repeatInterval);
}

void AppleM68Buttons::cancelRepeatTimer(void)
{
    fRepeatTimer->cancelTimeout();
}

void AppleM68Buttons::repeatHandler(void)
{
    lck_mtx_lock(fStateLock);
    
    for (int i = 0; i < fButtonCount; i++) {
        if (fButtonStates[i].state == kButtonStateLongPress ||
            fButtonStates[i].state == kButtonStateVeryLongPress) {
            
            fButtonStates[i].repeat_count++;
            
            /* Queue repeat event */
            queueEvent(fButtonConfigs[i].button_id, kButtonEventHold,
                      fButtonStates[i].repeat_count);
            
            /* Restart repeat timer */
            startRepeatTimer(i);
        }
    }
    
    lck_mtx_unlock(fStateLock);
}

void AppleM68Buttons::repeatHandler(OSObject* owner, IOTimerEventSource* sender)
{
    AppleM68Buttons* me = (AppleM68Buttons*)owner;
    if (me) {
        me->repeatHandler();
    }
}

#pragma mark - AppleM68Buttons::Interrupt Handling

void AppleM68Buttons::interruptHandler(void)
{
    uint16_t intStatus;
    uint16_t buttonState;
    int16_t wheelDelta;
    uint16_t switchState;
    
    fTotalInterrupts++;
    
    lck_mtx_lock(fStateLock);
    
    /* Read interrupt status */
    intStatus = readReg(M68_BTN_REG_INT_STATUS);
    
    /* Clear interrupts */
    writeReg(M68_BTN_REG_INT_CLEAR, intStatus);
    
    /* Process button interrupts */
    if (intStatus & M68_BTN_INT_BUTTON) {
        buttonState = readButtonState();
        processButtons(buttonState);
    }
    
    /* Process wheel interrupts */
    if (intStatus & M68_BTN_INT_WHEEL) {
        wheelDelta = readWheelDelta();
        if (wheelDelta != 0) {
            processClickWheel(wheelDelta);
        }
    }
    
    /* Process switch interrupts */
    if (intStatus & M68_BTN_INT_SWITCH) {
        switchState = readSwitchState();
        processSwitches(switchState);
    }
    
    /* Process error interrupts */
    if (intStatus & M68_BTN_INT_ERROR) {
        fErrors++;
        /* Handle error condition */
    }
    
    lck_mtx_unlock(fStateLock);
}

void AppleM68Buttons::interruptHandler(OSObject* owner, IOInterruptEventSource* src, int count)
{
    AppleM68Buttons* me = (AppleM68Buttons*)owner;
    if (me) {
        me->interruptHandler();
    }
}

#pragma mark - AppleM68Buttons::Button Processing

void AppleM68Buttons::processButtons(uint16_t buttonState)
{
    uint16_t changed = buttonState ^ readReg(M68_BTN_REG_BUTTON_EDGE);
    
    for (int i = 0; i < fButtonCount; i++) {
        if (fButtonConfigs[i].hardware_mask & changed) {
            bool pressed = (buttonState & fButtonConfigs[i].hardware_mask) != 0;
            processButton(i, pressed);
        }
    }
    
    /* Update edge detect register */
    writeReg(M68_BTN_REG_BUTTON_EDGE, buttonState);
}

void AppleM68Buttons::processButton(int buttonIndex, bool pressed)
{
    if (buttonIndex < 0 || buttonIndex >= fButtonCount) {
        return;
    }
    
    button_state_t* state = &fButtonStates[buttonIndex];
    
    /* Store physical state */
    state->physical_state = pressed ? 1 : 0;
    
    /* Start debounce */
    startDebounce(buttonIndex);
}

int AppleM68Buttons::findButtonByMask(uint16_t mask)
{
    for (int i = 0; i < fButtonCount; i++) {
        if (fButtonConfigs[i].hardware_mask == mask) {
            return i;
        }
    }
    return -1;
}

int AppleM68Buttons::findButtonById(uint8_t buttonId)
{
    for (int i = 0; i < fButtonCount; i++) {
        if (fButtonConfigs[i].button_id == buttonId) {
            return i;
        }
    }
    return -1;
}

#pragma mark - AppleM68Buttons::Click Wheel Processing

void AppleM68Buttons::processClickWheel(int16_t delta)
{
    uint64_t now = getCurrentTime();
    uint64_t elapsed = now - fLastWheelTime;
    
    fWheelDelta = delta;
    fWheelPosition += delta;
    
    /* Calculate velocity */
    if (elapsed > 0) {
        fWheelVelocity = (delta * 1000000) / (int32_t)elapsed;
    }
    
    /* Apply sensitivity */
    int32_t adjustedDelta = delta;
    switch (fWheelSensitivity) {
        case CLICK_WHEEL_SENSITIVITY_LOW:
            adjustedDelta = (delta > 0) ? 1 : (delta < 0) ? -1 : 0;
            break;
        case CLICK_WHEEL_SENSITIVITY_HIGH:
            adjustedDelta = delta * 2;
            break;
        case CLICK_WHEEL_SENSITIVITY_MAX:
            adjustedDelta = delta * 3;
            break;
        default:
            /* Medium - unchanged */
            break;
    }
    
    /* Determine direction */
    if (adjustedDelta > 0) {
        fWheelDirection = kWheelDirectionClockwise;
    } else if (adjustedDelta < 0) {
        fWheelDirection = kWheelDirectionCounterClockwise;
    } else {
        fWheelDirection = kWheelDirectionNone;
    }
    
    /* Queue wheel event */
    if (abs(adjustedDelta) > 0) {
        queueEvent(kButtonTypeClickWheel, kButtonEventRotate, abs(adjustedDelta));
        
        if (abs(fWheelVelocity) > MAX_CLICK_WHEEL_VELOCITY / 2) {
            queueEvent(kButtonTypeClickWheel, kButtonEventRotateAccelerated, abs(adjustedDelta));
        } else if (abs(fWheelVelocity) < MAX_CLICK_WHEEL_VELOCITY / 4) {
            queueEvent(kButtonTypeClickWheel, kButtonEventRotateDecelerated, abs(adjustedDelta));
        }
        
        fWheelEvents++;
    }
    
    fLastWheelTime = now;
}

#pragma mark - AppleM68Buttons::Switch Processing

void AppleM68Buttons::processSwitches(uint16_t switchState)
{
    uint8_t newHoldState;
    uint8_t newRingState;
    
    /* Process hold switch */
    if (switchState & M68_BTN_MASK_HOLD) {
        newHoldState = kSwitchStateOn;
    } else {
        newHoldState = kSwitchStateOff;
    }
    
    /* Apply inversion if needed */
    int holdIndex = findButtonByMask(M68_BTN_MASK_HOLD);
    if (holdIndex >= 0 && fButtonConfigs[holdIndex].invert) {
        newHoldState = (newHoldState == kSwitchStateOn) ? kSwitchStateOff : kSwitchStateOn;
    }
    
    if (newHoldState != fHoldSwitchState) {
        fHoldSwitchState = newHoldState;
        queueEvent(kButtonTypeHoldSwitch, fHoldSwitchState, 0);
    }
    
    /* Process ring/silent switch */
    if (switchState & M68_BTN_MASK_RING_SILENT) {
        newRingState = kSwitchStateOn;  /* Ring mode */
    } else {
        newRingState = kSwitchStateOff; /* Silent mode */
    }
    
    /* Apply inversion if needed */
    int ringIndex = findButtonByMask(M68_BTN_MASK_RING_SILENT);
    if (ringIndex >= 0 && fButtonConfigs[ringIndex].invert) {
        newRingState = (newRingState == kSwitchStateOn) ? kSwitchStateOff : kSwitchStateOn;
    }
    
    if (newRingState != fRingSilentState) {
        fRingSilentState = newRingState;
        queueEvent(kButtonTypeRingSilentSwitch, fRingSilentState, 0);
    }
}

#pragma mark - AppleM68Buttons::Event Management

void AppleM68Buttons::queueEvent(uint8_t buttonId, uint8_t eventType, uint32_t value)
{
    uint32_t nextTail;
    
    lck_mtx_lock(fEventLock);
    
    nextTail = (fEventQueueTail + 1) % 32;
    
    if (nextTail != fEventQueueHead) {
        fEventQueue[fEventQueueTail].button_id = buttonId;
        fEventQueue[fEventQueueTail].event_type = eventType;
        fEventQueue[fEventQueueTail].value = value;
        fEventQueue[fEventQueueTail].timestamp = getCurrentTime();
        
        fEventQueueTail = nextTail;
        fTotalEvents++;
    } else {
        /* Queue full, drop event */
        fErrors++;
    }
    
    lck_mtx_unlock(fEventLock);
    
    /* Dispatch event immediately */
    dispatchEvent(buttonId, eventType, value);
}

bool AppleM68Buttons::dequeueEvent(uint8_t* buttonId, uint8_t* eventType,
                                    uint32_t* value, uint64_t* timestamp)
{
    bool result = false;
    
    lck_mtx_lock(fEventLock);
    
    if (fEventQueueHead != fEventQueueTail) {
        *buttonId = fEventQueue[fEventQueueHead].button_id;
        *eventType = fEventQueue[fEventQueueHead].event_type;
        *value = fEventQueue[fEventQueueHead].value;
        *timestamp = fEventQueue[fEventQueueHead].timestamp;
        
        fEventQueueHead = (fEventQueueHead + 1) % 32;
        result = true;
    }
    
    lck_mtx_unlock(fEventLock);
    
    return result;
}

void AppleM68Buttons::dispatchEvent(uint8_t buttonId, uint8_t eventType, uint32_t value)
{
    /* Dispatch to HID system */
    if (eventType == kButtonEventRotate || eventType == kButtonEventRotateAccelerated ||
        eventType == kButtonEventRotateDecelerated) {
        /* Wheel event */
        AbsoluteTime ts;
        clock_get_uptime(&ts);
        
        dispatchWheelEvent(value, fWheelDirection, &ts);
    } else {
        /* Button event */
        uint32_t hidUsage = 0;
        
        /* Map button ID to HID usage */
        switch (buttonId) {
            case kButtonTypePower:
                hidUsage = 0x81;  /* System Power */
                break;
            case kButtonTypeVolumeUp:
                hidUsage = 0xE9;  /* Volume Increment */
                break;
            case kButtonTypeVolumeDown:
                hidUsage = 0xEA;  /* Volume Decrement */
                break;
            case kButtonTypeHome:
                hidUsage = 0x40;  /* Menu */
                break;
            case kButtonTypePlayPause:
                hidUsage = 0xCD;  /* Play/Pause */
                break;
            case kButtonTypeMenu:
                hidUsage = 0x40;  /* Menu */
                break;
            case kButtonTypeCenter:
                hidUsage = 0x41;  /* Select */
                break;
        }
        
        if (hidUsage != 0) {
            AbsoluteTime ts;
            clock_get_uptime(&ts);
            
            if (eventType == kButtonEventDown) {
                dispatchKeyboardEvent(hidUsage, true, &ts);
            } else if (eventType == kButtonEventUp) {
                dispatchKeyboardEvent(hidUsage, false, &ts);
            }
        }
    }
    
    /* Also post as a system event for non-HID consumers */
    if (eventType == kButtonEventClick || eventType == kButtonEventDoubleClick ||
        eventType == kButtonEventTripleClick) {
        /* Post as NX_System event */
        /* ... */
    }
}

#pragma mark - AppleM68Buttons::Utility

uint64_t AppleM68Buttons::getCurrentTime(void)
{
    uint64_t time;
    clock_get_uptime(&time);
    return time;
}

#pragma mark - AppleM68Buttons::IOService Overrides

bool AppleM68Buttons::start(IOService* provider)
{
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("AppleM68Buttons: Starting M68K Buttons Controller\n");
    
    /* Map device registers */
    if (!mapRegisters()) {
        IOLog("AppleM68Buttons: Failed to map registers\n");
        return false;
    }
    
    /* Read hardware version */
    readHardwareVersion();
    
    /* Reset controller */
    if (!resetController()) {
        IOLog("AppleM68Buttons: Failed to reset controller\n");
        unmapRegisters();
        return false;
    }
    
    /* Load platform configuration */
    loadPlatformConfig();
    
    /* Configure hardware */
    configureHardware();
    
    /* Create work loop */
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        IOLog("AppleM68Buttons: Failed to create work loop\n");
        unmapRegisters();
        return false;
    }
    
    /* Create command gate */
    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        IOLog("AppleM68Buttons: Failed to create command gate\n");
        fWorkLoop->release();
        fWorkLoop = NULL;
        unmapRegisters();
        return false;
    }
    
    fWorkLoop->addEventSource(fCommandGate);
    
    /* Create timers */
    if (!createTimers()) {
        IOLog("AppleM68Buttons: Failed to create timers\n");
        destroyWorkLoop();
        unmapRegisters();
        return false;
    }
    
    /* Create interrupt source */
    fInterruptSource = IOInterruptEventSource::interruptEventSource(this,
        OSMemberFunctionCast(IOInterruptEventAction, this,
                             &AppleM68Buttons::interruptHandler),
        provider, 0);
    
    if (!fInterruptSource) {
        IOLog("AppleM68Buttons: Failed to create interrupt source\n");
        destroyTimers();
        destroyWorkLoop();
        unmapRegisters();
        return false;
    }
    
    fWorkLoop->addEventSource(fInterruptSource);
    
    /* Initialize button states */
    for (int i = 0; i < fButtonCount; i++) {
        fButtonStates[i].button_id = fButtonConfigs[i].button_id;
        fButtonStates[i].state = kButtonStateIdle;
        fButtonStates[i].physical_state = 0;
        fButtonStates[i].debounced_state = 0;
        fButtonStates[i].last_change_time = 0;
        fButtonStates[i].down_time = 0;
        fButtonStates[i].tap_count = 0;
        fButtonStates[i].pending_events = 0;
    }
    
    /* Read initial switch states */
    uint16_t switchState = readSwitchState();
    processSwitches(switchState);
    
    /* Publish properties */
    setProperty("Controller ID", fControllerID, 16);
    setProperty("Controller Version", fControllerVersion, 16);
    setProperty("Button Count", fButtonCount, 32);
    setProperty("Debounce Time", fDebounceTime, 32);
    setProperty("Multi-tap Interval", fMultiTapInterval, 32);
    setProperty("Long Press Time", fLongPressTime, 32);
    setProperty("Wheel Sensitivity", fWheelSensitivity, 8);
    setProperty("Hold Switch", (fHoldSwitchState == kSwitchStateOn) ? "On" : "Off");
    setProperty("Ring/Silent", (fRingSilentState == kSwitchStateOn) ? "Ring" : "Silent");
    
    /* Register as HID device */
    registerService();
    
    IOLog("AppleM68Buttons: Started successfully with %d buttons\n", fButtonCount);
    
    return true;
}

void AppleM68Buttons::stop(IOService* provider)
{
    IOLog("AppleM68Buttons: Stopping\n");
    
    /* Disable interrupts */
    if (fRegisters) {
        writeReg(M68_BTN_REG_INT_ENABLE, 0);
    }
    
    /* Remove interrupt source */
    if (fWorkLoop && fInterruptSource) {
        fWorkLoop->removeEventSource(fInterruptSource);
        fInterruptSource->release();
        fInterruptSource = NULL;
    }
    
    /* Destroy timers */
    destroyTimers();
    
    /* Destroy work loop */
    if (fWorkLoop) {
        if (fCommandGate) {
            fWorkLoop->removeEventSource(fCommandGate);
            fCommandGate->release();
            fCommandGate = NULL;
        }
        
        fWorkLoop->release();
        fWorkLoop = NULL;
    }
    
    /* Unmap registers */
    unmapRegisters();
    
    super::stop(provider);
}

#pragma mark - AppleM68Buttons::IOHIDevice Overrides

IOReturn AppleM68Buttons::setProperties(OSObject* properties)
{
    OSDictionary* dict = OSDynamicCast(OSDictionary, properties);
    if (!dict) {
        return kIOReturnBadArgument;
    }
    
    OSNumber* num = NULL;
    
    /* Set debounce time */
    num = OSDynamicCast(OSNumber, dict->getObject("DebounceTime"));
    if (num) {
        fDebounceTime = num->unsigned32BitValue();
        if (fRegisters) {
            writeReg(M68_BTN_REG_DEBOUNCE, fDebounceTime);
        }
    }
    
    /* Set multi-tap interval */
    num = OSDynamicCast(OSNumber, dict->getObject("MultiTapInterval"));
    if (num) {
        fMultiTapInterval = num->unsigned32BitValue();
    }
    
    /* Set long press time */
    num = OSDynamicCast(OSNumber, dict->getObject("LongPressTime"));
    if (num) {
        fLongPressTime = num->unsigned32BitValue();
    }
    
    /* Set wheel sensitivity */
    num = OSDynamicCast(OSNumber, dict->getObject("WheelSensitivity"));
    if (num) {
        fWheelSensitivity = num->unsigned8BitValue();
    }
    
    /* Enable/disable tap tracking */
    OSBoolean* boolVal = OSDynamicCast(OSBoolean, dict->getObject("TapTracking"));
    if (boolVal) {
        fTapTrackingEnabled = boolVal->isTrue();
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleM68Buttons::handleOpen(IOService* client, IOOptionBits options, void* arg)
{
    /* Allow multiple clients (e.g., HID system and diagnostics) */
    return kIOReturnSuccess;
}

IOReturn AppleM68Buttons::message(UInt32 type, IOService* provider, void* argument)
{
    /* Handle system messages */
    switch (type) {
        case kIOMessageSystemWillSleep:
            IOLog("AppleM68Buttons: System will sleep\n");
            /* Prepare for sleep */
            break;
            
        case kIOMessageSystemHasPoweredOn:
            IOLog("AppleM68Buttons: System woke from sleep\n");
            /* Re-initialize hardware */
            resetController();
            break;
    }
    
    return kIOReturnSuccess;
}

#pragma mark - AppleM68Buttons::Power Management

IOReturn AppleM68Buttons::setPowerState(unsigned long powerState, IOService* device)
{
    if (powerState == 0) {
        /* Going to sleep */
        IOLog("AppleM68Buttons: Entering low power state\n");
        
        /* Disable interrupts */
        if (fRegisters) {
            writeReg(M68_BTN_REG_INT_ENABLE, 0);
            writeReg(M68_BTN_REG_CONTROL, M68_BTN_CTRL_SLEEP);
        }
        
    } else {
        /* Waking up */
        IOLog("AppleM68Buttons: Waking from low power state\n");
        
        /* Re-enable hardware */
        if (fRegisters) {
            writeReg(M68_BTN_REG_CONTROL, M68_BTN_CTRL_WAKE);
            writeReg(M68_BTN_REG_INT_ENABLE,
                    M68_BTN_INT_BUTTON | M68_BTN_INT_WHEEL | M68_BTN_INT_SWITCH);
        }
        
        /* Read initial states */
        uint16_t switchState = readSwitchState();
        processSwitches(switchState);
    }
    
    fPowerState = powerState;
    
    return IOPMAckImplied;
}

#pragma mark - AppleM68Buttons::User Client Access

IOReturn AppleM68Buttons::newUserClient(task_t owningTask,
                                         void* securityID,
                                         UInt32 type,
                                         OSDictionary* properties,
                                         IOUserClient** handler)
{
    /* Would create diagnostic/configuration client */
    return kIOReturnUnsupported;
}

#pragma mark - AppleM68Buttons::Public API

IOReturn AppleM68Buttons::setDebounceTime(uint32_t ms)
{
    if (ms < 1 || ms > 100) {
        return kIOReturnBadArgument;
    }
    
    fDebounceTime = ms;
    
    if (fRegisters) {
        writeReg(M68_BTN_REG_DEBOUNCE, ms);
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleM68Buttons::setMultiTapInterval(uint32_t ms)
{
    if (ms < 100 || ms > 1000) {
        return kIOReturnBadArgument;
    }
    
    fMultiTapInterval = ms;
    
    return kIOReturnSuccess;
}

IOReturn AppleM68Buttons::setLongPressTime(uint32_t ms)
{
    if (ms < 500 || ms > 10000) {
        return kIOReturnBadArgument;
    }
    
    fLongPressTime = ms;
    
    return kIOReturnSuccess;
}

IOReturn AppleM68Buttons::setWheelSensitivity(uint8_t sensitivity)
{
    if (sensitivity > CLICK_WHEEL_SENSITIVITY_MAX) {
        return kIOReturnBadArgument;
    }
    
    fWheelSensitivity = sensitivity;
    
    return kIOReturnSuccess;
}
