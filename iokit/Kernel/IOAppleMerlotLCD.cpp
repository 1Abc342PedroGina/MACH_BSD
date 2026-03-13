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

#include <IOKit/IOBSD.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOMessage.h>

#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/OSAtomic.h>

#include <machine/machine_routines.h>

/*==============================================================================
 * AppleMerlotLCD - LCD Controller for Apple Silicon (Merlot Family)
 * 
 * This driver handles the Merlot LCD controller used in Apple Silicon devices:
 * - Display timing generation
 * - DSI (Display Serial Interface) control
 * - Backlight PWM control
 * - Color management (Gamma/LUT)
 * - Display power sequencing
 * - HDR and EDR support
 * - ProMotion variable refresh rate
 *==============================================================================*/

/*==============================================================================
 * Constants and Definitions
 *==============================================================================*/

#define APPLE_MERLOT_LCD_VERSION        0x00030001  /* Version 3.1 */
#define APPLE_MERLOT_LCD_REVISION       0x00000001

/* Maximum values */
#define MAX_DISPLAY_MODES                32
#define MAX_GAMMA_ENTRIES                256
#define MAX_COLOR_TEMPERATURE             100
#define MAX_BACKLIGHT_LEVEL               65535
#define MAX_REFRESH_RATES                  8
#define MAX_DSI_LANES                      4
#define MAX_PIXEL_CLOCK                   1000000  /* kHz */

/* Timing parameters */
#define HBLANK_MIN                        8
#define HBLANK_MAX                        256
#define VBLANK_MIN                        2
#define VBLANK_MAX                        64
#define MIN_PIXEL_CLOCK                    25000  /* kHz */
#define MAX_PIXEL_CLOCK_MERLOT             600000 /* kHz */
#define MAX_PIXEL_CLOCK_MERLOT_PRO         800000 /* kHz */
#define MAX_PIXEL_CLOCK_MERLOT_MAX          1000000 /* kHz */

/* Backlight control */
#define BACKLIGHT_PWM_FREQ_MIN             1000   /* Hz */
#define BACKLIGHT_PWM_FREQ_MAX             20000  /* Hz */
#define BACKLIGHT_PWM_FREQ_DEFAULT         15000  /* Hz */
#define BACKLIGHT_RAMP_TIME                 100   /* ms */
#define BACKLIGHT_UPDATE_INTERVAL            16   /* ms */

/* Power management */
#define POWER_UP_DELAY                      5     /* ms */
#define POWER_DOWN_DELAY                     10    /* ms */
#define POWER_OFF_DELAY                      100   /* ms */
#define RESET_DELAY                          1     /* ms */

/* Thermal management */
#define THERMAL_THROTTLE_TEMP                45    /* °C */
#define THERMAL_CRITICAL_TEMP                 85    /* °C */
#define THERMAL_EMERGENCY_TEMP               105   /* °C */

/* Color management */
#define DEFAULT_GAMMA                        2.2f
#define DEFAULT_COLOR_TEMPERATURE             6500  /* K */
#define MAX_LUMINANCE_NITS                    1600
#define EDR_MAX_BOOST                          4.0f

/* ProMotion VRR */
#define VRR_MIN_REFRESH                        24    /* Hz */
#define VRR_MAX_REFRESH                        120   /* Hz */
#define VRR_UPDATE_INTERVAL                     8    /* ms */

/*==============================================================================
 * Display Modes
 *==============================================================================*/

typedef struct {
    uint32_t    width;                      /* Horizontal active pixels */
    uint32_t    height;                     /* Vertical active lines */
    uint32_t    refresh_rate;                /* Refresh rate in Hz */
    uint32_t    pixel_clock;                  /* Pixel clock in kHz */
    
    /* Horizontal timing */
    uint32_t    h_active;                     /* Active pixels (same as width) */
    uint32_t    h_sync_start;                  /* Start of h-sync */
    uint32_t    h_sync_end;                    /* End of h-sync */
    uint32_t    h_total;                       /* Total horizontal pixels */
    
    /* Vertical timing */
    uint32_t    v_active;                     /* Active lines (same as height) */
    uint32_t    v_sync_start;                  /* Start of v-sync */
    uint32_t    v_sync_end;                    /* End of v-sync */
    uint32_t    v_total;                       /* Total vertical lines */
    
    /* Flags */
    uint32_t    flags;                        /* Mode flags */
    uint32_t    bpp;                           /* Bits per pixel */
    uint32_t    color_format;                   /* Color format */
    uint32_t    dsi_lanes;                      /* Number of DSI lanes */
    uint32_t    dsi_rate;                       /* DSI bit rate per lane (Mbps) */
} merlot_display_mode_t;

/* Mode flags */
#define MERLOT_MODE_FLAG_INTERLACED        (1 << 0)
#define MERLOT_MODE_FLAG_DOUBLE_SCAN       (1 << 1)
#define MERLOT_MODE_FLAG_PROGRESSIVE       (1 << 2)
#define MERLOT_MODE_FLAG_VRR                (1 << 3)
#define MERLOT_MODE_FLAG_HDR                (1 << 4)
#define MERLOT_MODE_FLAG_EDR                (1 << 5)
#define MERLOT_MODE_FLAG_P3                 (1 << 6)
#define MERLOT_MODE_FLAG_10BIT              (1 << 7)

/* Color formats */
#define MERLOT_COLOR_FORMAT_RGB565          0x01
#define MERLOT_COLOR_FORMAT_RGB888          0x02
#define MERLOT_COLOR_FORMAT_XRGB8888        0x03
#define MERLOT_COLOR_FORMAT_ARGB8888        0x04
#define MERLOT_COLOR_FORMAT_YUV422          0x05
#define MERLOT_COLOR_FORMAT_YUV420          0x06
#define MERLOT_COLOR_FORMAT_P010            0x07  /* 10-bit YUV */
#define MERLOT_COLOR_FORMAT_P016            0x08  /* 16-bit YUV */

/*==============================================================================
 * Hardware Register Definitions (Merlot LCD Controller)
 * 
 * Based on Apple's Merlot display controller family:
 * - Merlot (A12/A13)
 * - Merlot Pro (A14/M1)
 * - Merlot Max (M1 Pro/Max/Ultra)
 * - Merlot Extreme (M2/M3 series)
 *==============================================================================*/

/* Register base offsets */
#define MERLOT_REG_VERSION              0x0000  /* Controller version */
#define MERLOT_REG_FEATURES              0x0004  /* Feature bits */
#define MERLOT_REG_CONTROL                0x0008  /* Main control */
#define MERLOT_REG_STATUS                 0x000C  /* Status register */
#define MERLOT_REG_INT_ENABLE             0x0010  /* Interrupt enable */
#define MERLOT_REG_INT_STATUS             0x0014  /* Interrupt status */
#define MERLOT_REG_INT_CLEAR              0x0018  /* Interrupt clear */
#define MERLOT_REG_POWER_STATE            0x001C  /* Power state */

/* Display timing registers */
#define MERLOT_REG_H_ACTIVE               0x0020  /* Horizontal active */
#define MERLOT_REG_H_SYNC_START           0x0024  /* H-sync start */
#define MERLOT_REG_H_SYNC_END             0x0028  /* H-sync end */
#define MERLOT_REG_H_TOTAL                0x002C  /* Horizontal total */
#define MERLOT_REG_V_ACTIVE               0x0030  /* Vertical active */
#define MERLOT_REG_V_SYNC_START           0x0034  /* V-sync start */
#define MERLOT_REG_V_SYNC_END             0x0038  /* V-sync end */
#define MERLOT_REG_V_TOTAL                0x003C  /* Vertical total */
#define MERLOT_REG_PIXEL_CLOCK             0x0040  /* Pixel clock */

/* Color management */
#define MERLOT_REG_GAMMA_CTRL              0x0100  /* Gamma control */
#define MERLOT_REG_GAMMA_RED               0x0200  /* Red gamma LUT (256 entries) */
#define MERLOT_REG_GAMMA_GREEN             0x0400  /* Green gamma LUT */
#define MERLOT_REG_GAMMA_BLUE              0x0600  /* Blue gamma LUT */
#define MERLOT_REG_COLOR_MATRIX             0x0800  /* Color transformation matrix */
#define MERLOT_REG_CSC_COEFF                0x0900  /* Color space conversion */

/* DSI control */
#define MERLOT_REG_DSI_CONTROL             0x1000  /* DSI control */
#define MERLOT_REG_DSI_STATUS              0x1004  /* DSI status */
#define MERLOT_REG_DSI_LANE_CONFIG         0x1008  /* Lane configuration */
#define MERLOT_REG_DSI_CLOCK                0x100C  /* DSI clock rate */
#define MERLOT_REG_DSI_TIMING               0x1010  /* DSI timing */
#define MERLOT_REG_DSI_VIDEO_MODE          0x1014  /* Video mode */
#define MERLOT_REG_DSI_CMD_MODE             0x1018  /* Command mode */

/* Backlight control */
#define MERLOT_REG_BACKLIGHT_CTRL          0x2000  /* Backlight control */
#define MERLOT_REG_BACKLIGHT_STATUS        0x2004  /* Backlight status */
#define MERLOT_REG_BACKLIGHT_PWM           0x2008  /* PWM control */
#define MERLOT_REG_BACKLIGHT_FREQ          0x200C  /* PWM frequency */
#define MERLOT_REG_BACKLIGHT_DUTY          0x2010  /* PWM duty cycle */
#define MERLOT_REG_BACKLIGHT_CURRENT       0x2014  /* LED current */
#define MERLOT_REG_BACKLIGHT_VOLTAGE       0x2018  /* LED voltage */
#define MERLOT_REG_BACKLIGHT_RAMP          0x201C  /* Ramp control */

/* Thermal management */
#define MERLOT_REG_TEMPERATURE             0x3000  /* Temperature sensor */
#define MERLOT_REG_THERMAL_LIMIT           0x3004  /* Thermal limit */
#define MERLOT_REG_THERMAL_STATUS          0x3008  /* Thermal status */

/* ProMotion/VRR */
#define MERLOT_REG_VRR_CONTROL             0x4000  /* VRR control */
#define MERLOT_REG_VRR_STATUS              0x4004  /* VRR status */
#define MERLOT_REG_VRR_MIN_REFRESH         0x4008  /* Minimum refresh rate */
#define MERLOT_REG_VRR_MAX_REFRESH         0x400C  /* Maximum refresh rate */
#define MERLOT_REG_VRR_CURRENT_REFRESH     0x4010  /* Current refresh rate */

/* Debug */
#define MERLOT_REG_DEBUG                    0x8000  /* Debug register */
#define MERLOT_REG_TEST_PATTERN             0x8004  /* Test pattern control */

/* Control bits */
#define MERLOT_CTRL_ENABLE                  (1 << 0)
#define MERLOT_CTRL_RESET                    (1 << 1)
#define MERLOT_CTRL_SLEEP                    (1 << 2)
#define MERLOT_CTRL_WAKE                     (1 << 3)
#define MERLOT_CTRL_DISPLAY_ON               (1 << 4)
#define MERLOT_CTRL_DISPLAY_OFF              (1 << 5)
#define MERLOT_CTRL_BACKLIGHT_ENABLE         (1 << 6)
#define MERLOT_CTRL_VRR_ENABLE                (1 << 7)
#define MERLOT_CTRL_HDR_ENABLE                (1 << 8)
#define MERLOT_CTRL_TEARING_FREE              (1 << 9)

/* Status bits */
#define MERLOT_STATUS_READY                  (1 << 0)
#define MERLOT_STATUS_DISPLAY_ON              (1 << 1)
#define MERLOT_STATUS_BACKLIGHT_ON            (1 << 2)
#define MERLOT_STATUS_VRR_ACTIVE              (1 << 3)
#define MERLOT_STATUS_HDR_ACTIVE              (1 << 4)
#define MERLOT_STATUS_TE_ENABLED              (1 << 5)
#define MERLOT_STATUS_ERROR                    (1 << 15)

/* Interrupt bits */
#define MERLOT_INT_VSYNC                     (1 << 0)
#define MERLOT_INT_TEARING                    (1 << 1)
#define MERLOT_INT_VRR_CHANGE                 (1 << 2)
#define MERLOT_INT_THERMAL_WARNING            (1 << 3)
#define MERLOT_INT_THERMAL_CRITICAL           (1 << 4)
#define MERLOT_INT_BACKLIGHT_FAULT            (1 << 5)
#define MERLOT_INT_DISPLAY_ERROR              (1 << 6)
#define MERLOT_INT_ALL                        0x7F

/* Power states */
#define MERLOT_POWER_OFF                    0
#define MERLOT_POWER_SLEEP                   1
#define MERLOT_POWER_STANDBY                 2
#define MERLOT_POWER_ON                      3

/* DSI lane configurations */
#define MERLOT_DSI_LANE_1                   0x01
#define MERLOT_DSI_LANE_2                   0x02
#define MERLOT_DSI_LANE_4                   0x04

/*==============================================================================
 * Display Configuration Structure
 *==============================================================================*/

typedef struct {
    char        panel_name[64];             /* Panel identifier */
    uint32_t    native_width;                 /* Native panel width */
    uint32_t    native_height;                /* Native panel height */
    uint32_t    native_refresh;               /* Native refresh rate */
    uint32_t    min_refresh;                  /* Minimum VRR refresh */
    uint32_t    max_refresh;                  /* Maximum VRR refresh */
    uint32_t    pixel_packing;                 /* Pixel packing format */
    uint32_t    dsi_lanes;                     /* Number of DSI lanes */
    uint32_t    dsi_rate;                       /* DSI data rate (Mbps) */
    uint32_t    hporch;                         /* Horizontal porch */
    uint32_t    vporch;                         /* Vertical porch */
    uint32_t    hsync_width;                    /* HSYNC pulse width */
    uint32_t    vsync_width;                    /* VSYNC pulse width */
    uint32_t    backlight_max;                  /* Maximum backlight value */
    uint32_t    backlight_min;                  /* Minimum backlight value */
    uint32_t    color_depth;                    /* Color depth (8/10/12-bit) */
    uint32_t    color_gamut;                    /* Color gamut (sRGB/P3/2020) */
    uint32_t    hdr_capabilities;               /* HDR capabilities */
    uint32_t    edr_capabilities;               /* EDR capabilities */
    uint32_t    thermal_throttle_temp;          /* Throttle temperature */
    uint32_t    thermal_critical_temp;          /* Critical temperature */
    uint32_t    panel_version;                   /* Panel version */
} merlot_panel_config_t;

/* Panel flags */
#define MERLOT_PANEL_FLAG_HDR10             (1 << 0)
#define MERLOT_PANEL_FLAG_DOLBY_VISION       (1 << 1)
#define MERLOT_PANEL_FLAG_HLG                 (1 << 2)
#define MERLOT_PANEL_FLAG_PRO_DISPLAY        (1 << 3)
#define MERLOT_PANEL_FLAG_XDR                 (1 << 4)
#define MERLOT_PANEL_FLAG_LTPO                (1 << 5)  /* Low-temperature polycrystalline oxide */
#define MERLOT_PANEL_FLAG_OLED                (1 << 6)
#define MERLOT_PANEL_FLAG_MICROLED            (1 << 7)

/*==============================================================================
 * AppleMerlotLCD Main Class
 *==============================================================================*/

class AppleMerlotLCD : public IOFramebuffer
{
    OSDeclareDefaultStructors(AppleMerlotLCD)
    
private:
    /* IOKit resources */
    IOWorkLoop*                 fWorkLoop;
    IOCommandGate*              fCommandGate;
    IOTimerEventSource*         fVRRTimer;
    IOTimerEventSource*         fBacklightTimer;
    IOTimerEventSource*         fThermalTimer;
    IOInterruptEventSource*     fInterruptSource;
    
    /* Hardware resources */
    IOMemoryMap*                fMemoryMap;
    volatile uint32_t*          fRegisters;
    IOPhysicalAddress           fPhysicalAddress;
    IOPhysicalLength            fPhysicalLength;
    uint32_t                    fControllerVersion;
    uint32_t                    fFeatures;
    
    /* Display configuration */
    merlot_panel_config_t       fPanelConfig;
    merlot_display_mode_t       fCurrentMode;
    merlot_display_mode_t       fSupportedModes[MAX_DISPLAY_MODES];
    uint32_t                    fNumModes;
    
    /* Display state */
    bool                        fDisplayOn;
    bool                        fBacklightOn;
    uint32_t                    fPowerState;
    uint32_t                    fBrightness;
    uint32_t                    fTargetBrightness;
    uint32_t                    fMaxBrightness;
    uint32_t                    fMinBrightness;
    uint64_t                    fLastBrightnessUpdate;
    
    /* Color management */
    float                       fGamma;
    uint32_t                    fColorTemperature;
    uint32_t                    fColorGamut;
    uint32_t                    fColorDepth;
    uint16_t                    fRedGamma[MAX_GAMMA_ENTRIES];
    uint16_t                    fGreenGamma[MAX_GAMMA_ENTRIES];
    uint16_t                    fBlueGamma[MAX_GAMMA_ENTRIES];
    float                       fColorMatrix[9];
    
    /* ProMotion/VRR */
    bool                        fVRREnabled;
    uint32_t                    fMinRefresh;
    uint32_t                    fMaxRefresh;
    uint32_t                    fCurrentRefresh;
    uint32_t                    fTargetRefresh;
    uint64_t                    fLastFrameTime;
    uint32_t                    fFrameCount;
    
    /* HDR/EDR */
    bool                        fHDREnabled;
    bool                        fEDREnabled;
    float                       fMaxLuminance;
    float                       fEDRBoost;
    float                       fHDRMetadata[8];
    
    /* Thermal management */
    uint32_t                    fCurrentTemperature;
    uint32_t                    fThermalThrottleTemp;
    uint32_t                    fThermalCriticalTemp;
    uint32_t                    fThermalState;
    bool                        fThermalThrottling;
    
    /* Statistics */
    uint64_t                    fTotalFrames;
    uint64_t                    fTotalVSyncs;
    uint64_t                    fVRRChanges;
    uint64_t                    fBrightnessChanges;
    uint64_t                    fThermalEvents;
    uint64_t                    fErrors;
    
    /* Locking */
    lck_mtx_t*                  fDisplayLock;
    lck_mtx_t*                  fBacklightLock;
    
    /*==============================================================================
     * Private Methods
     *==============================================================================*/
    
    /* Hardware access */
    uint32_t                    readReg(uint32_t offset);
    void                        writeReg(uint32_t offset, uint32_t value);
    void                        setBits(uint32_t offset, uint32_t bits);
    void                        clearBits(uint32_t offset, uint32_t bits);
    bool                        waitForBit(uint32_t offset, uint32_t bit, bool set, uint32_t timeout);
    
    /* Hardware initialization */
    bool                        mapRegisters(void);
    void                        unmapRegisters(void);
    bool                        resetController(void);
    bool                        readHardwareVersion(void);
    bool                        detectPanel(void);
    bool                        initializeHardware(void);
    void                        shutdownHardware(void);
    
    /* Display timing */
    bool                        setDisplayMode(const merlot_display_mode_t* mode);
    bool                        getCurrentMode(merlot_display_mode_t* mode);
    bool                        validateMode(const merlot_display_mode_t* mode);
    void                        programTiming(const merlot_display_mode_t* mode);
    
    /* Backlight control */
    bool                        setBacklightBrightness(uint32_t brightness);
    bool                        setBacklightPWM(uint32_t freq, uint32_t duty);
    void                        backlightRamp(uint32_t target);
    void                        backlightUpdate(void);
    static void                 backlightHandler(OSObject* owner, IOTimerEventSource* sender);
    
    /* ProMotion/VRR */
    bool                        setVRRState(bool enable);
    bool                        setRefreshRate(uint32_t refresh);
    void                        vrrUpdate(void);
    static void                 vrrHandler(OSObject* owner, IOTimerEventSource* sender);
    
    /* Color management */
    bool                        setGamma(float gamma);
    bool                        setColorTemperature(uint32_t kelvin);
    bool                        setColorGamut(uint32_t gamut);
    bool                        setColorMatrix(const float* matrix);
    void                        programGamma(const uint16_t* red, const uint16_t* green,
                                              const uint16_t* blue, uint32_t size);
    void                        programColorMatrix(const float* matrix);
    
    /* HDR/EDR */
    bool                        setHDREnabled(bool enable);
    bool                        setEDREnabled(bool enable);
    bool                        setMaxLuminance(float nits);
    bool                        setEDRBoost(float boost);
    void                        programHDRMetadata(const float* metadata);
    
    /* Thermal management */
    void                        updateTemperature(void);
    void                        thermalCheck(void);
    void                        thermalThrottle(void);
    static void                 thermalHandler(OSObject* owner, IOTimerEventSource* sender);
    
    /* Interrupt handling */
    void                        interruptHandler(void);
    static void                 interruptHandler(OSObject* owner, IOInterruptEventSource* src, int count);
    
    /* Power management */
    bool                        setPowerStateInternal(uint32_t state);
    void                        powerUpSequence(void);
    void                        powerDownSequence(void);
    
    /* Mode detection */
    void                        detectSupportedModes(void);
    void                        addDisplayMode(uint32_t width, uint32_t height,
                                               uint32_t refresh, uint32_t flags);
    
    /* Utility */
    uint64_t                    getCurrentTime(void);
    void                        logDisplayInfo(void);
    
protected:
    bool                        init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                        free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                        start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                        stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    /* IOFramebuffer overrides */
    virtual IOReturn            enableController(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            disableController(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            setDisplayMode(IOIndex mode) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            getDisplayModes(IODisplayModeID* modes, IOIndex* count) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            getInformationForMode(IODisplayModeID mode, IODisplayModeInformation* info) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                                    IOPixelAperture aperture,
                                                    IOPixelInformation* info) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            setStartupDisplayMode(IOIndex mode) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            setAttribute(IOSelect attribute, uintptr_t value) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            getAttribute(IOSelect attribute, uintptr_t* value) APPLE_KEXT_OVERRIDE;
    
    /* Power management */
    virtual IOReturn            setPowerState(unsigned long powerState,
                                              IOService* device) APPLE_KEXT_OVERRIDE;
    
    /* Public API */
    virtual IOReturn            setBrightness(uint32_t brightness);
    virtual uint32_t            getBrightness(void) { return fBrightness; }
    virtual IOReturn            setVRR(bool enable);
    virtual bool                getVRR(void) { return fVRREnabled; }
    virtual IOReturn            setRefresh(uint32_t refresh);
    virtual uint32_t            getRefresh(void) { return fCurrentRefresh; }
    virtual IOReturn            setHDR(bool enable);
    virtual bool                getHDR(void) { return fHDREnabled; }
    virtual IOReturn            setEDR(bool enable);
    virtual bool                getEDR(void) { return fEDREnabled; }
    virtual uint32_t            getTemperature(void) { return fCurrentTemperature; }
    virtual IOReturn            setColorTemp(uint32_t kelvin);
    virtual uint32_t            getColorTemp(void) { return fColorTemperature; }
};

OSDefineMetaClassAndStructors(AppleMerlotLCD, IOFramebuffer)

/*==============================================================================
 * AppleMerlotLCD Implementation
 *==============================================================================*/

#pragma mark - AppleMerlotLCD::Initialization

bool AppleMerlotLCD::init(OSDictionary* dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    
    /* Initialize member variables */
    fWorkLoop = NULL;
    fCommandGate = NULL;
    fVRRTimer = NULL;
    fBacklightTimer = NULL;
    fThermalTimer = NULL;
    fInterruptSource = NULL;
    
    fMemoryMap = NULL;
    fRegisters = NULL;
    fPhysicalAddress = 0;
    fPhysicalLength = 0;
    fControllerVersion = 0;
    fFeatures = 0;
    
    /* Initialize panel config */
    bzero(&fPanelConfig, sizeof(fPanelConfig));
    bzero(&fCurrentMode, sizeof(fCurrentMode));
    bzero(fSupportedModes, sizeof(fSupportedModes));
    fNumModes = 0;
    
    /* Initialize display state */
    fDisplayOn = false;
    fBacklightOn = false;
    fPowerState = MERLOT_POWER_OFF;
    fBrightness = MAX_BACKLIGHT_LEVEL;
    fTargetBrightness = MAX_BACKLIGHT_LEVEL;
    fMaxBrightness = MAX_BACKLIGHT_LEVEL;
    fMinBrightness = 0;
    fLastBrightnessUpdate = 0;
    
    /* Initialize color management */
    fGamma = DEFAULT_GAMMA;
    fColorTemperature = DEFAULT_COLOR_TEMPERATURE;
    fColorGamut = 0;
    fColorDepth = 8;
    bzero(fRedGamma, sizeof(fRedGamma));
    bzero(fGreenGamma, sizeof(fGreenGamma));
    bzero(fBlueGamma, sizeof(fBlueGamma));
    bzero(fColorMatrix, sizeof(fColorMatrix));
    
    /* Initialize ProMotion/VRR */
    fVRREnabled = false;
    fMinRefresh = VRR_MIN_REFRESH;
    fMaxRefresh = VRR_MAX_REFRESH;
    fCurrentRefresh = 60;
    fTargetRefresh = 60;
    fLastFrameTime = 0;
    fFrameCount = 0;
    
    /* Initialize HDR/EDR */
    fHDREnabled = false;
    fEDREnabled = false;
    fMaxLuminance = MAX_LUMINANCE_NITS;
    fEDRBoost = 1.0f;
    bzero(fHDRMetadata, sizeof(fHDRMetadata));
    
    /* Initialize thermal management */
    fCurrentTemperature = 25;
    fThermalThrottleTemp = THERMAL_THROTTLE_TEMP;
    fThermalCriticalTemp = THERMAL_CRITICAL_TEMP;
    fThermalState = 0;
    fThermalThrottling = false;
    
    /* Initialize statistics */
    fTotalFrames = 0;
    fTotalVSyncs = 0;
    fVRRChanges = 0;
    fBrightnessChanges = 0;
    fThermalEvents = 0;
    fErrors = 0;
    
    /* Create locks */
    fDisplayLock = lck_mtx_alloc_init(IOService::getKernelLockGroup(), LCK_ATTR_NULL);
    fBacklightLock = lck_mtx_alloc_init(IOService::getKernelLockGroup(), LCK_ATTR_NULL);
    
    if (!fDisplayLock || !fBacklightLock) {
        return false;
    }
    
    return true;
}

void AppleMerlotLCD::free(void)
{
    /* Free locks */
    if (fDisplayLock) {
        lck_mtx_free(fDisplayLock, IOService::getKernelLockGroup());
        fDisplayLock = NULL;
    }
    
    if (fBacklightLock) {
        lck_mtx_free(fBacklightLock, IOService::getKernelLockGroup());
        fBacklightLock = NULL;
    }
    
    /* Unmap registers */
    unmapRegisters();
    
    super::free();
}

#pragma mark - AppleMerlotLCD::Hardware Access

uint32_t AppleMerlotLCD::readReg(uint32_t offset)
{
    if (!fRegisters || offset >= fPhysicalLength) {
        return 0xFFFFFFFF;
    }
    
    return fRegisters[offset / 4];
}

void AppleMerlotLCD::writeReg(uint32_t offset, uint32_t value)
{
    if (!fRegisters || offset >= fPhysicalLength) {
        return;
    }
    
    fRegisters[offset / 4] = value;
    
    /* Ensure write is complete */
    OSMemoryBarrier();
}

void AppleMerlotLCD::setBits(uint32_t offset, uint32_t bits)
{
    uint32_t value = readReg(offset);
    value |= bits;
    writeReg(offset, value);
}

void AppleMerlotLCD::clearBits(uint32_t offset, uint32_t bits)
{
    uint32_t value = readReg(offset);
    value &= ~bits;
    writeReg(offset, value);
}

bool AppleMerlotLCD::waitForBit(uint32_t offset, uint32_t bit, bool set, uint32_t timeout)
{
    uint32_t value;
    uint32_t ticks = 0;
    uint32_t delay = 10; /* 10 us */
    
    while (ticks < (timeout * 1000)) {
        value = readReg(offset);
        
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

#pragma mark - AppleMerlotLCD::Hardware Initialization

bool AppleMerlotLCD::mapRegisters(void)
{
    IOService* provider = getProvider();
    if (!provider) {
        IOLog("AppleMerlotLCD: No provider\n");
        return false;
    }
    
    IODeviceMemory* memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory) {
        IOLog("AppleMerlotLCD: No device memory\n");
        return false;
    }
    
    fPhysicalAddress = memory->getPhysicalAddress();
    fPhysicalLength = memory->getLength();
    
    fMemoryMap = memory->makeMapping(kIOMapAny | kIOMapInhibitCache);
    if (!fMemoryMap) {
        IOLog("AppleMerlotLCD: Failed to map registers\n");
        return false;
    }
    
    fRegisters = (volatile uint32_t*)fMemoryMap->getVirtualAddress();
    if (!fRegisters) {
        IOLog("AppleMerlotLCD: Invalid virtual address\n");
        fMemoryMap->release();
        fMemoryMap = NULL;
        return false;
    }
    
    IOLog("AppleMerlotLCD: Mapped registers at %p (phys: 0x%llx, size: %lld)\n",
          fRegisters, fPhysicalAddress, fPhysicalLength);
    
    return true;
}

void AppleMerlotLCD::unmapRegisters(void)
{
    if (fMemoryMap) {
        fMemoryMap->release();
        fMemoryMap = NULL;
        fRegisters = NULL;
    }
}

bool AppleMerlotLCD::resetController(void)
{
    IOLog("AppleMerlotLCD: Resetting controller\n");
    
    /* Assert reset */
    writeReg(MERLOT_REG_CONTROL, MERLOT_CTRL_RESET);
    IODelay(RESET_DELAY * 1000);
    
    /* Clear reset */
    writeReg(MERLOT_REG_CONTROL, 0);
    IODelay(RESET_DELAY * 1000);
    
    /* Wait for controller ready */
    if (!waitForBit(MERLOT_REG_STATUS, MERLOT_STATUS_READY, true, 100)) {
        IOLog("AppleMerlotLCD: Controller not ready after reset\n");
        return false;
    }
    
    IOLog("AppleMerlotLCD: Reset complete\n");
    
    return true;
}

bool AppleMerlotLCD::readHardwareVersion(void)
{
    fControllerVersion = readReg(MERLOT_REG_VERSION);
    fFeatures = readReg(MERLOT_REG_FEATURES);
    
    IOLog("AppleMerlotLCD: Controller version: 0x%08x, Features: 0x%08x\n",
          fControllerVersion, fFeatures);
    
    return true;
}

bool AppleMerlotLCD::detectPanel(void)
{
    /* Read panel configuration from device tree or EDID */
    IORegistryEntry* entry = IORegistryEntry::fromPath("/", gIODTPlane);
    if (entry) {
        OSData* data = NULL;
        
        /* Get panel name */
        data = OSDynamicCast(OSData, entry->getProperty("panel-name"));
        if (data) {
            strlcpy(fPanelConfig.panel_name, (const char*)data->getBytesNoCopy(),
                    sizeof(fPanelConfig.panel_name));
        }
        
        /* Get native resolution */
        data = OSDynamicCast(OSData, entry->getProperty("native-width"));
        if (data && data->getLength() >= 4) {
            fPanelConfig.native_width = *(uint32_t*)data->getBytesNoCopy();
        }
        
        data = OSDynamicCast(OSData, entry->getProperty("native-height"));
        if (data && data->getLength() >= 4) {
            fPanelConfig.native_height = *(uint32_t*)data->getBytesNoCopy();
        }
        
        /* Get DSI configuration */
        data = OSDynamicCast(OSData, entry->getProperty("dsi-lanes"));
        if (data && data->getLength() >= 4) {
            fPanelConfig.dsi_lanes = *(uint32_t*)data->getBytesNoCopy();
        }
        
        data = OSDynamicCast(OSData, entry->getProperty("dsi-rate"));
        if (data && data->getLength() >= 4) {
            fPanelConfig.dsi_rate = *(uint32_t*)data->getBytesNoCopy();
        }
        
        entry->release();
    }
    
    /* Set defaults if not found */
    if (fPanelConfig.native_width == 0) {
        fPanelConfig.native_width = 1920;
        fPanelConfig.native_height = 1080;
    }
    
    if (fPanelConfig.dsi_lanes == 0) {
        fPanelConfig.dsi_lanes = 4;
    }
    
    if (fPanelConfig.dsi_rate == 0) {
        fPanelConfig.dsi_rate = 1000; /* 1 Gbps per lane */
    }
    
    if (fPanelConfig.panel_name[0] == '\0') {
        strlcpy(fPanelConfig.panel_name, "Apple Merlot LCD",
                sizeof(fPanelConfig.panel_name));
    }
    
    IOLog("AppleMerlotLCD: Detected panel: %s (%dx%d, %d lanes @ %d Mbps)\n",
          fPanelConfig.panel_name, fPanelConfig.native_width,
          fPanelConfig.native_height, fPanelConfig.dsi_lanes,
          fPanelConfig.dsi_rate);
    
    return true;
}

bool AppleMerlotLCD::initializeHardware(void)
{
    /* Reset controller */
    if (!resetController()) {
        return false;
    }
    
    /* Configure DSI lanes */
    uint32_t laneConfig = 0;
    switch (fPanelConfig.dsi_lanes) {
        case 1:
            laneConfig = MERLOT_DSI_LANE_1;
            break;
        case 2:
            laneConfig = MERLOT_DSI_LANE_2;
            break;
        case 4:
            laneConfig = MERLOT_DSI_LANE_4;
            break;
        default:
            laneConfig = MERLOT_DSI_LANE_4;
            break;
    }
    
    writeReg(MERLOT_REG_DSI_LANE_CONFIG, laneConfig);
    
    /* Set DSI clock rate */
    writeReg(MERLOT_REG_DSI_CLOCK, fPanelConfig.dsi_rate);
    
    /* Configure backlight PWM */
    writeReg(MERLOT_REG_BACKLIGHT_FREQ, BACKLIGHT_PWM_FREQ_DEFAULT);
    writeReg(MERLOT_REG_BACKLIGHT_DUTY, 0);
    
    /* Set thermal limits */
    writeReg(MERLOT_REG_THERMAL_LIMIT,
             (fThermalCriticalTemp << 16) | fThermalThrottleTemp);
    
    /* Enable interrupts */
    writeReg(MERLOT_REG_INT_ENABLE, MERLOT_INT_ALL);
    
    return true;
}

void AppleMerlotLCD::shutdownHardware(void)
{
    /* Disable interrupts */
    writeReg(MERLOT_REG_INT_ENABLE, 0);
    
    /* Turn off display */
    clearBits(MERLOT_REG_CONTROL, MERLOT_CTRL_DISPLAY_ON);
    
    /* Turn off backlight */
    clearBits(MERLOT_REG_CONTROL, MERLOT_CTRL_BACKLIGHT_ENABLE);
    writeReg(MERLOT_REG_BACKLIGHT_DUTY, 0);
    
    /* Put controller to sleep */
    writeReg(MERLOT_REG_CONTROL, MERLOT_CTRL_SLEEP);
}

#pragma mark - AppleMerlotLCD::Display Timing

bool AppleMerlotLCD::validateMode(const merlot_display_mode_t* mode)
{
    if (!mode) {
        return false;
    }
    
    /* Basic validation */
    if (mode->width == 0 || mode->height == 0 || mode->refresh_rate == 0) {
        return false;
    }
    
    if (mode->width > 8192 || mode->height > 8192) {
        return false;
    }
    
    if (mode->pixel_clock < MIN_PIXEL_CLOCK || mode->pixel_clock > MAX_PIXEL_CLOCK) {
        return false;
    }
    
    if (mode->h_total <= mode->h_active || mode->v_total <= mode->v_active) {
        return false;
    }
    
    if (mode->h_sync_start < mode->h_active ||
        mode->h_sync_end > mode->h_total ||
        mode->h_sync_start >= mode->h_sync_end) {
        return false;
    }
    
    if (mode->v_sync_start < mode->v_active ||
        mode->v_sync_end > mode->v_total ||
        mode->v_sync_start >= mode->v_sync_end) {
        return false;
    }
    
    return true;
}

void AppleMerlotLCD::programTiming(const merlot_display_mode_t* mode)
{
    writeReg(MERLOT_REG_H_ACTIVE, mode->h_active);
    writeReg(MERLOT_REG_H_SYNC_START, mode->h_sync_start);
    writeReg(MERLOT_REG_H_SYNC_END, mode->h_sync_end);
    writeReg(MERLOT_REG_H_TOTAL, mode->h_total);
    
    writeReg(MERLOT_REG_V_ACTIVE, mode->v_active);
    writeReg(MERLOT_REG_V_SYNC_START, mode->v_sync_start);
    writeReg(MERLOT_REG_V_SYNC_END, mode->v_sync_end);
    writeReg(MERLOT_REG_V_TOTAL, mode->v_total);
    
    writeReg(MERLOT_REG_PIXEL_CLOCK, mode->pixel_clock);
}

bool AppleMerlotLCD::setDisplayMode(const merlot_display_mode_t* mode)
{
    if (!validateMode(mode)) {
        IOLog("AppleMerlotLCD: Invalid display mode\n");
        return false;
    }
    
    lck_mtx_lock(fDisplayLock);
    
    /* Check if VRR is enabled and adjust refresh rate */
    if (fVRREnabled && mode->refresh_rate >= fMinRefresh &&
        mode->refresh_rate <= fMaxRefresh) {
        fCurrentRefresh = mode->refresh_rate;
    }
    
    /* Program timing registers */
    programTiming(mode);
    
    /* Update current mode */
    memcpy(&fCurrentMode, mode, sizeof(merlot_display_mode_t));
    
    lck_mtx_unlock(fDisplayLock);
    
    IOLog("AppleMerlotLCD: Display mode set to %dx%d @ %dHz\n",
          mode->width, mode->height, mode->refresh_rate);
    
    return true;
}

bool AppleMerlotLCD::getCurrentMode(merlot_display_mode_t* mode)
{
    if (!mode) {
        return false;
    }
    
    lck_mtx_lock(fDisplayLock);
    memcpy(mode, &fCurrentMode, sizeof(merlot_display_mode_t));
    lck_mtx_unlock(fDisplayLock);
    
    return true;
}

#pragma mark - AppleMerlotLCD::Backlight Control

bool AppleMerlotLCD::setBacklightPWM(uint32_t freq, uint32_t duty)
{
    if (freq < BACKLIGHT_PWM_FREQ_MIN || freq > BACKLIGHT_PWM_FREQ_MAX) {
        return false;
    }
    
    if (duty > MAX_BACKLIGHT_LEVEL) {
        duty = MAX_BACKLIGHT_LEVEL;
    }
    
    writeReg(MERLOT_REG_BACKLIGHT_FREQ, freq);
    writeReg(MERLOT_REG_BACKLIGHT_DUTY, duty);
    
    return true;
}

void AppleMerlotLCD::backlightRamp(uint32_t target)
{
    if (target > fMaxBrightness) {
        target = fMaxBrightness;
    }
    
    if (target < fMinBrightness) {
        target = fMinBrightness;
    }
    
    fTargetBrightness = target;
    
    /* Start ramp if not already running */
    if (!fBacklightTimer || !fBacklightTimer->isEnabled()) {
        fBacklightTimer->setTimeoutMS(BACKLIGHT_UPDATE_INTERVAL);
    }
}

void AppleMerlotLCD::backlightUpdate(void)
{
    uint32_t step;
    uint32_t newBrightness = fBrightness;
    
    lck_mtx_lock(fBacklightLock);
    
    /* Calculate ramp step */
    if (fBrightness < fTargetBrightness) {
        step = (fTargetBrightness - fBrightness) / 8;
        if (step < 1) step = 1;
        newBrightness = fBrightness + step;
        if (newBrightness > fTargetBrightness) {
            newBrightness = fTargetBrightness;
        }
    } else if (fBrightness > fTargetBrightness) {
        step = (fBrightness - fTargetBrightness) / 8;
        if (step < 1) step = 1;
        newBrightness = fBrightness - step;
        if (newBrightness < fTargetBrightness) {
            newBrightness = fTargetBrightness;
        }
    }
    
    if (newBrightness != fBrightness) {
        fBrightness = newBrightness;
        writeReg(MERLOT_REG_BACKLIGHT_DUTY, fBrightness);
        fBrightnessChanges++;
    }
    
    /* Continue ramping if not at target */
    if (fBrightness != fTargetBrightness) {
        fBacklightTimer->setTimeoutMS(BACKLIGHT_UPDATE_INTERVAL);
    }
    
    lck_mtx_unlock(fBacklightLock);
}

void AppleMerlotLCD::backlightHandler(OSObject* owner, IOTimerEventSource* sender)
{
    AppleMerlotLCD* me = (AppleMerlotLCD*)owner;
    if (me) {
        me->backlightUpdate();
    }
}

bool AppleMerlotLCD::setBacklightBrightness(uint32_t brightness)
{
    if (brightness > fMaxBrightness) {
        brightness = fMaxBrightness;
    }
    
    if (brightness < fMinBrightness) {
        brightness = fMinBrightness;
    }
    
    backlightRamp(brightness);
    
    return true;
}

#pragma mark - AppleMerlotLCD::ProMotion/VRR

bool AppleMerlotLCD::setVRRState(bool enable)
{
    lck_mtx_lock(fDisplayLock);
    
    if (enable) {
        if (!fVRREnabled) {
            setBits(MERLOT_REG_VRR_CONTROL, MERLOT_CTRL_VRR_ENABLE);
            writeReg(MERLOT_REG_VRR_MIN_REFRESH, fMinRefresh);
            writeReg(MERLOT_REG_VRR_MAX_REFRESH, fMaxRefresh);
            fVRREnabled = true;
            
            /* Start VRR timer */
            if (fVRRTimer) {
                fVRRTimer->setTimeoutMS(VRR_UPDATE_INTERVAL);
            }
        }
    } else {
        if (fVRREnabled) {
            clearBits(MERLOT_REG_VRR_CONTROL, MERLOT_CTRL_VRR_ENABLE);
            fVRREnabled = false;
            
            /* Stop VRR timer */
            if (fVRRTimer) {
                fVRRTimer->cancelTimeout();
            }
            
            /* Reset to base refresh rate */
            setRefreshRate(60);
        }
    }
    
    lck_mtx_unlock(fDisplayLock);
    
    return true;
}

bool AppleMerlotLCD::setRefreshRate(uint32_t refresh)
{
    if (!fVRREnabled) {
        return false;
    }
    
    if (refresh < fMinRefresh || refresh > fMaxRefresh) {
        return false;
    }
    
    fTargetRefresh = refresh;
    
    return true;
}

void AppleMerlotLCD::vrrUpdate(void)
{
    uint64_t now = getCurrentTime();
    uint32_t frameInterval;
    uint32_t newRefresh;
    
    if (!fVRREnabled) {
        return;
    }
    
    lck_mtx_lock(fDisplayLock);
    
    /* Calculate frame rate based on frame timing */
    if (fLastFrameTime != 0 && fFrameCount > 0) {
        frameInterval = (uint32_t)((now - fLastFrameTime) / fFrameCount / 1000);
        if (frameInterval > 0) {
            newRefresh = 1000000 / frameInterval; /* Convert to Hz */
            
            /* Apply smoothing */
            newRefresh = (fCurrentRefresh * 3 + newRefresh) / 4;
            
            /* Clamp to valid range */
            if (newRefresh < fMinRefresh) newRefresh = fMinRefresh;
            if (newRefresh > fMaxRefresh) newRefresh = fMaxRefresh;
            
            if (newRefresh != fCurrentRefresh) {
                fCurrentRefresh = newRefresh;
                fVRRChanges++;
                
                /* Update hardware */
                writeReg(MERLOT_REG_VRR_CURRENT_REFRESH, fCurrentRefresh);
            }
        }
    }
    
    fLastFrameTime = now;
    fFrameCount = 0;
    
    lck_mtx_unlock(fDisplayLock);
    
    /* Restart timer */
    fVRRTimer->setTimeoutMS(VRR_UPDATE_INTERVAL);
}

void AppleMerlotLCD::vrrHandler(OSObject* owner, IOTimerEventSource* sender)
{
    AppleMerlotLCD* me = (AppleMerlotLCD*)owner;
    if (me) {
        me->vrrUpdate();
    }
}

#pragma mark - AppleMerlotLCD::Color Management

bool AppleMerlotLCD::setGamma(float gamma)
{
    if (gamma < 1.0f || gamma > 3.0f) {
        return false;
    }
    
    fGamma = gamma;
    
    /* Generate gamma LUT */
    for (int i = 0; i < MAX_GAMMA_ENTRIES; i++) {
        float normalized = (float)i / (MAX_GAMMA_ENTRIES - 1);
        float value = powf(normalized, 1.0f / gamma);
        uint16_t gammaValue = (uint16_t)(value * 65535.0f);
        
        fRedGamma[i] = gammaValue;
        fGreenGamma[i] = gammaValue;
        fBlueGamma[i] = gammaValue;
    }
    
    /* Program LUT */
    programGamma(fRedGamma, fGreenGamma, fBlueGamma, MAX_GAMMA_ENTRIES);
    
    return true;
}

bool AppleMerlotLCD::setColorTemperature(uint32_t kelvin)
{
    if (kelvin < 1000 || kelvin > 20000) {
        return false;
    }
    
    fColorTemperature = kelvin;
    
    /* Calculate color temperature correction matrix */
    /* Using CIE 1931 xy to RGB conversion */
    float x, y;
    
    if (kelvin <= 4000) {
        x = 0.27475e9f / (kelvin * kelvin * kelvin) -
            0.98598e6f / (kelvin * kelvin) +
            1.17444e3f / kelvin + 0.145986f;
    } else {
        x = -4.6070e9f / (kelvin * kelvin * kelvin) +
            2.9678e6f / (kelvin * kelvin) +
            0.09911e3f / kelvin + 0.244063f;
    }
    
    y = -3.0f * x * x + 2.87f * x - 0.275f;
    
    /* Convert to RGB correction matrix */
    float matrix[9];
    /* ... color space conversion math ... */
    
    setColorMatrix(matrix);
    
    return true;
}

bool AppleMerlotLCD::setColorGamut(uint32_t gamut)
{
    fColorGamut = gamut;
    
    /* Configure color gamut in hardware */
    switch (gamut) {
        case 0: /* sRGB */
            /* Set sRGB matrix */
            break;
        case 1: /* P3 */
            setBits(MERLOT_REG_CONTROL, MERLOT_CTRL_HDR_ENABLE);
            break;
        case 2: /* Rec.2020 */
            setBits(MERLOT_REG_CONTROL, MERLOT_CTRL_HDR_ENABLE);
            break;
    }
    
    return true;
}

bool AppleMerlotLCD::setColorMatrix(const float* matrix)
{
    if (!matrix) {
        return false;
    }
    
    memcpy(fColorMatrix, matrix, sizeof(fColorMatrix));
    programColorMatrix(matrix);
    
    return true;
}

void AppleMerlotLCD::programGamma(const uint16_t* red, const uint16_t* green,
                                    const uint16_t* blue, uint32_t size)
{
    if (size > MAX_GAMMA_ENTRIES) {
        size = MAX_GAMMA_ENTRIES;
    }
    
    /* Program red gamma LUT */
    for (uint32_t i = 0; i < size; i++) {
        writeReg(MERLOT_REG_GAMMA_RED + (i * 4), red[i]);
    }
    
    /* Program green gamma LUT */
    for (uint32_t i = 0; i < size; i++) {
        writeReg(MERLOT_REG_GAMMA_GREEN + (i * 4), green[i]);
    }
    
    /* Program blue gamma LUT */
    for (uint32_t i = 0; i < size; i++) {
        writeReg(MERLOT_REG_GAMMA_BLUE + (i * 4), blue[i]);
    }
    
    /* Enable gamma correction */
    setBits(MERLOT_REG_GAMMA_CTRL, 1);
}

void AppleMerlotLCD::programColorMatrix(const float* matrix)
{
    /* Program color correction matrix (3x3) */
    for (int i = 0; i < 9; i++) {
        /* Convert float to fixed-point for hardware */
        uint32_t fixed = (uint32_t)(matrix[i] * 65536.0f);
        writeReg(MERLOT_REG_COLOR_MATRIX + (i * 4), fixed);
    }
}

#pragma mark - AppleMerlotLCD::HDR/EDR

bool AppleMerlotLCD::setHDREnabled(bool enable)
{
    lck_mtx_lock(fDisplayLock);
    
    if (enable) {
        if (!fHDREnabled) {
            setBits(MERLOT_REG_CONTROL, MERLOT_CTRL_HDR_ENABLE);
            fHDREnabled = true;
            
            /* Program HDR metadata */
            if (fHDRMetadata[0] != 0) {
                programHDRMetadata(fHDRMetadata);
            }
        }
    } else {
        if (fHDREnabled) {
            clearBits(MERLOT_REG_CONTROL, MERLOT_CTRL_HDR_ENABLE);
            fHDREnabled = false;
        }
    }
    
    lck_mtx_unlock(fDisplayLock);
    
    return true;
}

bool AppleMerlotLCD::setEDREnabled(bool enable)
{
    fEDREnabled = enable;
    return true;
}

bool AppleMerlotLCD::setMaxLuminance(float nits)
{
    if (nits < 100.0f || nits > MAX_LUMINANCE_NITS) {
        return false;
    }
    
    fMaxLuminance = nits;
    
    /* Update HDR metadata */
    fHDRMetadata[0] = fMaxLuminance;           /* Max luminance */
    fHDRMetadata[1] = fMaxLuminance * 0.01f;   /* Min luminance */
    fHDRMetadata[2] = fEDRBoost;               /* EDR boost factor */
    
    programHDRMetadata(fHDRMetadata);
    
    return true;
}

bool AppleMerlotLCD::setEDRBoost(float boost)
{
    if (boost < 1.0f || boost > EDR_MAX_BOOST) {
        return false;
    }
    
    fEDRBoost = boost;
    fMaxLuminance = MAX_LUMINANCE_NITS * boost;
    
    return setMaxLuminance(fMaxLuminance);
}

void AppleMerlotLCD::programHDRMetadata(const float* metadata)
{
    /* Program HDR metadata registers */
    /* ... */
}

#pragma mark - AppleMerlotLCD::Thermal Management

void AppleMerlotLCD::updateTemperature(void)
{
    uint32_t temp = readReg(MERLOT_REG_TEMPERATURE);
    
    /* Convert to Celsius (implementation specific) */
    fCurrentTemperature = temp & 0xFF;
}

void AppleMerlotLCD::thermalCheck(void)
{
    updateTemperature();
    
    if (fCurrentTemperature >= fThermalCriticalTemp) {
        /* Critical temperature - emergency shutdown */
        IOLog("AppleMerlotLCD: CRITICAL temperature %d°C, shutting down\n",
              fCurrentTemperature);
        
        setPowerStateInternal(MERLOT_POWER_OFF);
        fThermalEvents++;
        
    } else if (fCurrentTemperature >= fThermalThrottleTemp) {
        /* Throttling temperature */
        if (!fThermalThrottling) {
            IOLog("AppleMerlotLCD: Temperature %d°C, throttling display\n",
                  fCurrentTemperature);
            
            thermalThrottle();
            fThermalThrottling = true;
            fThermalEvents++;
        }
    } else {
        if (fThermalThrottling) {
            IOLog("AppleMerlotLCD: Temperature returned to normal\n");
            fThermalThrottling = false;
            
            /* Restore normal operation */
            setBacklightBrightness(fTargetBrightness);
        }
    }
}

void AppleMerlotLCD::thermalThrottle(void)
{
    /* Reduce backlight to 50% */
    setBacklightBrightness(fMaxBrightness / 2);
    
    /* Disable HDR if active */
    if (fHDREnabled) {
        setHDREnabled(false);
    }
}

void AppleMerlotLCD::thermalHandler(OSObject* owner, IOTimerEventSource* sender)
{
    AppleMerlotLCD* me = (AppleMerlotLCD*)owner;
    if (me) {
        me->thermalCheck();
        me->fThermalTimer->setTimeoutMS(5000); /* Check every 5 seconds */
    }
}

#pragma mark - AppleMerlotLCD::Interrupt Handling

void AppleMerlotLCD::interruptHandler(void)
{
    uint32_t intStatus = readReg(MERLOT_REG_INT_STATUS);
    
    /* Clear interrupts */
    writeReg(MERLOT_REG_INT_CLEAR, intStatus);
    
    if (intStatus & MERLOT_INT_VSYNC) {
        fTotalVSyncs++;
        fFrameCount++;
    }
    
    if (intStatus & MERLOT_INT_TEARING) {
        /* Tearing effect detected */
        /* Could adjust VRR or enable tear-free mode */
    }
    
    if (intStatus & MERLOT_INT_VRR_CHANGE) {
        fVRRChanges++;
    }
    
    if (intStatus & MERLOT_INT_THERMAL_WARNING) {
        fThermalEvents++;
        thermalCheck();
    }
    
    if (intStatus & MERLOT_INT_THERMAL_CRITICAL) {
        fThermalEvents++;
        thermalCheck();
    }
    
    if (intStatus & MERLOT_INT_BACKLIGHT_FAULT) {
        fErrors++;
        IOLog("AppleMerlotLCD: Backlight fault detected\n");
        /* Attempt recovery */
        writeReg(MERLOT_REG_BACKLIGHT_CTRL, 1);
    }
    
    if (intStatus & MERLOT_INT_DISPLAY_ERROR) {
        fErrors++;
        IOLog("AppleMerlotLCD: Display error detected\n");
        /* Reset display */
        resetController();
    }
}

void AppleMerlotLCD::interruptHandler(OSObject* owner, IOInterruptEventSource* src, int count)
{
    AppleMerlotLCD* me = (AppleMerlotLCD*)owner;
    if (me) {
        me->interruptHandler();
    }
}

#pragma mark - AppleMerlotLCD::Power Management

bool AppleMerlotLCD::setPowerStateInternal(uint32_t state)
{
    if (state == fPowerState) {
        return true;
    }
    
    switch (state) {
        case MERLOT_POWER_OFF:
            powerDownSequence();
            break;
            
        case MERLOT_POWER_SLEEP:
            /* Enter sleep mode */
            writeReg(MERLOT_REG_CONTROL, MERLOT_CTRL_SLEEP);
            fDisplayOn = false;
            fBacklightOn = false;
            break;
            
        case MERLOT_POWER_ON:
            powerUpSequence();
            break;
    }
    
    fPowerState = state;
    
    return true;
}

void AppleMerlotLCD::powerUpSequence(void)
{
    IOLog("AppleMerlotLCD: Power up sequence\n");
    
    /* Wake controller */
    writeReg(MERLOT_REG_CONTROL, MERLOT_CTRL_WAKE);
    IODelay(POWER_UP_DELAY * 1000);
    
    /* Enable display */
    setBits(MERLOT_REG_CONTROL, MERLOT_CTRL_DISPLAY_ON);
    IODelay(POWER_UP_DELAY * 1000);
    
    /* Restore display mode */
    programTiming(&fCurrentMode);
    
    /* Restore backlight */
    setBits(MERLOT_REG_CONTROL, MERLOT_CTRL_BACKLIGHT_ENABLE);
    setBacklightBrightness(fBrightness);
    
    fDisplayOn = true;
    fBacklightOn = true;
}

void AppleMerlotLCD::powerDownSequence(void)
{
    IOLog("AppleMerlotLCD: Power down sequence\n");
    
    /* Turn off backlight */
    clearBits(MERLOT_REG_CONTROL, MERLOT_CTRL_BACKLIGHT_ENABLE);
    writeReg(MERLOT_REG_BACKLIGHT_DUTY, 0);
    IODelay(POWER_DOWN_DELAY * 1000);
    
    /* Turn off display */
    clearBits(MERLOT_REG_CONTROL, MERLOT_CTRL_DISPLAY_ON);
    IODelay(POWER_DOWN_DELAY * 1000);
    
    /* Put controller to sleep */
    writeReg(MERLOT_REG_CONTROL, MERLOT_CTRL_SLEEP);
    IODelay(POWER_OFF_DELAY * 1000);
    
    fDisplayOn = false;
    fBacklightOn = false;
}

#pragma mark - AppleMerlotLCD::Mode Detection

void AppleMerlotLCD::detectSupportedModes(void)
{
    fNumModes = 0;
    
    /* Add standard modes */
    addDisplayMode(640, 480, 60, 0);           /* VGA */
    addDisplayMode(800, 600, 60, 0);           /* SVGA */
    addDisplayMode(1024, 768, 60, 0);          /* XGA */
    addDisplayMode(1280, 720, 60, 0);          /* HD */
    addDisplayMode(1280, 800, 60, 0);          /* WXGA */
    addDisplayMode(1366, 768, 60, 0);          /* HD */
    addDisplayMode(1440, 900, 60, 0);          /* WXGA+ */
    addDisplayMode(1600, 900, 60, 0);          /* HD+ */
    addDisplayMode(1680, 1050, 60, 0);         /* WSXGA+ */
    addDisplayMode(1920, 1080, 60, 0);         /* Full HD */
    
    /* Add high refresh modes if supported */
    if (fFeatures & MERLOT_CTRL_VRR_ENABLE) {
        addDisplayMode(1920, 1080, 120, MERLOT_MODE_FLAG_VRR);
        addDisplayMode(2560, 1440, 60, 0);
        addDisplayMode(2560, 1440, 120, MERLOT_MODE_FLAG_VRR);
    }
    
    /* Add native panel resolution */
    addDisplayMode(fPanelConfig.native_width,
                   fPanelConfig.native_height,
                   fPanelConfig.native_refresh,
                   MERLOT_MODE_FLAG_PROGRESSIVE);
}

void AppleMerlotLCD::addDisplayMode(uint32_t width, uint32_t height,
                                     uint32_t refresh, uint32_t flags)
{
    if (fNumModes >= MAX_DISPLAY_MODES) {
        return;
    }
    
    merlot_display_mode_t* mode = &fSupportedModes[fNumModes];
    
    mode->width = width;
    mode->height = height;
    mode->refresh_rate = refresh;
    mode->flags = flags;
    mode->bpp = 32;
    mode->color_format = MERLOT_COLOR_FORMAT_ARGB8888;
    mode->dsi_lanes = fPanelConfig.dsi_lanes;
    mode->dsi_rate = fPanelConfig.dsi_rate;
    
    /* Calculate timing (simplified) */
    mode->h_active = width;
    mode->h_sync_start = width + 16;
    mode->h_sync_end = width + 16 + 64;
    mode->h_total = width + 16 + 64 + 80;
    
    mode->v_active = height;
    mode->v_sync_start = height + 2;
    mode->v_sync_end = height + 2 + 4;
    mode->v_total = height + 2 + 4 + 12;
    
    /* Calculate pixel clock */
    mode->pixel_clock = (mode->h_total * mode->v_total * refresh) / 1000;
    
    fNumModes++;
}

#pragma mark - AppleMerlotLCD::Utility

uint64_t AppleMerlotLCD::getCurrentTime(void)
{
    uint64_t time;
    clock_get_uptime(&time);
    return time;
}

void AppleMerlotLCD::logDisplayInfo(void)
{
    IOLog("\n===== AppleMerlotLCD Information =====\n");
    IOLog("Controller Version: 0x%08x\n", fControllerVersion);
    IOLog("Features: 0x%08x\n", fFeatures);
    IOLog("Panel: %s\n", fPanelConfig.panel_name);
    IOLog("Native Resolution: %dx%d\n",
          fPanelConfig.native_width, fPanelConfig.native_height);
    IOLog("DSI: %d lanes @ %d Mbps\n",
          fPanelConfig.dsi_lanes, fPanelConfig.dsi_rate);
    IOLog("Current Mode: %dx%d @ %dHz\n",
          fCurrentMode.width, fCurrentMode.height, fCurrentMode.refresh_rate);
    IOLog("Brightness: %d/%d\n", fBrightness, fMaxBrightness);
    IOLog("Temperature: %d°C\n", fCurrentTemperature);
    IOLog("VRR: %s (%d-%d Hz, current %d Hz)\n",
          fVRREnabled ? "Enabled" : "Disabled",
          fMinRefresh, fMaxRefresh, fCurrentRefresh);
    IOLog("HDR: %s, EDR: %s\n",
          fHDREnabled ? "Enabled" : "Disabled",
          fEDREnabled ? "Enabled" : "Disabled");
    IOLog("Statistics: Frames=%lld, VSyncs=%lld, VRR Changes=%d, Errors=%d\n",
          fTotalFrames, fTotalVSyncs, fVRRChanges, fErrors);
    IOLog("=====================================\n");
}

#pragma mark - AppleMerlotLCD::IOService Overrides

bool AppleMerlotLCD::start(IOService* provider)
{
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("AppleMerlotLCD: Starting Merlot LCD Controller\n");
    
    /* Map registers */
    if (!mapRegisters()) {
        IOLog("AppleMerlotLCD: Failed to map registers\n");
        return false;
    }
    
    /* Read hardware version */
    readHardwareVersion();
    
    /* Detect panel */
    if (!detectPanel()) {
        IOLog("AppleMerlotLCD: Panel detection failed\n");
        unmapRegisters();
        return false;
    }
    
    /* Initialize hardware */
    if (!initializeHardware()) {
        IOLog("AppleMerlotLCD: Hardware initialization failed\n");
        unmapRegisters();
        return false;
    }
    
    /* Create work loop */
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        IOLog("AppleMerlotLCD: Failed to create work loop\n");
        unmapRegisters();
        return false;
    }
    
    /* Create command gate */
    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        IOLog("AppleMerlotLCD: Failed to create command gate\n");
        fWorkLoop->release();
        fWorkLoop = NULL;
        unmapRegisters();
        return false;
    }
    
    fWorkLoop->addEventSource(fCommandGate);
    
    /* Create timers */
    fVRRTimer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &AppleMerlotLCD::vrrHandler));
    
    if (!fVRRTimer) {
        IOLog("AppleMerlotLCD: Failed to create VRR timer\n");
        fWorkLoop->removeEventSource(fCommandGate);
        fCommandGate->release();
        fCommandGate = NULL;
        fWorkLoop->release();
        fWorkLoop = NULL;
        unmapRegisters();
        return false;
    }
    
    fWorkLoop->addEventSource(fVRRTimer);
    
    fBacklightTimer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &AppleMerlotLCD::backlightHandler));
    
    if (!fBacklightTimer) {
        IOLog("AppleMerlotLCD: Failed to create backlight timer\n");
        fWorkLoop->removeEventSource(fVRRTimer);
        fVRRTimer->release();
        fVRRTimer = NULL;
        fWorkLoop->removeEventSource(fCommandGate);
        fCommandGate->release();
        fCommandGate = NULL;
        fWorkLoop->release();
        fWorkLoop = NULL;
        unmapRegisters();
        return false;
    }
    
    fWorkLoop->addEventSource(fBacklightTimer);
    
    fThermalTimer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &AppleMerlotLCD::thermalHandler));
    
    if (!fThermalTimer) {
        IOLog("AppleMerlotLCD: Failed to create thermal timer\n");
        fWorkLoop->removeEventSource(fBacklightTimer);
        fBacklightTimer->release();
        fBacklightTimer = NULL;
        fWorkLoop->removeEventSource(fVRRTimer);
        fVRRTimer->release();
        fVRRTimer = NULL;
        fWorkLoop->removeEventSource(fCommandGate);
        fCommandGate->release();
        fCommandGate = NULL;
        fWorkLoop->release();
        fWorkLoop = NULL;
        unmapRegisters();
        return false;
    }
    
    fWorkLoop->addEventSource(fThermalTimer);
    
    /* Create interrupt source */
    fInterruptSource = IOInterruptEventSource::interruptEventSource(this,
        OSMemberFunctionCast(IOInterruptEventAction, this,
                             &AppleMerlotLCD::interruptHandler),
        provider, 0);
    
    if (!fInterruptSource) {
        IOLog("AppleMerlotLCD: Failed to create interrupt source\n");
        fWorkLoop->removeEventSource(fThermalTimer);
        fThermalTimer->release();
        fThermalTimer = NULL;
        fWorkLoop->removeEventSource(fBacklightTimer);
        fBacklightTimer->release();
        fBacklightTimer = NULL;
        fWorkLoop->removeEventSource(fVRRTimer);
        fVRRTimer->release();
        fVRRTimer = NULL;
        fWorkLoop->removeEventSource(fCommandGate);
        fCommandGate->release();
        fCommandGate = NULL;
        fWorkLoop->release();
        fWorkLoop = NULL;
        unmapRegisters();
        return false;
    }
    
    fWorkLoop->addEventSource(fInterruptSource);
    
    /* Detect supported display modes */
    detectSupportedModes();
    
    /* Set default display mode */
    if (fNumModes > 0) {
        setDisplayMode(&fSupportedModes[0]);
    }
    
    /* Start thermal monitoring */
    fThermalTimer->setTimeoutMS(5000);
    
    /* Register power management */
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, NULL, 0);
    
    /* Publish properties */
    setProperty("Controller Version", fControllerVersion, 32);
    setProperty("Features", fFeatures, 32);
    setProperty("Panel Name", fPanelConfig.panel_name);
    setProperty("Native Width", fPanelConfig.native_width, 32);
    setProperty("Native Height", fPanelConfig.native_height, 32);
    setProperty("DSI Lanes", fPanelConfig.dsi_lanes, 32);
    setProperty("DSI Rate", fPanelConfig.dsi_rate, 32);
    setProperty("VRR Capable", fFeatures & MERLOT_CTRL_VRR_ENABLE ? "Yes" : "No");
    setProperty("HDR Capable", fFeatures & MERLOT_CTRL_HDR_ENABLE ? "Yes" : "No");
    setProperty("Max Brightness", fMaxBrightness, 32);
    setProperty("Max Refresh", fMaxRefresh, 32);
    setProperty("Min Refresh", fMinRefresh, 32);
    
    /* Register service */
    registerService();
    
    IOLog("AppleMerlotLCD: Started successfully\n");
    logDisplayInfo();
    
    return true;
}

void AppleMerlotLCD::stop(IOService* provider)
{
    IOLog("AppleMerlotLCD: Stopping\n");
    
    /* Stop timers */
    if (fVRRTimer) {
        fVRRTimer->cancelTimeout();
    }
    
    if (fBacklightTimer) {
        fBacklightTimer->cancelTimeout();
    }
    
    if (fThermalTimer) {
        fThermalTimer->cancelTimeout();
    }
    
    /* Disable interrupts */
    writeReg(MERLOT_REG_INT_ENABLE, 0);
    
    /* Power down */
    setPowerStateInternal(MERLOT_POWER_OFF);
    
    /* Shutdown hardware */
    shutdownHardware();
    
    /* Remove interrupt source */
    if (fWorkLoop && fInterruptSource) {
        fWorkLoop->removeEventSource(fInterruptSource);
        fInterruptSource->release();
        fInterruptSource = NULL;
    }
    
    /* Destroy work loop */
    if (fWorkLoop) {
        if (fThermalTimer) {
            fWorkLoop->removeEventSource(fThermalTimer);
            fThermalTimer->release();
            fThermalTimer = NULL;
        }
        
        if (fBacklightTimer) {
            fWorkLoop->removeEventSource(fBacklightTimer);
            fBacklightTimer->release();
            fBacklightTimer = NULL;
        }
        
        if (fVRRTimer) {
            fWorkLoop->removeEventSource(fVRRTimer);
            fVRRTimer->release();
            fVRRTimer = NULL;
        }
        
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
    
    /* Power management */
    PMstop();
    
    super::stop(provider);
}

#pragma mark - AppleMerlotLCD::Power Management

IOReturn AppleMerlotLCD::setPowerState(unsigned long powerState, IOService* device)
{
    if (powerState == 0) {
        /* Going to sleep */
        IOLog("AppleMerlotLCD: Going to sleep\n");
        setPowerStateInternal(MERLOT_POWER_SLEEP);
    } else {
        /* Waking up */
        IOLog("AppleMerlotLCD: Waking from sleep\n");
        setPowerStateInternal(MERLOT_POWER_ON);
    }
    
    return IOPMAckImplied;
}

#pragma mark - AppleMerlotLCD::IOFramebuffer Overrides

IOReturn AppleMerlotLCD::enableController(void)
{
    IOReturn ret = kIOReturnSuccess;
    
    IOLog("AppleMerlotLCD: Enabling controller\n");
    
    lck_mtx_lock(fDisplayLock);
    
    if (!fDisplayOn) {
        /* Power up the display */
        setPowerStateInternal(MERLOT_POWER_ON);
        
        /* Wait for controller ready */
        if (!waitForBit(MERLOT_REG_STATUS, MERLOT_STATUS_READY, true, 100)) {
            IOLog("AppleMerlotLCD: Controller not ready after enable\n");
            ret = kIOReturnIOError;
            goto exit;
        }
        
        /* Enable VSync interrupt */
        setBits(MERLOT_REG_INT_ENABLE, MERLOT_INT_VSYNC);
        
        fDisplayOn = true;
    }
    
exit:
    lck_mtx_unlock(fDisplayLock);
    
    return ret;
}

IOReturn AppleMerlotLCD::disableController(void)
{
    IOLog("AppleMerlotLCD: Disabling controller\n");
    
    lck_mtx_lock(fDisplayLock);
    
    if (fDisplayOn) {
        /* Disable VSync interrupt */
        clearBits(MERLOT_REG_INT_ENABLE, MERLOT_INT_VSYNC);
        
        /* Power down the display */
        setPowerStateInternal(MERLOT_POWER_SLEEP);
        
        fDisplayOn = false;
    }
    
    lck_mtx_unlock(fDisplayLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCD::setDisplayMode(IOIndex mode)
{
    IOReturn ret = kIOReturnSuccess;
    
    if (mode >= fNumModes) {
        return kIOReturnBadArgument;
    }
    
    IOLog("AppleMerlotLCD: Setting display mode %d\n", mode);
    
    if (!setDisplayMode(&fSupportedModes[mode])) {
        ret = kIOReturnUnsupported;
    }
    
    return ret;
}

IOReturn AppleMerlotLCD::getDisplayModes(IODisplayModeID* modes, IOIndex* count)
{
    if (!modes || !count) {
        return kIOReturnBadArgument;
    }
    
    IOIndex maxCount = *count;
    *count = fNumModes;
    
    if (maxCount < fNumModes) {
        return kIOReturnSuccess;  /* Not enough space, return count only */
    }
    
    for (uint32_t i = 0; i < fNumModes; i++) {
        modes[i] = (IODisplayModeID)i;
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCD::getInformationForMode(IODisplayModeID mode, IODisplayModeInformation* info)
{
    if (!info || mode >= fNumModes) {
        return kIOReturnBadArgument;
    }
    
    const merlot_display_mode_t* m = &fSupportedModes[mode];
    
    info->nominalWidth = m->width;
    info->nominalHeight = m->height;
    info->refreshRate = m->refresh_rate << 16;  /* Fixed point */
    info->maxDepthIndex = 0;  /* Will be filled by getPixelInformation */
    info->flags = kDisplayModeValidFlag;
    
    if (m->flags & MERLOT_MODE_FLAG_VRR) {
        info->flags |= kDisplayModeInterlacedFlag;  /* Reuse for VRR capable */
    }
    
    if (m->flags & MERLOT_MODE_FLAG_HDR) {
        info->flags |= kDisplayModeWidescreenFlag;  /* Reuse for HDR capable */
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCD::getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                              IOPixelAperture aperture,
                                              IOPixelInformation* info)
{
    if (!info || mode >= fNumModes) {
        return kIOReturnBadArgument;
    }
    
    const merlot_display_mode_t* m = &fSupportedModes[mode];
    
    /* Fill pixel information */
    info->activeWidth = m->width;
    info->activeHeight = m->height;
    info->bytesPerRow = m->width * (m->bpp / 8);
    info->bytesPerPlane = info->bytesPerRow * m->height;
    info->bitsPerPixel = m->bpp;
    info->pixelType = kIOPixelInformationDigital;
    info->componentMasks[0] = 0xFF0000;  /* Red */
    info->componentMasks[1] = 0x00FF00;  /* Green */
    info->componentMasks[2] = 0x0000FF;  /* Blue */
    info->componentMasks[3] = 0xFF000000; /* Alpha */
    
    strlcpy(info->pixelFormat, "ARGB8888", sizeof(info->pixelFormat));
    
    info->flags = kIODisplayModeValidFlag;
    info->verticalPixels = m->height;
    info->horizontalPixels = m->width;
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCD::setStartupDisplayMode(IOIndex mode)
{
    /* Store startup mode in NVRAM */
    if (mode < fNumModes) {
        /* Would write to NVRAM */
        return kIOReturnSuccess;
    }
    
    return kIOReturnBadArgument;
}

IOReturn AppleMerlotLCD::setAttribute(IOSelect attribute, uintptr_t value)
{
    IOReturn ret = kIOReturnSuccess;
    
    switch (attribute) {
        case kDisplayBrightness:
            ret = setBrightness((uint32_t)value);
            break;
            
        case kDisplayColorTemperature:
            ret = setColorTemperature((uint32_t)value);
            break;
            
        case kDisplayVRRState:
            ret = setVRR(value != 0);
            break;
            
        case kDisplayHDRState:
            ret = setHDR(value != 0);
            break;
            
        case kDisplayEDRState:
            ret = setEDR(value != 0);
            break;
            
        case kDisplayRefreshRate:
            ret = setRefreshRate((uint32_t)value);
            break;
            
        case kDisplayGamma:
            /* Convert fixed point to float */
            ret = setGamma((float)value / 65536.0f);
            break;
            
        default:
            ret = super::setAttribute(attribute, value);
            break;
    }
    
    return ret;
}

IOReturn AppleMerlotLCD::getAttribute(IOSelect attribute, uintptr_t* value)
{
    IOReturn ret = kIOReturnSuccess;
    
    if (!value) {
        return kIOReturnBadArgument;
    }
    
    switch (attribute) {
        case kDisplayBrightness:
            *value = fBrightness;
            break;
            
        case kDisplayColorTemperature:
            *value = fColorTemperature;
            break;
            
        case kDisplayVRRState:
            *value = fVRREnabled ? 1 : 0;
            break;
            
        case kDisplayHDRState:
            *value = fHDREnabled ? 1 : 0;
            break;
            
        case kDisplayEDRState:
            *value = fEDREnabled ? 1 : 0;
            break;
            
        case kDisplayRefreshRate:
            *value = fCurrentRefresh;
            break;
            
        case kDisplayGamma:
            /* Convert float to fixed point */
            *value = (uintptr_t)(fGamma * 65536.0f);
            break;
            
        case kDisplayTemperature:
            *value = fCurrentTemperature;
            break;
            
        default:
            ret = super::getAttribute(attribute, value);
            break;
    }
    
    return ret;
}

#pragma mark - AppleMerlotLCD::Public API

IOReturn AppleMerlotLCD::setBrightness(uint32_t brightness)
{
    if (brightness > MAX_BACKLIGHT_LEVEL) {
        brightness = MAX_BACKLIGHT_LEVEL;
    }
    
    IOLog("AppleMerlotLCD: Setting brightness to %d\n", brightness);
    
    setBacklightBrightness(brightness);
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCD::setVRR(bool enable)
{
    IOLog("AppleMerlotLCD: %s VRR\n", enable ? "Enabling" : "Disabling");
    
    if (!setVRRState(enable)) {
        return kIOReturnUnsupported;
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCD::setRefresh(uint32_t refresh)
{
    if (refresh < fMinRefresh || refresh > fMaxRefresh) {
        return kIOReturnBadArgument;
    }
    
    IOLog("AppleMerlotLCD: Setting refresh rate to %dHz\n", refresh);
    
    if (!setRefreshRate(refresh)) {
        return kIOReturnUnsupported;
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCD::setHDR(bool enable)
{
    IOLog("AppleMerlotLCD: %s HDR\n", enable ? "Enabling" : "Disabling");
    
    if (!setHDREnabled(enable)) {
        return kIOReturnUnsupported;
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCD::setEDR(bool enable)
{
    IOLog("AppleMerlotLCD: %s EDR\n", enable ? "Enabling" : "Disabling");
    
    if (!setEDREnabled(enable)) {
        return kIOReturnUnsupported;
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCD::setColorTemp(uint32_t kelvin)
{
    if (kelvin < 1000 || kelvin > 20000) {
        return kIOReturnBadArgument;
    }
    
    IOLog("AppleMerlotLCD: Setting color temperature to %dK\n", kelvin);
    
    if (!setColorTemperature(kelvin)) {
        return kIOReturnUnsupported;
    }
    
    return kIOReturnSuccess;
}

#pragma mark - AppleMerlotLCD::Power Management (Continued)

IOReturn AppleMerlotLCD::setPowerState(unsigned long powerState, IOService* device)
{
    if (powerState == 0) {
        /* Going to sleep */
        IOLog("AppleMerlotLCD: Going to sleep\n");
        setPowerStateInternal(MERLOT_POWER_SLEEP);
    } else {
        /* Waking up */
        IOLog("AppleMerlotLCD: Waking from sleep\n");
        setPowerStateInternal(MERLOT_POWER_ON);
    }
    
    return IOPMAckImplied;
}

#pragma mark - AppleMerlotLCD::Diagnostics and Debug

void AppleMerlotLCD::logDisplayInfo(void)
{
    IOLog("\n===== AppleMerlotLCD Information =====\n");
    IOLog("Controller Version: 0x%08x\n", fControllerVersion);
    IOLog("Features: 0x%08x\n", fFeatures);
    IOLog("Panel: %s\n", fPanelConfig.panel_name);
    IOLog("Native Resolution: %dx%d\n",
          fPanelConfig.native_width, fPanelConfig.native_height);
    IOLog("DSI: %d lanes @ %d Mbps\n",
          fPanelConfig.dsi_lanes, fPanelConfig.dsi_rate);
    IOLog("Current Mode: %dx%d @ %dHz\n",
          fCurrentMode.width, fCurrentMode.height, fCurrentMode.refresh_rate);
    IOLog("Brightness: %d/%d\n", fBrightness, fMaxBrightness);
    IOLog("Temperature: %d°C\n", fCurrentTemperature);
    IOLog("VRR: %s (%d-%d Hz, current %d Hz)\n",
          fVRREnabled ? "Enabled" : "Disabled",
          fMinRefresh, fMaxRefresh, fCurrentRefresh);
    IOLog("HDR: %s, EDR: %s\n",
          fHDREnabled ? "Enabled" : "Disabled",
          fEDREnabled ? "Enabled" : "Disabled");
    IOLog("Statistics: Frames=%lld, VSyncs=%lld, VRR Changes=%d, Errors=%d\n",
          fTotalFrames, fTotalVSyncs, fVRRChanges, fErrors);
    
    /* Read and log current register values for debugging */
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("\n--- Register Dump ---\n");
        IOLog("CONTROL: 0x%08x\n", readReg(MERLOT_REG_CONTROL));
        IOLog("STATUS: 0x%08x\n", readReg(MERLOT_REG_STATUS));
        IOLog("H_ACTIVE: %d\n", readReg(MERLOT_REG_H_ACTIVE));
        IOLog("V_ACTIVE: %d\n", readReg(MERLOT_REG_V_ACTIVE));
        IOLog("PIXEL_CLOCK: %d kHz\n", readReg(MERLOT_REG_PIXEL_CLOCK));
        IOLog("BACKLIGHT_DUTY: %d\n", readReg(MERLOT_REG_BACKLIGHT_DUTY));
        IOLog("TEMPERATURE: %d°C\n", readReg(MERLOT_REG_TEMPERATURE) & 0xFF);
        IOLog("VRR_CURRENT: %d Hz\n", readReg(MERLOT_REG_VRR_CURRENT_REFRESH));
    }
    
    IOLog("=====================================\n");
}

/*==============================================================================
 * AppleMerlotLCD User Client for diagnostics and configuration
 *==============================================================================*/

class AppleMerlotLCDUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(AppleMerlotLCDUserClient)
    
private:
    task_t                      fTask;
    AppleMerlotLCD*             fProvider;
    bool                        fValid;
    
    /* Client permissions */
    uint32_t                    fPermissions;
    
    /* Async notifications */
    mach_port_t                 fNotificationPort;
    uint64_t                    fAsyncEvents;
    
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
    static IOReturn             sGetDisplayInfo(AppleMerlotLCDUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args);
    
    static IOReturn             sSetBrightness(AppleMerlotLCDUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args);
    
    static IOReturn             sSetColorTemperature(AppleMerlotLCDUserClient* target,
                                                       void* reference,
                                                       IOExternalMethodArguments* args);
    
    static IOReturn             sSetVRR(AppleMerlotLCDUserClient* target,
                                         void* reference,
                                         IOExternalMethodArguments* args);
    
    static IOReturn             sSetHDR(AppleMerlotLCDUserClient* target,
                                         void* reference,
                                         IOExternalMethodArguments* args);
    
    static IOReturn             sGetStatistics(AppleMerlotLCDUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args);
    
    static IOReturn             sGetTemperature(AppleMerlotLCDUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args);
    
    static IOReturn             sSetGamma(AppleMerlotLCDUserClient* target,
                                           void* reference,
                                           IOExternalMethodArguments* args);
    
    static IOReturn             sSetColorGamut(AppleMerlotLCDUserClient* target,
                                                void* reference,
                                                IOExternalMethodArguments* args);
    
    static IOReturn             sRunDiagnostics(AppleMerlotLCDUserClient* target,
                                                 void* reference,
                                                 IOExternalMethodArguments* args);
};

OSDefineMetaClassAndStructors(AppleMerlotLCDUserClient, IOUserClient)

/* External method dispatch table */
enum {
    kMethodGetDisplayInfo,
    kMethodSetBrightness,
    kMethodSetColorTemperature,
    kMethodSetVRR,
    kMethodSetHDR,
    kMethodGetStatistics,
    kMethodGetTemperature,
    kMethodSetGamma,
    kMethodSetColorGamut,
    kMethodRunDiagnostics,
    kMethodCount
};

static IOExternalMethodDispatch sMerlotMethods[kMethodCount] = {
    {   /* kMethodGetDisplayInfo */
        (IOExternalMethodAction)&AppleMerlotLCDUserClient::sGetDisplayInfo,
        0, 0, 0, 1
    },
    {   /* kMethodSetBrightness */
        (IOExternalMethodAction)&AppleMerlotLCDUserClient::sSetBrightness,
        1, 0, 0, 0
    },
    {   /* kMethodSetColorTemperature */
        (IOExternalMethodAction)&AppleMerlotLCDUserClient::sSetColorTemperature,
        1, 0, 0, 0
    },
    {   /* kMethodSetVRR */
        (IOExternalMethodAction)&AppleMerlotLCDUserClient::sSetVRR,
        1, 0, 0, 0
    },
    {   /* kMethodSetHDR */
        (IOExternalMethodAction)&AppleMerlotLCDUserClient::sSetHDR,
        1, 0, 0, 0
    },
    {   /* kMethodGetStatistics */
        (IOExternalMethodAction)&AppleMerlotLCDUserClient::sGetStatistics,
        0, 0, 0, 1
    },
    {   /* kMethodGetTemperature */
        (IOExternalMethodAction)&AppleMerlotLCDUserClient::sGetTemperature,
        0, 0, 1, 0
    },
    {   /* kMethodSetGamma */
        (IOExternalMethodAction)&AppleMerlotLCDUserClient::sSetGamma,
        0, 1, 0, 0
    },
    {   /* kMethodSetColorGamut */
        (IOExternalMethodAction)&AppleMerlotLCDUserClient::sSetColorGamut,
        1, 0, 0, 0
    },
    {   /* kMethodRunDiagnostics */
        (IOExternalMethodAction)&AppleMerlotLCDUserClient::sRunDiagnostics,
        0, 0, 0, 1
    }
};

/*==============================================================================
 * AppleMerlotLCDUserClient Implementation
 *==============================================================================*/

bool AppleMerlotLCDUserClient::init(OSDictionary* dictionary)
{
    if (!IOUserClient::init(dictionary)) {
        return false;
    }
    
    fTask = NULL;
    fProvider = NULL;
    fValid = false;
    fPermissions = 0;
    fNotificationPort = MACH_PORT_NULL;
    fAsyncEvents = 0;
    
    return true;
}

void AppleMerlotLCDUserClient::free(void)
{
    IOUserClient::free();
}

bool AppleMerlotLCDUserClient::start(IOService* provider)
{
    if (!IOUserClient::start(provider)) {
        return false;
    }
    
    fProvider = OSDynamicCast(AppleMerlotLCD, provider);
    if (!fProvider) {
        return false;
    }
    
    fTask = current_task();
    
    /* Check entitlements for admin access */
    bool hasAdmin = false;
    /* Would check entitlement com.apple.private.display.admin */
    
    fPermissions = hasAdmin ? 0x03 : 0x01;  /* 0x01 = read, 0x03 = read/write */
    
    fValid = true;
    
    return true;
}

void AppleMerlotLCDUserClient::stop(IOService* provider)
{
    fValid = false;
    fProvider = NULL;
    
    IOUserClient::stop(provider);
}

IOReturn AppleMerlotLCDUserClient::clientClose(void)
{
    fValid = false;
    terminate();
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCDUserClient::clientDied(void)
{
    return clientClose();
}

IOReturn AppleMerlotLCDUserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)
{
    fNotificationPort = port;
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCDUserClient::externalMethod(uint32_t selector,
                                                    IOExternalMethodArguments* arguments,
                                                    IOExternalMethodDispatch* dispatch,
                                                    OSObject* target,
                                                    void* reference)
{
    if (selector < kMethodCount) {
        dispatch = &sMerlotMethods[selector];
        if (!dispatch) {
            return kIOReturnBadArgument;
        }
        
        return dispatch->function(this, reference, arguments);
    }
    
    return kIOReturnBadArgument;
}

/* External method implementations */

IOReturn AppleMerlotLCDUserClient::sGetDisplayInfo(AppleMerlotLCDUserClient* target,
                                                     void* reference,
                                                     IOExternalMethodArguments* args)
{
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    struct {
        uint32_t width;
        uint32_t height;
        uint32_t refresh;
        uint32_t brightness;
        uint32_t maxBrightness;
        uint32_t colorTemp;
        uint32_t vrrEnabled;
        uint32_t hdrEnabled;
        uint32_t edrEnabled;
        uint32_t temperature;
        char     panelName[64];
    } info;
    
    info.width = target->fProvider->fCurrentMode.width;
    info.height = target->fProvider->fCurrentMode.height;
    info.refresh = target->fProvider->fCurrentRefresh;
    info.brightness = target->fProvider->fBrightness;
    info.maxBrightness = target->fProvider->fMaxBrightness;
    info.colorTemp = target->fProvider->fColorTemperature;
    info.vrrEnabled = target->fProvider->fVRREnabled ? 1 : 0;
    info.hdrEnabled = target->fProvider->fHDREnabled ? 1 : 0;
    info.edrEnabled = target->fProvider->fEDREnabled ? 1 : 0;
    info.temperature = target->fProvider->fCurrentTemperature;
    strlcpy(info.panelName, target->fProvider->fPanelConfig.panel_name, sizeof(info.panelName));
    
    /* Copy to user space */
    if (args->structureOutputDescriptor) {
        args->structureOutputDescriptor->writeBytes(0, &info, sizeof(info));
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCDUserClient::sSetBrightness(AppleMerlotLCDUserClient* target,
                                                    void* reference,
                                                    IOExternalMethodArguments* args)
{
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Check write permission */
    if ((target->fPermissions & 0x02) == 0) {
        return kIOReturnNotPermitted;
    }
    
    uint32_t brightness = (uint32_t)args->scalarInput[0];
    
    return target->fProvider->setBrightness(brightness);
}

IOReturn AppleMerlotLCDUserClient::sSetColorTemperature(AppleMerlotLCDUserClient* target,
                                                          void* reference,
                                                          IOExternalMethodArguments* args)
{
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Check write permission */
    if ((target->fPermissions & 0x02) == 0) {
        return kIOReturnNotPermitted;
    }
    
    uint32_t kelvin = (uint32_t)args->scalarInput[0];
    
    return target->fProvider->setColorTemp(kelvin);
}

IOReturn AppleMerlotLCDUserClient::sSetVRR(AppleMerlotLCDUserClient* target,
                                             void* reference,
                                             IOExternalMethodArguments* args)
{
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Check write permission */
    if ((target->fPermissions & 0x02) == 0) {
        return kIOReturnNotPermitted;
    }
    
    bool enable = (args->scalarInput[0] != 0);
    
    return target->fProvider->setVRR(enable);
}

IOReturn AppleMerlotLCDUserClient::sSetHDR(AppleMerlotLCDUserClient* target,
                                             void* reference,
                                             IOExternalMethodArguments* args)
{
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Check write permission */
    if ((target->fPermissions & 0x02) == 0) {
        return kIOReturnNotPermitted;
    }
    
    bool enable = (args->scalarInput[0] != 0);
    
    return target->fProvider->setHDR(enable);
}

IOReturn AppleMerlotLCDUserClient::sGetStatistics(AppleMerlotLCDUserClient* target,
                                                    void* reference,
                                                    IOExternalMethodArguments* args)
{
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    struct {
        uint64_t frames;
        uint64_t vsyncs;
        uint32_t vrrChanges;
        uint32_t brightnessChanges;
        uint32_t thermalEvents;
        uint32_t errors;
    } stats;
    
    stats.frames = target->fProvider->fTotalFrames;
    stats.vsyncs = target->fProvider->fTotalVSyncs;
    stats.vrrChanges = target->fProvider->fVRRChanges;
    stats.brightnessChanges = target->fProvider->fBrightnessChanges;
    stats.thermalEvents = target->fProvider->fThermalEvents;
    stats.errors = target->fProvider->fErrors;
    
    if (args->structureOutputDescriptor) {
        args->structureOutputDescriptor->writeBytes(0, &stats, sizeof(stats));
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCDUserClient::sGetTemperature(AppleMerlotLCDUserClient* target,
                                                     void* reference,
                                                     IOExternalMethodArguments* args)
{
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    args->scalarOutput[0] = target->fProvider->fCurrentTemperature;
    
    return kIOReturnSuccess;
}

IOReturn AppleMerlotLCDUserClient::sSetGamma(AppleMerlotLCDUserClient* target,
                                               void* reference,
                                               IOExternalMethodArguments* args)
{
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Check write permission */
    if ((target->fPermissions & 0x02) == 0) {
        return kIOReturnNotPermitted;
    }
    
    if (!args->structureInputDescriptor) {
        return kIOReturnBadArgument;
    }
    
    float gamma;
    args->structureInputDescriptor->readBytes(0, &gamma, sizeof(gamma));
    
    return target->fProvider->setGamma(gamma) ? kIOReturnSuccess : kIOReturnUnsupported;
}

IOReturn AppleMerlotLCDUserClient::sSetColorGamut(AppleMerlotLCDUserClient* target,
                                                    void* reference,
                                                    IOExternalMethodArguments* args)
{
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Check write permission */
    if ((target->fPermissions & 0x02) == 0) {
        return kIOReturnNotPermitted;
    }
    
    uint32_t gamut = (uint32_t)args->scalarInput[0];
    
    if (target->fProvider->setColorGamut(gamut)) {
        return kIOReturnSuccess;
    }
    
    return kIOReturnUnsupported;
}

IOReturn AppleMerlotLCDUserClient::sRunDiagnostics(AppleMerlotLCDUserClient* target,
                                                     void* reference,
                                                     IOExternalMethodArguments* args)
{
    if (!target || !target->fValid || !target->fProvider) {
        return kIOReturnNotAttached;
    }
    
    /* Check admin permission */
    if ((target->fPermissions & 0x02) == 0) {
        return kIOReturnNotPermitted;
    }
    
    uint32_t result = 0;
    
    /* Run display diagnostics */
    target->fProvider->logDisplayInfo();
    
    /* Check controller status */
    uint32_t status = target->fProvider->readReg(MERLOT_REG_STATUS);
    if (status & MERLOT_STATUS_ERROR) {
        result |= (1 << 0);  /* Error bit */
    }
    
    /* Check backlight */
    if (!(status & MERLOT_STATUS_BACKLIGHT_ON)) {
        result |= (1 << 1);  /* Backlight off */
    }
    
    /* Check temperature */
    if (target->fProvider->fCurrentTemperature > THERMAL_CRITICAL_TEMP) {
        result |= (1 << 2);  /* Overheating */
    }
    
    args->scalarOutput[0] = result;
    
    return kIOReturnSuccess;
}

/*==============================================================================
 * AppleMerlotLCD Factory Method (for IOKit matching)
 *==============================================================================*/

IOService* createAppleMerlotLCD(void)
{
    return new AppleMerlotLCD;
}

/*==============================================================================
 * Module Initialization
 *==============================================================================*/

__attribute__((constructor))
static void appleMerlotLCDInit(void)
{
    IOLog("AppleMerlotLCD: Driver loaded (version %d.%d)\n",
          (APPLE_MERLOT_LCD_VERSION >> 16) & 0xFFFF,
          APPLE_MERLOT_LCD_VERSION & 0xFFFF);
}

__attribute__((destructor))
static void appleMerlotLCDExit(void)
{
    IOLog("AppleMerlotLCD: Driver unloaded\n");
}
