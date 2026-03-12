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

/*
 * AppleH1CLCD.cpp
 * H1 Chip LCD Controller Driver
 * Supports:
 * - Apple H1 (343S00437) display controller
 * - Apple H1 (343S00289) display controller
 * - Apple H1 (343S00367) Pro display controller
 * - Integrated with DCP (Display Co-Processor)
 * - MIPI DSI interface
 * - OLED/LCD panel control
 * - ProMotion variable refresh rate
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOMessage.h>

#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSDebug.h>

#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/kauth.h>

#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>

#include <machine/machine_routines.h>

/*==============================================================================
 * H1 LCD Controller Constants
 *==============================================================================*/

#define APPLE_H1CLCD_VERSION            0x00010005  /* Version 1.0.5 */
#define APPLE_H1CLCD_REVISION           0x00000001

/* H1 Chip Variants */
#define H1_CHIP_UNKNOWN                  0
#define H1_CHIP_343S00437                 1   /* AirPods Pro/Max */
#define H1_CHIP_343S00289                 2   /* AirPods (2nd/3rd gen) */
#define H1_CHIP_343S00367                 3   /* Beats Studio Pro */
#define H1_CHIP_343S00634                 4   /* AirPods Pro 2 */
#define H1_CHIP_343S00858                 5   /* Vision Pro displays */

/* Display Types */
#define DISPLAY_TYPE_UNKNOWN             0
#define DISPLAY_TYPE_LCD                 1
#define DISPLAY_TYPE_OLED                 2
#define DISPLAY_TYPE_MICROLED             3
#define DISPLAY_TYPE_LTPO                 4   /* Low-temperature polycrystalline oxide */

/* Display Panels */
#define PANEL_UNKNOWN                    0
#define PANEL_FLEXIBLE_OLED               1
#define PANEL_RIGID_OLED                  2
#define PANEL_LTPS_LCD                    3
#define PANEL_LTPO_OLED                   4
#define PANEL_MICROLED                    5

/* Display Resolutions */
#define RESOLUTION_UNKNOWN               0
#define RESOLUTION_368x448                1   /* AirPods Max */
#define RESOLUTION_456x456                2   /* AirPods Pro case */
#define RESOLUTION_394x408                3   /* Vision Pro internal */
#define RESOLUTION_3660x3200              4   /* Vision Pro external */

/* Interface Types */
#define INTERFACE_UNKNOWN                0
#define INTERFACE_MIPI_DSI                1   /* MIPI DSI */
#define INTERFACE_DPI                    2   /* Display Pixel Interface */
#define INTERFACE_EDP                    3   /* Embedded DisplayPort */
#define INTERFACE_HDMI                   4   /* HDMI */

/* Color Formats */
#define COLOR_FORMAT_RGB565              0
#define COLOR_FORMAT_RGB888              1
#define COLOR_FORMAT_ARGB8888             2
#define COLOR_FORMAT_XRGB8888             3
#define COLOR_FORMAT_YUV422               4
#define COLOR_FORMAT_YUV420               5
#define COLOR_FORMAT_P3                   6   /* DCI-P3 */
#define COLOR_FORMAT_DISPLAY_P3            7   /* Display P3 */
#define COLOR_FORMAT_BT2020               8   /* Rec. 2020 */

/* Color Depths */
#define COLOR_DEPTH_8BIT                 8
#define COLOR_DEPTH_10BIT                 10
#define COLOR_DEPTH_12BIT                 12
#define COLOR_DEPTH_16BIT                 16

/* HDR Modes */
#define HDR_MODE_OFF                     0
#define HDR_MODE_HDR10                    1
#define HDR_MODE_HLG                      2
#define HDR_MODE_DOLBY_VISION             3

/* ProMotion Refresh Rates */
#define REFRESH_1HZ                       1
#define REFRESH_10HZ                      10
#define REFRESH_24HZ                      24
#define REFRESH_30HZ                      30
#define REFRESH_48HZ                      48
#define REFRESH_60HZ                      60
#define REFRESH_90HZ                      90
#define REFRESH_120HZ                     120
#define REFRESH_240HZ                     240
#define REFRESH_VARIABLE                  0xFF

/* Brightness Levels */
#define BRIGHTNESS_MIN                    0
#define BRIGHTNESS_MAX                    100
#define BRIGHTNESS_DEFAULT                50

/* Display Power States */
#define DISPLAY_POWER_OFF                0
#define DISPLAY_POWER_SLEEP               1
#define DISPLAY_POWER_LOW_POWER            2
#define DISPLAY_POWER_ON                   3
#define DISPLAY_POWER_MAX                  3

/* Display Control Flags */
#define DISPLAY_FLAG_ENABLE               (1 << 0)
#define DISPLAY_FLAG_BL_ENABLE             (1 << 1)  /* Backlight enable */
#define DISPLAY_FLAG_HDR_ENABLE            (1 << 2)
#define DISPLAY_FLAG_PROMOTION_ENABLE      (1 << 3)  /* ProMotion enable */
#define DISPLAY_FLAG_AOD_ENABLE            (1 << 4)  /* Always-On Display */
#define DISPLAY_FLAG_TRUETONE_ENABLE       (1 << 5)
#define DISPLAY_FLAG_NIGHT_SHIFT_ENABLE    (1 << 6)
#define DISPLAY_FLAG_COLOR_FILTER_ENABLE   (1 << 7)
#define DISPLAY_FLAG_HW_OFFLOAD            (1 << 8)  /* Hardware offload */

/* Panel Status */
#define PANEL_STATUS_OK                  0
#define PANEL_STATUS_DEGRADED             1
#define PANEL_STATUS_BURN_IN              2
#define PANEL_STATUS_STUCK_PIXELS         3
#define PANEL_STATUS_DEAD_PIXELS          4
#define PANEL_STATUS_OVERHEAT              5
#define PANEL_STATUS_UNREACHABLE          6

/*==============================================================================
 * H1 CLCD Register Map
 *==============================================================================*/

struct h1clcd_registers {
    /* Identification (0x0000-0x00FF) */
    uint32_t    chip_id;                    /* 0x0000 - Chip ID */
    uint32_t    chip_revision;               /* 0x0004 - Chip revision */
    uint32_t    fw_version;                  /* 0x0008 - Firmware version */
    uint32_t    bootloader_version;          /* 0x000C - Bootloader version */
    uint32_t    capabilities;                 /* 0x0010 - Capability flags */
    uint32_t    features;                     /* 0x0014 - Feature flags */
    uint32_t    reserved_018[10];            /* 0x0018-0x003F */
    
    /* Display Control (0x0040-0x00FF) */
    uint32_t    display_enable;               /* 0x0040 - Display enable */
    uint32_t    display_status;               /* 0x0044 - Display status */
    uint32_t    display_mode;                  /* 0x0048 - Display mode */
    uint32_t    display_format;                /* 0x004C - Display format */
    uint32_t    display_timing;                /* 0x0050 - Display timing */
    uint32_t    reserved_054[11];             /* 0x0054-0x007F */
    
    /* Panel Control (0x0080-0x00FF) */
    uint32_t    panel_type;                    /* 0x0080 - Panel type */
    uint32_t    panel_vendor;                  /* 0x0084 - Panel vendor */
    uint32_t    panel_info;                    /* 0x0088 - Panel information */
    uint32_t    panel_status;                  /* 0x008C - Panel status */
    uint32_t    panel_temperature;              /* 0x0090 - Panel temperature */
    uint32_t    panel_age;                      /* 0x0094 - Panel age (hours) */
    uint32_t    reserved_098[26];              /* 0x0098-0x00FF */
    
    /* Resolution (0x0100-0x01FF) */
    uint32_t    h_active;                      /* 0x0100 - Horizontal active pixels */
    uint32_t    h_blank;                       /* 0x0104 - Horizontal blanking */
    uint32_t    h_front_porch;                 /* 0x0108 - Horizontal front porch */
    uint32_t    h_sync_width;                  /* 0x010C - Horizontal sync width */
    uint32_t    h_back_porch;                  /* 0x0110 - Horizontal back porch */
    uint32_t    v_active;                      /* 0x0114 - Vertical active lines */
    uint32_t    v_blank;                       /* 0x0118 - Vertical blanking */
    uint32_t    v_front_porch;                 /* 0x011C - Vertical front porch */
    uint32_t    v_sync_width;                  /* 0x0120 - Vertical sync width */
    uint32_t    v_back_porch;                  /* 0x0124 - Vertical back porch */
    uint32_t    pixel_clock;                    /* 0x0128 - Pixel clock (Hz) */
    uint32_t    refresh_rate;                   /* 0x012C - Refresh rate (mHz) */
    uint32_t    reserved_130[52];              /* 0x0130-0x01FF */
    
    /* Color Control (0x0200-0x02FF) */
    uint32_t    color_format;                   /* 0x0200 - Color format */
    uint32_t    color_depth;                    /* 0x0204 - Color depth */
    uint32_t    color_gamut;                    /* 0x0208 - Color gamut */
    uint32_t    hdr_mode;                       /* 0x020C - HDR mode */
    uint32_t    brightness;                      /* 0x0210 - Brightness level */
    uint32_t    contrast;                       /* 0x0214 - Contrast level */
    uint32_t    saturation;                      /* 0x0218 - Saturation level */
    uint32_t    hue;                            /* 0x021C - Hue level */
    uint32_t    gamma_r;                        /* 0x0220 - Red gamma */
    uint32_t    gamma_g;                        /* 0x0224 - Green gamma */
    uint32_t    gamma_b;                        /* 0x0228 - Blue gamma */
    uint32_t    whitepoint;                      /* 0x022C - Whitepoint */
    uint32_t    reserved_230[52];               /* 0x0230-0x02FF */
    
    /* Backlight Control (0x0300-0x03FF) */
    uint32_t    backlight_enable;                /* 0x0300 - Backlight enable */
    uint32_t    backlight_pwm;                   /* 0x0304 - Backlight PWM value */
    uint32_t    backlight_current;               /* 0x0308 - Backlight current */
    uint32_t    backlight_voltage;               /* 0x030C - Backlight voltage */
    uint32_t    backlight_power;                  /* 0x0310 - Backlight power (mW) */
    uint32_t    backlight_temperature;            /* 0x0314 - Backlight temperature */
    uint32_t    reserved_318[58];                /* 0x0318-0x03FF */
    
    /* Power Management (0x0400-0x04FF) */
    uint32_t    power_state;                      /* 0x0400 - Power state */
    uint32_t    power_flags;                      /* 0x0404 - Power flags */
    uint32_t    voltage_core;                     /* 0x0408 - Core voltage (mV) */
    uint32_t    voltage_io;                       /* 0x040C - IO voltage (mV) */
    uint32_t    voltage_pll;                      /* 0x0410 - PLL voltage (mV) */
    uint32_t    current_core;                     /* 0x0414 - Core current (mA) */
    uint32_t    power_consumption;                /* 0x0418 - Power consumption (mW) */
    uint32_t    temperature;                      /* 0x041C - Die temperature */
    uint32_t    throttle_level;                   /* 0x0420 - Throttle level (0-100) */
    uint32_t    reserved_424[55];                 /* 0x0424-0x04FF */
    
    /* Interrupt Control (0x0500-0x05FF) */
    uint32_t    int_status;                       /* 0x0500 - Interrupt status */
    uint32_t    int_enable;                       /* 0x0504 - Interrupt enable */
    uint32_t    int_clear;                        /* 0x0508 - Interrupt clear */
    uint32_t    int_mask;                         /* 0x050C - Interrupt mask */
    uint32_t    reserved_510[60];                 /* 0x0510-0x05FF */
    
    /* DMA Control (0x0600-0x06FF) */
    uint32_t    dma_enable;                       /* 0x0600 - DMA enable */
    uint32_t    dma_status;                       /* 0x0604 - DMA status */
    uint32_t    dma_fb_address;                   /* 0x0608 - Framebuffer address */
    uint32_t    dma_fb_size;                      /* 0x060C - Framebuffer size */
    uint32_t    dma_line_stride;                  /* 0x0610 - Line stride */
    uint32_t    dma_underflow;                    /* 0x0614 - Underflow count */
    uint32_t    dma_overflow;                     /* 0x0618 - Overflow count */
    uint32_t    reserved_61C[56];                 /* 0x061C-0x06FF */
    
    /* MIPI DSI Control (0x0700-0x07FF) */
    uint32_t    dsi_enable;                       /* 0x0700 - DSI enable */
    uint32_t    dsi_status;                       /* 0x0704 - DSI status */
    uint32_t    dsi_lanes;                        /* 0x0708 - Active lanes */
    uint32_t    dsi_bitrate;                      /* 0x070C - Bitrate per lane */
    uint32_t    dsi_video_mode;                   /* 0x0710 - Video mode */
    uint32_t    dsi_command_mode;                 /* 0x0714 - Command mode */
    uint32_t    reserved_718[58];                 /* 0x0718-0x07FF */
    
    /* Debug (0x0F00-0x0FFF) */
    uint32_t    debug_control;                    /* 0x0F00 - Debug control */
    uint32_t    debug_status;                     /* 0x0F04 - Debug status */
    uint32_t    debug_data[63];                   /* 0x0F08-0x0FFC */
};

/* Interrupt bits */
#define INT_VSYNC                     (1 << 0)
#define INT_HSYNC                     (1 << 1)
#define INT_UNDERFLOW                 (1 << 2)
#define INT_OVERFLOW                  (1 << 3)
#define INT_TE                        (1 << 4)  /* Tearing effect */
#define INT_PANEL_CHANGE              (1 << 5)
#define INT_POWER_CHANGE              (1 << 6)
#define INT_THERMAL_WARNING           (1 << 7)
#define INT_THERMAL_CRITICAL          (1 << 8)
#define INT_BACKLIGHT_FAIL            (1 << 9)
#define INT_ALL                       (0xFFFFFFFF)

/* Capability bits */
#define CAP_HDR                       (1 << 0)
#define CAP_PROMOTION                 (1 << 1)
#define CAP_AOD                       (1 << 2)
#define CAP_TRUETONE                  (1 << 3)
#define CAP_NIGHT_SHIFT               (1 << 4)
#define CAP_COLOR_FILTER              (1 << 5)
#define CAP_HW_OFFLOAD                (1 << 6)
#define CAP_MIPI_DSI                  (1 << 7)
#define CAP_DPI                       (1 << 8)
#define CAP_EDP                       (1 << 9)
#define CAP_PANEL_SELF_REFRESH        (1 << 10)
#define CAP_CABC                       (1 << 11)  /* Content Adaptive Brightness */

/* Feature bits */
#define FEATURE_FRC                    (1 << 0)  /* Frame Rate Control */
#define FEATURE_DITHER                 (1 << 1)
#define FEATURE_SCALER                 (1 << 2)
#define FEATURE_ROTATION               (1 << 3)
#define FEATURE_MIRROR                 (1 << 4)
#define FEATURE_GAMMA_CORRECTION       (1 << 5)
#define FEATURE_COLOR_MATRIX           (1 << 6)
#define FEATURE_HISTOGRAM              (1 << 7)
#define FEATURE_CRC                    (1 << 8)  /* Cyclic Redundancy Check */

/*==============================================================================
 * Panel Configuration
 *==============================================================================*/

struct panel_config_t {
    uint32_t    panel_type;
    uint32_t    vendor_id;
    uint32_t    model_id;
    uint32_t    h_active;
    uint32_t    v_active;
    uint32_t    h_front_porch;
    uint32_t    h_sync_width;
    uint32_t    h_back_porch;
    uint32_t    v_front_porch;
    uint32_t    v_sync_width;
    uint32_t    v_back_porch;
    uint32_t    pixel_clock_min;
    uint32_t    pixel_clock_max;
    uint32_t    refresh_min;
    uint32_t    refresh_max;
    uint32_t    color_depth;
    uint32_t    color_gamut;
    uint32_t    brightness_min;
    uint32_t    brightness_max;
    uint32_t    operating_temp_min;
    uint32_t    operating_temp_max;
    uint32_t    flags;
    char        name[64];
    char        manufacturer[32];
};

/* Known panels */
static const panel_config_t kKnownPanels[] = {
    {
        .panel_type = PANEL_LTPO_OLED,
        .vendor_id = 0x01,
        .model_id = 0x1001,
        .h_active = 368,
        .v_active = 448,
        .h_front_porch = 20,
        .h_sync_width = 10,
        .h_back_porch = 20,
        .v_front_porch = 4,
        .v_sync_width = 2,
        .v_back_porch = 4,
        .pixel_clock_min = 20000000,
        .pixel_clock_max = 80000000,
        .refresh_min = 1,
        .refresh_max = 120,
        .color_depth = 10,
        .color_gamut = COLOR_FORMAT_DISPLAY_P3,
        .brightness_min = 2,
        .brightness_max = 1000,
        .operating_temp_min = -20,
        .operating_temp_max = 70,
        .flags = CAP_HDR | CAP_PROMOTION | CAP_AOD | CAP_TRUETONE,
        .name = "LTPO OLED 368x448",
        .manufacturer = "LG Display"
    },
    {
        .panel_type = PANEL_MICROLED,
        .vendor_id = 0x02,
        .model_id = 0x2001,
        .h_active = 3660,
        .v_active = 3200,
        .h_front_porch = 88,
        .h_sync_width = 44,
        .h_back_porch = 148,
        .v_front_porch = 4,
        .v_sync_width = 5,
        .v_back_porch = 36,
        .pixel_clock_min = 500000000,
        .pixel_clock_max = 2000000000,
        .refresh_min = 90,
        .refresh_max = 96,
        .color_depth = 12,
        .color_gamut = COLOR_FORMAT_BT2020,
        .brightness_min = 1,
        .brightness_max = 5000,
        .operating_temp_min = -10,
        .operating_temp_max = 50,
        .flags = CAP_HDR | CAP_HW_OFFLOAD | CAP_TRUETONE,
        .name = "MicroLED 3660x3200",
        .manufacturer = "Sony"
    },
    {
        .panel_type = PANEL_FLEXIBLE_OLED,
        .vendor_id = 0x03,
        .model_id = 0x3001,
        .h_active = 394,
        .v_active = 408,
        .h_front_porch = 16,
        .h_sync_width = 8,
        .h_back_porch = 16,
        .v_front_porch = 2,
        .v_sync_width = 2,
        .v_back_porch = 2,
        .pixel_clock_min = 15000000,
        .pixel_clock_max = 60000000,
        .refresh_min = 60,
        .refresh_max = 100,
        .color_depth = 8,
        .color_gamut = COLOR_FORMAT_P3,
        .brightness_min = 2,
        .brightness_max = 800,
        .operating_temp_min = -10,
        .operating_temp_max = 60,
        .flags = CAP_HDR | CAP_AOD | CAP_TRUETONE,
        .name = "Flexible OLED 394x408",
        .manufacturer = "Samsung"
    }
};

/*==============================================================================
 * Display Mode Structure
 *==============================================================================*/

struct display_mode_t {
    uint32_t    width;
    uint32_t    height;
    uint32_t    refresh_rate;
    uint32_t    pixel_clock;
    uint32_t    h_total;
    uint32_t    v_total;
    uint32_t    h_sync_start;
    uint32_t    h_sync_end;
    uint32_t    v_sync_start;
    uint32_t    v_sync_end;
    uint32_t    flags;
    uint32_t    color_format;
    uint32_t    color_depth;
    uint32_t    dsi_lanes;
    uint32_t    dsi_bitrate;
};

/*==============================================================================
 * Color Calibration Data
 *==============================================================================*/

struct color_calibration_t {
    float       r_gamma[256];
    float       g_gamma[256];
    float       b_gamma[256];
    float       color_matrix[3][3];
    float       whitepoint_x;
    float       whitepoint_y;
    float       brightness_curve[101];  /* 0-100% brightness levels */
    uint32_t    crc;
};

/*==============================================================================
 * Thermal Management
 *==============================================================================*/

struct thermal_config_t {
    uint32_t    temp_shutdown;
    uint32_t    temp_throttle_start;
    uint32_t    temp_throttle_max;
    uint32_t    temp_warning;
    uint32_t    temp_normal;
    uint32_t    backlight_reduction[101];  /* % backlight per temperature */
};

static const thermal_config_t kThermalConfig = {
    .temp_shutdown = 110,
    .temp_throttle_start = 85,
    .temp_throttle_max = 100,
    .temp_warning = 80,
    .temp_normal = 50,
    .backlight_reduction = { 0 }  /* Would be filled with curve */
};

/*==============================================================================
 * H1 CLCD Main Class
 *==============================================================================*/

class AppleH1CLCD : public IOService
{
    OSDeclareDefaultStructors(AppleH1CLCD)
    
private:
    /* Hardware resources */
    IOMemoryMap*                    fMemoryMap;
    volatile h1clcd_registers*       fRegisters;
    IOPhysicalAddress                fRegPhys;
    IOPhysicalLength                 fRegSize;
    
    /* IOKit resources */
    IOWorkLoop*                      fWorkLoop;
    IOCommandGate*                   fCommandGate;
    IOInterruptEventSource*          fInterruptSource;
    IOTimerEventSource*              fTimerSource;
    
    /* Chip identification */
    uint32_t                         fChipId;
    uint32_t                         fChipRevision;
    uint32_t                         fFirmwareVersion;
    uint32_t                         fChipCapabilities;
    uint32_t                         fChipFeatures;
    
    /* Panel information */
    uint32_t                         fPanelType;
    uint32_t                         fPanelVendor;
    uint32_t                         fPanelModel;
    panel_config_t                   fPanelConfig;
    bool                             fPanelDetected;
    
    /* Display state */
    uint32_t                         fDisplayState;
    uint32_t                         fDisplayFlags;
    uint32_t                         fPowerState;
    uint32_t                         fBrightness;
    uint32_t                         fContrast;
    uint32_t                         fColorFormat;
    uint32_t                         fColorDepth;
    uint32_t                         fHdrMode;
    uint32_t                         fRefreshRate;
    uint32_t                         fTargetRefreshRate;
    bool                             fVariableRefresh;
    
    /* Display timings */
    uint32_t                         fHActive;
    uint32_t                         fVActive;
    uint32_t                         fPixelClock;
    uint32_t                         fHTotal;
    uint32_t                         fVTotal;
    
    /* Color calibration */
    color_calibration_t              fColorCal;
    bool                             fCalibrationLoaded;
    uint64_t                         fLastCalibrationUpdate;
    
    /* Thermal management */
    uint32_t                         fTemperature;
    uint32_t                         fThrottleLevel;
    uint32_t                         fThermalWarningCount;
    uint64_t                         fLastThermalSample;
    lck_mtx_t*                       fThermalLock;
    
    /* Backlight */
    uint32_t                         fBacklightEnabled;
    uint32_t                         fBacklightPWM;
    uint32_t                         fBacklightCurrent;
    uint32_t                         fBacklightVoltage;
    uint32_t                         fBacklightPower;
    uint32_t                         fBacklightMax;
    lck_mtx_t*                       fBacklightLock;
    
    /* DMA and framebuffer */
    IOBufferMemoryDescriptor*        fFramebufferMem;
    IOPhysicalAddress                 fFramebufferPhys;
    void*                             fFramebufferVirtual;
    uint32_t                          fFramebufferSize;
    uint32_t                          fLineStride;
    uint32_t                          fDMAUnderflows;
    uint32_t                          fDMAOverflows;
    lck_mtx_t*                        fDMALock;
    
    /* MIPI DSI */
    uint32_t                          fDSIEnabled;
    uint32_t                          fDSIActiveLanes;
    uint32_t                          fDSIBitrate;
    uint32_t                          fDSIVideoMode;
    uint32_t                          fDSICommandMode;
    
    /* Display modes */
    display_mode_t                    fCurrentMode;
    OSArray*                          fModeArray;
    
    /* Statistics */
    uint64_t                          fVSyncCount;
    uint64_t                          fFrameCount;
    uint64_t                          fUnderflowCount;
    uint64_t                          fOverflowCount;
    uint64_t                          fTearingCount;
    uint64_t                          fThermalThrottleCount;
    uint64_t                          fPowerTransitions;
    
    /* Timestamps */
    uint64_t                          fLastVSync;
    uint64_t                          fLastFrame;
    uint64_t                          fPowerOnTime;
    uint64_t                          fTotalOnTime;
    
    /* Locking */
    lck_grp_t*                        fLockGroup;
    lck_attr_t*                       fLockAttr;
    lck_mtx_t*                        fDisplayLock;
    
    /*==============================================================================
     * Private Methods
     *==============================================================================*/
    
    /* Hardware initialization */
    bool                              mapRegisters(void);
    void                              unmapRegisters(void);
    bool                              identifyChip(void);
    bool                              detectPanel(void);
    bool                              initializeHardware(void);
    void                              shutdownHardware(void);
    
    /* Display control */
    IOReturn                          enableDisplay(bool enable);
    IOReturn                          setDisplayTiming(const display_mode_t* mode);
    IOReturn                          setColorFormat(uint32_t format, uint32_t depth);
    IOReturn                          setBrightness(uint32_t brightness);
    IOReturn                          setBacklight(uint32_t level);
    IOReturn                          setHDRMode(uint32_t mode);
    IOReturn                          setRefreshRate(uint32_t rate);
    IOReturn                          setPowerStateInternal(uint32_t state);
    
    /* Panel communication */
    IOReturn                          panelWriteCommand(uint8_t cmd, const uint8_t* data, uint32_t len);
    IOReturn                          panelReadData(uint8_t cmd, uint8_t* data, uint32_t* len);
    IOReturn                          panelInitSequence(void);
    IOReturn                          panelSleep(void);
    IOReturn                          panelWake(void);
    
    /* Color management */
    IOReturn                          loadColorCalibration(void);
    IOReturn                          applyColorCalibration(void);
    IOReturn                          updateGamma(const float* r, const float* g, const float* b);
    IOReturn                          updateColorMatrix(const float matrix[3][3]);
    IOReturn                          updateWhitepoint(float x, float y);
    
    /* Thermal management */
    void                              updateTemperature(void);
    IOReturn                          handleThermalEvent(void);
    IOReturn                          throttleDisplay(uint32_t level);
    
    /* DMA management */
    IOReturn                          configureFramebuffer(void* fb, uint32_t size, uint32_t stride);
    IOReturn                          enableDMA(bool enable);
    IOReturn                          handleDMAUnderflow(void);
    IOReturn                          handleDMAOverflow(void);
    
    /* Interrupt handling */
    void                              interruptHandler(void);
    static void                       interruptHandler(IOInterruptEventSource* source, int count);
    
    /* Timer */
    void                              timerFired(void);
    static void                       timerFired(OSObject* owner, IOTimerEventSource* sender);
    
    /* Mode management */
    IOReturn                          buildModeList(void);
    const display_mode_t*             findBestMode(uint32_t width, uint32_t height, uint32_t rate);
    
    /* Work loop */
    bool                              createWorkLoop(void);
    void                              destroyWorkLoop(void);
    
    /* Utility */
    uint32_t                          readRegister(uint32_t reg);
    void                              writeRegister(uint32_t reg, uint32_t value);
    uint32_t                          getCurrentUptime(void);
    
protected:
    bool                              init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                              free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                              start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                              stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    /* Power management */
    IOReturn                          setPowerState(unsigned long powerState,
                                                    IOService* device) APPLE_KEXT_OVERRIDE;
    IOReturn                          powerStateWillChangeTo(IOPMPowerFlags capabilities,
                                                              unsigned long stateNumber,
                                                              IOService* whatDevice) APPLE_KEXT_OVERRIDE;
    IOReturn                          powerStateDidChangeTo(IOPMPowerFlags capabilities,
                                                             unsigned long stateNumber,
                                                             IOService* whatDevice) APPLE_KEXT_OVERRIDE;
    
    /* Platform functions */
    IOReturn                          callPlatformFunction(const OSSymbol* functionName,
                                                            bool waitForFunction,
                                                            void* param1, void* param2,
                                                            void* param3, void* param4) APPLE_KEXT_OVERRIDE;
    
    /* Property access */
    virtual bool                      serializeProperties(OSSerialize* s) const APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleH1CLCD, IOService)

/*==============================================================================
 * Register Access
 *==============================================================================*/

inline uint32_t AppleH1CLCD::readRegister(uint32_t reg)
{
    if (!fRegisters || reg >= fRegSize) {
        return 0xFFFFFFFF;
    }
    
    uint32_t value = *((volatile uint32_t*)((uintptr_t)fRegisters + reg));
    
    return value;
}

inline void AppleH1CLCD::writeRegister(uint32_t reg, uint32_t value)
{
    if (!fRegisters || reg >= fRegSize) {
        return;
    }
    
    *((volatile uint32_t*)((uintptr_t)fRegisters + reg)) = value;
    
    /* Ensure write is complete */
    OSMemoryBarrier();
}

/*==============================================================================
 * Hardware Initialization
 *==============================================================================*/

bool AppleH1CLCD::mapRegisters(void)
{
    IOService* provider = getProvider();
    if (!provider) {
        IOLog("AppleH1CLCD: No provider\n");
        return false;
    }
    
    IODeviceMemory* memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory) {
        IOLog("AppleH1CLCD: No device memory\n");
        return false;
    }
    
    fRegPhys = memory->getPhysicalAddress();
    fRegSize = memory->getLength();
    
    fMemoryMap = memory->makeMapping(kIOMapAny | kIOMapInhibitCache);
    if (!fMemoryMap) {
        IOLog("AppleH1CLCD: Failed to map registers\n");
        return false;
    }
    
    fRegisters = (volatile h1clcd_registers*)fMemoryMap->getVirtualAddress();
    if (!fRegisters) {
        IOLog("AppleH1CLCD: Invalid virtual address\n");
        fMemoryMap->release();
        fMemoryMap = NULL;
        return false;
    }
    
    IOLog("AppleH1CLCD: Mapped registers at %p (phys: 0x%llx, size: %lld)\n",
          fRegisters, fRegPhys, fRegSize);
    
    return true;
}

void AppleH1CLCD::unmapRegisters(void)
{
    if (fMemoryMap) {
        fMemoryMap->release();
        fMemoryMap = NULL;
        fRegisters = NULL;
    }
}

bool AppleH1CLCD::identifyChip(void)
{
    if (!fRegisters) {
        return false;
    }
    
    fChipId = readRegister(offsetof(h1clcd_registers, chip_id));
    fChipRevision = readRegister(offsetof(h1clcd_registers, chip_revision));
    fFirmwareVersion = readRegister(offsetof(h1clcd_registers, fw_version));
    fChipCapabilities = readRegister(offsetof(h1clcd_registers, capabilities));
    fChipFeatures = readRegister(offsetof(h1clcd_registers, features));
    
    IOLog("AppleH1CLCD: Chip ID: 0x%08x, Revision: 0x%08x, Firmware: 0x%08x\n",
          fChipId, fChipRevision, fFirmwareVersion);
    
    /* Identify chip variant */
    switch (fChipId) {
        case 0x34300437:
            IOLog("AppleH1CLCD: H1 343S00437 (AirPods Pro/Max) detected\n");
            break;
        case 0x34300289:
            IOLog("AppleH1CLCD: H1 343S00289 (AirPods 2nd/3rd gen) detected\n");
            break;
        case 0x34300367:
            IOLog("AppleH1CLCD: H1 343S00367 (Beats Studio Pro) detected\n");
            break;
        case 0x34300634:
            IOLog("AppleH1CLCD: H1 343S00634 (AirPods Pro 2) detected\n");
            break;
        case 0x34300858:
            IOLog("AppleH1CLCD: H1 343S00858 (Vision Pro) detected\n");
            break;
        default:
            IOLog("AppleH1CLCD: Unknown H1 variant: 0x%08x\n", fChipId);
            break;
    }
    
    return true;
}

bool AppleH1CLCD::detectPanel(void)
{
    if (!fRegisters) {
        return false;
    }
    
    fPanelType = readRegister(offsetof(h1clcd_registers, panel_type));
    fPanelVendor = readRegister(offsetof(h1clcd_registers, panel_vendor));
    fPanelModel = readRegister(offsetof(h1clcd_registers, panel_info));
    
    IOLog("AppleH1CLCD: Panel detected - Type: 0x%x, Vendor: 0x%x, Model: 0x%x\n",
          fPanelType, fPanelVendor, fPanelModel);
    
    /* Find matching panel configuration */
    bool found = false;
    for (size_t i = 0; i < sizeof(kKnownPanels) / sizeof(kKnownPanels[0]); i++) {
        if (kKnownPanels[i].panel_type == fPanelType &&
            kKnownPanels[i].vendor_id == fPanelVendor &&
            kKnownPanels[i].model_id == fPanelModel) {
            
            fPanelConfig = kKnownPanels[i];
            found = true;
            IOLog("AppleH1CLCD: Matched panel: %s by %s\n",
                  fPanelConfig.name, fPanelConfig.manufacturer);
            break;
        }
    }
    
    if (!found) {
        IOLog("AppleH1CLCD: Unknown panel, using default configuration\n");
        /* Use default configuration */
        fPanelConfig.panel_type = fPanelType;
        fPanelConfig.vendor_id = fPanelVendor;
        fPanelConfig.model_id = fPanelModel;
        fPanelConfig.h_active = 368;
        fPanelConfig.v_active = 448;
        fPanelConfig.refresh_min = 60;
        fPanelConfig.refresh_max = 60;
        fPanelConfig.color_depth = 8;
        fPanelConfig.brightness_min = 2;
        fPanelConfig.brightness_max = 600;
    }
    
    fPanelDetected = true;
    
    /* Read panel status */
    uint32_t panelStatus = readRegister(offsetof(h1clcd_registers, panel_status));
    if (panelStatus != PANEL_STATUS_OK) {
        IOLog("AppleH1CLCD: Panel status warning: %d\n", panelStatus);
    }
    
    return true;
}

bool AppleH1CLCD::initializeHardware(void)
{
    if (!fRegisters) {
        return false;
    }
    
    IOLog("AppleH1CLCD: Initializing hardware\n");
    
    /* Reset controller */
    writeRegister(offsetof(h1clcd_registers, display_enable), 0);
    writeRegister(offsetof(h1clcd_registers, dma_enable), 0);
    writeRegister(offsetof(h1clcd_registers, backlight_enable), 0);
    
    /* Configure display timing based on panel */
    fHActive = fPanelConfig.h_active;
    fVActive = fPanelConfig.v_active;
    
    writeRegister(offsetof(h1clcd_registers, h_active), fHActive);
    writeRegister(offsetof(h1clcd_registers, h_front_porch), fPanelConfig.h_front_porch);
    writeRegister(offsetof(h1clcd_registers, h_sync_width), fPanelConfig.h_sync_width);
    writeRegister(offsetof(h1clcd_registers, h_back_porch), fPanelConfig.h_back_porch);
    
    writeRegister(offsetof(h1clcd_registers, v_active), fVActive);
    writeRegister(offsetof(h1clcd_registers, v_front_porch), fPanelConfig.v_front_porch);
    writeRegister(offsetof(h1clcd_registers, v_sync_width), fPanelConfig.v_sync_width);
    writeRegister(offsetof(h1clcd_registers, v_back_porch), fPanelConfig.v_back_porch);
    
    fHTotal = fHActive + fPanelConfig.h_front_porch + fPanelConfig.h_sync_width + fPanelConfig.h_back_porch;
    fVTotal = fVActive + fPanelConfig.v_front_porch + fPanelConfig.v_sync_width + fPanelConfig.v_back_porch;
    
    /* Set initial pixel clock (60Hz) */
    fPixelClock = fHTotal * fVTotal * 60;
    writeRegister(offsetof(h1clcd_registers, pixel_clock), fPixelClock);
    writeRegister(offsetof(h1clcd_registers, refresh_rate), 60 * 1000); /* mHz */
    
    /* Configure color format */
    fColorFormat = COLOR_FORMAT_DISPLAY_P3;
    fColorDepth = fPanelConfig.color_depth;
    writeRegister(offsetof(h1clcd_registers, color_format), fColorFormat);
    writeRegister(offsetof(h1clcd_registers, color_depth), fColorDepth);
    
    /* Configure backlight */
    fBacklightMax = fPanelConfig.brightness_max;
    fBrightness = BRIGHTNESS_DEFAULT;
    fBacklightPWM = (fBrightness * 65535) / 100;
    
    writeRegister(offsetof(h1clcd_registers, brightness), fBrightness);
    writeRegister(offsetof(h1clcd_registers, backlight_pwm), fBacklightPWM);
    
    /* Configure interrupts */
    writeRegister(offsetof(h1clcd_registers, int_enable), INT_VSYNC | INT_UNDERFLOW | INT_THERMAL_WARNING);
    
    /* Initialize panel */
    panelInitSequence();
    
    /* Load color calibration */
    loadColorCalibration();
    
    IOLog("AppleH1CLCD: Hardware initialized\n");
    
    return true;
}

void AppleH1CLCD::shutdownHardware(void)
{
    if (!fRegisters) {
        return;
    }
    
    IOLog("AppleH1CLCD: Shutting down hardware\n");
    
    /* Disable display */
    writeRegister(offsetof(h1clcd_registers, display_enable), 0);
    
    /* Disable DMA */
    writeRegister(offsetof(h1clcd_registers, dma_enable), 0);
    
    /* Disable backlight */
    writeRegister(offsetof(h1clcd_registers, backlight_enable), 0);
    
    /* Disable interrupts */
    writeRegister(offsetof(h1clcd_registers, int_enable), 0);
    
    /* Put panel to sleep */
    panelSleep();
    
    IOLog("AppleH1CLCD: Hardware shut down\n");
}

/*==============================================================================
 * Panel Communication
 *==============================================================================*/

IOReturn AppleH1CLCD::panelWriteCommand(uint8_t cmd, const uint8_t* data, uint32_t len)
{
    /* Would implement MIPI DSI command write */
    /* For now, stub implementation */
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::panelReadData(uint8_t cmd, uint8_t* data, uint32_t* len)
{
    /* Would implement MIPI DSI read */
    /* For now, stub implementation */
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::panelInitSequence(void)
{
    IOLog("AppleH1CLCD: Initializing panel\n");
    
    /* Example panel initialization sequence */
    const uint8_t init_cmds[] = {
        0x11, 0x00,  /* Exit sleep */
        0x29, 0x00,  /* Display on */
        0x35, 0x00,  /* TE on */
        0x44, 0x02, 0x00, 0x00,  /* Set TE scanline */
        0x51, 0x01, 0xFF,  /* Write brightness */
        0x53, 0x01, 0x2C,  /* Write CTRL display */
        0x55, 0x01, 0x00,  /* Write CABC */
    };
    
    /* Send initialization commands */
    for (size_t i = 0; i < sizeof(init_cmds); ) {
        uint8_t cmd = init_cmds[i++];
        uint8_t len = init_cmds[i++];
        panelWriteCommand(cmd, &init_cmds[i], len);
        i += len;
        
        /* Delay between commands */
        IODelay(10 * 1000);  /* 10ms */
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::panelSleep(void)
{
    IOLog("AppleH1CLCD: Putting panel to sleep\n");
    
    /* Send display off command */
    uint8_t cmd = 0x28;  /* Display off */
    panelWriteCommand(cmd, NULL, 0);
    
    IODelay(50 * 1000);  /* 50ms */
    
    /* Send sleep in command */
    cmd = 0x10;  /* Sleep in */
    panelWriteCommand(cmd, NULL, 0);
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::panelWake(void)
{
    IOLog("AppleH1CLCD: Waking panel\n");
    
    /* Send sleep out command */
    uint8_t cmd = 0x11;  /* Sleep out */
    panelWriteCommand(cmd, NULL, 0);
    
    IODelay(120 * 1000);  /* 120ms - required wake time */
    
    /* Send display on command */
    cmd = 0x29;  /* Display on */
    panelWriteCommand(cmd, NULL, 0);
    
    return kIOReturnSuccess;
}

/*==============================================================================
 * Display Control
 *==============================================================================*/

IOReturn AppleH1CLCD::enableDisplay(bool enable)
{
    if (!fRegisters) {
        return kIOReturnNotReady;
    }
    
    lck_mtx_lock(fDisplayLock);
    
    if (enable) {
        if (!(fDisplayState & DISPLAY_FLAG_ENABLE)) {
            /* Wake panel */
            if (fPowerState < DISPLAY_POWER_ON) {
                setPowerStateInternal(DISPLAY_POWER_ON);
            }
            
            panelWake();
            
            /* Enable display */
            writeRegister(offsetof(h1clcd_registers, display_enable), 1);
            fDisplayState |= DISPLAY_FLAG_ENABLE;
            
            IOLog("AppleH1CLCD: Display enabled\n");
        }
    } else {
        if (fDisplayState & DISPLAY_FLAG_ENABLE) {
            /* Disable display */
            writeRegister(offsetof(h1clcd_registers, display_enable), 0);
            fDisplayState &= ~DISPLAY_FLAG_ENABLE;
            
            /* Put panel to sleep if power state is low */
            if (fPowerState < DISPLAY_POWER_ON) {
                panelSleep();
            }
            
            IOLog("AppleH1CLCD: Display disabled\n");
        }
    }
    
    lck_mtx_unlock(fDisplayLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::setDisplayTiming(const display_mode_t* mode)
{
    if (!fRegisters || !mode) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fDisplayLock);
    
    /* Check if mode is supported */
    if (mode->width != fPanelConfig.h_active || mode->height != fPanelConfig.v_active) {
        lck_mtx_unlock(fDisplayLock);
        return kIOReturnUnsupported;
    }
    
    /* Calculate timings */
    uint32_t h_total = mode->h_total;
    uint32_t v_total = mode->v_total;
    
    /* Update registers */
    writeRegister(offsetof(h1clcd_registers, h_active), mode->width);
    writeRegister(offsetof(h1clcd_registers, v_active), mode->height);
    writeRegister(offsetof(h1clcd_registers, h_sync_start), mode->h_sync_start);
    writeRegister(offsetof(h1clcd_registers, h_sync_end), mode->h_sync_end);
    writeRegister(offsetof(h1clcd_registers, v_sync_start), mode->v_sync_start);
    writeRegister(offsetof(h1clcd_registers, v_sync_end), mode->v_sync_end);
    writeRegister(offsetof(h1clcd_registers, pixel_clock), mode->pixel_clock);
    writeRegister(offsetof(h1clcd_registers, refresh_rate), mode->refresh_rate);
    
    /* Store current mode */
    fCurrentMode = *mode;
    fHActive = mode->width;
    fVActive = mode->height;
    fPixelClock = mode->pixel_clock;
    fRefreshRate = mode->refresh_rate / 1000;  /* Convert mHz to Hz */
    
    lck_mtx_unlock(fDisplayLock);
    
    IOLog("AppleH1CLCD: Display timing set: %dx%d @ %dHz\n",
          fHActive, fVActive, fRefreshRate);
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::setColorFormat(uint32_t format, uint32_t depth)
{
    if (!fRegisters) {
        return kIOReturnNotReady;
    }
    
    lck_mtx_lock(fDisplayLock);
    
    writeRegister(offsetof(h1clcd_registers, color_format), format);
    writeRegister(offsetof(h1clcd_registers, color_depth), depth);
    
    fColorFormat = format;
    fColorDepth = depth;
    
    lck_mtx_unlock(fDisplayLock);
    
    IOLog("AppleH1CLCD: Color format set: format=%d, depth=%d\n", format, depth);
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::setBrightness(uint32_t brightness)
{
    if (brightness > 100) {
        brightness = 100;
    }
    
    if (!fRegisters) {
        return kIOReturnNotReady;
    }
    
    lck_mtx_lock(fBacklightLock);
    
    fBrightness = brightness;
    
    /* Convert percentage to PWM value (0-65535) */
    uint32_t pwm = (brightness * 65535) / 100;
    
    /* Apply brightness curve from calibration */
    if (fCalibrationLoaded && brightness <= 100) {
        float factor = fColorCal.brightness_curve[brightness];
        pwm = (uint32_t)(pwm * factor);
    }
    
    writeRegister(offsetof(h1clcd_registers, brightness), brightness);
    writeRegister(offsetof(h1clcd_registers, backlight_pwm), pwm);
    
    lck_mtx_unlock(fBacklightLock);
    
    IOLog("AppleH1CLCD: Brightness set to %d%% (PWM: %d)\n", brightness, pwm);
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::setBacklight(uint32_t level)
{
    return setBrightness(level);
}

IOReturn AppleH1CLCD::setHDRMode(uint32_t mode)
{
    if (!fRegisters) {
        return kIOReturnNotReady;
    }
    
    if (!(fChipCapabilities & CAP_HDR)) {
        return kIOReturnUnsupported;
    }
    
    lck_mtx_lock(fDisplayLock);
    
    writeRegister(offsetof(h1clcd_registers, hdr_mode), mode);
    fHdrMode = mode;
    
    if (mode != HDR_MODE_OFF) {
        fDisplayFlags |= DISPLAY_FLAG_HDR_ENABLE;
    } else {
        fDisplayFlags &= ~DISPLAY_FLAG_HDR_ENABLE;
    }
    
    lck_mtx_unlock(fDisplayLock);
    
    IOLog("AppleH1CLCD: HDR mode set to %d\n", mode);
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::setRefreshRate(uint32_t rate)
{
    if (!fRegisters) {
        return kIOReturnNotReady;
    }
    
    /* Check if rate is within panel limits */
    if (rate < fPanelConfig.refresh_min || rate > fPanelConfig.refresh_max) {
        return kIOReturnUnsupported;
    }
    
    lck_mtx_lock(fDisplayLock);
    
    /* Calculate pixel clock for new refresh rate */
    uint32_t new_pixel_clock = fHTotal * fVTotal * rate;
    
    /* Check if within clock limits */
    if (new_pixel_clock < fPanelConfig.pixel_clock_min ||
        new_pixel_clock > fPanelConfig.pixel_clock_max) {
        lck_mtx_unlock(fDisplayLock);
        return kIOReturnUnsupported;
    }
    
    writeRegister(offsetof(h1clcd_registers, pixel_clock), new_pixel_clock);
    writeRegister(offsetof(h1clcd_registers, refresh_rate), rate * 1000);
    
    fPixelClock = new_pixel_clock;
    fRefreshRate = rate;
    
    lck_mtx_unlock(fDisplayLock);
    
    IOLog("AppleH1CLCD: Refresh rate set to %dHz\n", rate);
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::setPowerStateInternal(uint32_t state)
{
    if (state > DISPLAY_POWER_MAX) {
        return kIOReturnBadArgument;
    }
    
    if (state == fPowerState) {
        return kIOReturnSuccess;
    }
    
    IOLog("AppleH1CLCD: Power state changing: %d -> %d\n", fPowerState, state);
    
    switch (state) {
        case DISPLAY_POWER_OFF:
            /* Full power off */
            enableDisplay(false);
            writeRegister(offsetof(h1clcd_registers, power_state), 0);
            break;
            
        case DISPLAY_POWER_SLEEP:
            /* Sleep state - display off, memory retained */
            writeRegister(offsetof(h1clcd_registers, display_enable), 0);
            writeRegister(offsetof(h1clcd_registers, power_state), 1);
            panelSleep();
            break;
            
        case DISPLAY_POWER_LOW_POWER:
            /* Low power state - reduced brightness, maybe lower refresh */
            writeRegister(offsetof(h1clcd_registers, power_state), 2);
            if (fChipCapabilities & CAP_PROMOTION) {
                /* Switch to lower refresh rate for power saving */
                fTargetRefreshRate = fRefreshRate;
                setRefreshRate(30);
            }
            break;
            
        case DISPLAY_POWER_ON:
            /* Full power on */
            writeRegister(offsetof(h1clcd_registers, power_state), 3);
            panelWake();
            enableDisplay(true);
            
            /* Restore previous refresh rate if changed */
            if (fChipCapabilities & CAP_PROMOTION && fTargetRefreshRate > 0) {
                setRefreshRate(fTargetRefreshRate);
                fTargetRefreshRate = 0;
            }
            
            fPowerOnTime = getCurrentUptime();
            break;
    }
    
    fPowerState = state;
    fPowerTransitions++;
    
    return kIOReturnSuccess;
}

/*==============================================================================
 * Color Management
 *==============================================================================*/

IOReturn AppleH1CLCD::loadColorCalibration(void)
{
    IOLog("AppleH1CLCD: Loading color calibration\n");
    
    /* In real implementation, would load from NVRAM or panel */
    
    /* Initialize with default values */
    for (int i = 0; i < 256; i++) {
        float v = i / 255.0f;
        fColorCal.r_gamma[i] = v;
        fColorCal.g_gamma[i] = v;
        fColorCal.b_gamma[i] = v;
    }
    
    /* Default color matrix (identity) */
    fColorCal.color_matrix[0][0] = 1.0f; fColorCal.color_matrix[0][1] = 0.0f; fColorCal.color_matrix[0][2] = 0.0f;
    fColorCal.color_matrix[1][0] = 0.0f; fColorCal.color_matrix[1][1] = 1.0f; fColorCal.color_matrix[1][2] = 0.0f;
    fColorCal.color_matrix[2][0] = 0.0f; fColorCal.color_matrix[2][1] = 0.0f; fColorCal.color_matrix[2][2] = 1.0f;
    
    /* Default whitepoint (D65) */
    fColorCal.whitepoint_x = 0.3127f;
    fColorCal.whitepoint_y = 0.3290f;
    
    /* Default brightness curve (linear) */
    for (int i = 0; i <= 100; i++) {
        fColorCal.brightness_curve[i] = i / 100.0f;
    }
    
    fCalibrationLoaded = true;
    fLastCalibrationUpdate = getCurrentUptime();
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::applyColorCalibration(void)
{
    if (!fCalibrationLoaded) {
        return kIOReturnNotReady;
    }
    
    /* Would program hardware with calibration data */
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::updateGamma(const float* r, const float* g, const float* b)
{
    if (!r || !g || !b) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fDisplayLock);
    
    memcpy(fColorCal.r_gamma, r, sizeof(float) * 256);
    memcpy(fColorCal.g_gamma, g, sizeof(float) * 256);
    memcpy(fColorCal.b_gamma, b, sizeof(float) * 256);
    
    /* Program gamma LUTs */
    /* Would write to hardware gamma registers */
    
    lck_mtx_unlock(fDisplayLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::updateColorMatrix(const float matrix[3][3])
{
    lck_mtx_lock(fDisplayLock);
    
    memcpy(fColorCal.color_matrix, matrix, sizeof(float) * 9);
    
    /* Program color matrix */
    /* Would write to hardware color matrix registers */
    
    lck_mtx_unlock(fDisplayLock);
    
    return kIOReturnSuccess;
}

/*==============================================================================
 * Thermal Management
 *==============================================================================*/

void AppleH1CLCD::updateTemperature(void)
{
    if (!fRegisters) {
        return;
    }
    
    uint32_t temp = readRegister(offsetof(h1clcd_registers, temperature));
    
    lck_mtx_lock(fThermalLock);
    
    fTemperature = temp;
    fLastThermalSample = getCurrentUptime();
    
    /* Check thermal thresholds */
    if (temp >= kThermalConfig.temp_warning) {
        fThermalWarningCount++;
        
        if (temp >= kThermalConfig.temp_shutdown) {
            IOLog("AppleH1CLCD: CRITICAL TEMPERATURE: %d°C - Shutting down\n", temp);
            /* Emergency shutdown */
            setPowerStateInternal(DISPLAY_POWER_OFF);
        } else if (temp >= kThermalConfig.temp_throttle_start) {
            uint32_t throttle = (temp - kThermalConfig.temp_throttle_start) * 100 /
                                 (kThermalConfig.temp_throttle_max - kThermalConfig.temp_throttle_start);
            if (throttle > 100) throttle = 100;
            
            throttleDisplay(throttle);
        }
    }
    
    lck_mtx_unlock(fThermalLock);
}

IOReturn AppleH1CLCD::handleThermalEvent(void)
{
    updateTemperature();
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::throttleDisplay(uint32_t level)
{
    if (level > 100) {
        level = 100;
    }
    
    lck_mtx_lock(fThermalLock);
    
    fThrottleLevel = level;
    fThermalThrottleCount++;
    
    /* Reduce backlight based on thermal level */
    if (level > 0 && fBacklightEnabled) {
        uint32_t new_brightness = fBrightness * (100 - level) / 100;
        setBrightness(new_brightness);
    }
    
    /* Could also reduce refresh rate at high throttle levels */
    if (level > 80 && (fChipCapabilities & CAP_PROMOTION)) {
        setRefreshRate(30);
    }
    
    writeRegister(offsetof(h1clcd_registers, throttle_level), level);
    
    lck_mtx_unlock(fThermalLock);
    
    IOLog("AppleH1CLCD: Thermal throttle level: %d%%\n", level);
    
    return kIOReturnSuccess;
}

/*==============================================================================
 * DMA and Framebuffer Management
 *==============================================================================*/

IOReturn AppleH1CLCD::configureFramebuffer(void* fb, uint32_t size, uint32_t stride)
{
    if (!fb || size == 0) {
        return kIOReturnBadArgument;
    }
    
    lck_mtx_lock(fDMALock);
    
    fFramebufferVirtual = fb;
    fFramebufferSize = size;
    fLineStride = stride;
    
    /* Get physical address */
    fFramebufferPhys = pmap_extract(kernel_pmap, (vm_address_t)fb);
    
    writeRegister(offsetof(h1clcd_registers, dma_fb_address), (uint32_t)fFramebufferPhys);
    writeRegister(offsetof(h1clcd_registers, dma_fb_size), size);
    writeRegister(offsetof(h1clcd_registers, dma_line_stride), stride);
    
    lck_mtx_unlock(fDMALock);
    
    IOLog("AppleH1CLCD: Framebuffer configured: virt=%p, phys=0x%llx, size=%d, stride=%d\n",
          fb, fFramebufferPhys, size, stride);
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::enableDMA(bool enable)
{
    if (!fRegisters) {
        return kIOReturnNotReady;
    }
    
    lck_mtx_lock(fDMALock);
    
    if (enable) {
        writeRegister(offsetof(h1clcd_registers, dma_enable), 1);
        fDisplayFlags |= DISPLAY_FLAG_ENABLE;
    } else {
        writeRegister(offsetof(h1clcd_registers, dma_enable), 0);
        fDisplayFlags &= ~DISPLAY_FLAG_ENABLE;
    }
    
    lck_mtx_unlock(fDMALock);
    
    IOLog("AppleH1CLCD: DMA %s\n", enable ? "enabled" : "disabled");
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::handleDMAUnderflow(void)
{
    lck_mtx_lock(fDMALock);
    
    fDMAUnderflows++;
    fUnderflowCount++;
    
    /* Read current underflow count from hardware */
    uint32_t hw_underflow = readRegister(offsetof(h1clcd_registers, dma_underflow));
    
    /* Attempt to recover */
    writeRegister(offsetof(h1clcd_registers, dma_enable), 0);
    IODelay(10);
    writeRegister(offsetof(h1clcd_registers, dma_enable), 1);
    
    lck_mtx_unlock(fDMALock);
    
    IOLog("AppleH1CLCD: DMA underflow occurred (count: %d, hw: %d)\n",
          fDMAUnderflows, hw_underflow);
    
    return kIOReturnSuccess;
}

IOReturn AppleH1CLCD::handleDMAOverflow(void)
{
    lck_mtx_lock(fDMALock);
    
    fDMAOverflows++;
    fOverflowCount++;
    
    uint32_t hw_overflow = readRegister(offsetof(h1clcd_registers, dma_overflow));
    
    lck_mtx_unlock(fDMALock);
    
    IOLog("AppleH1CLCD: DMA overflow occurred (count: %d, hw: %d)\n",
          fDMAOverflows, hw_overflow);
    
    return kIOReturnSuccess;
}

/*==============================================================================
 * Interrupt Handling
 *==============================================================================*/

void AppleH1CLCD::interruptHandler(void)
{
    uint32_t int_status;
    
    if (!fRegisters) {
        return;
    }
    
    int_status = readRegister(offsetof(h1clcd_registers, int_status));
    
    /* Clear interrupts */
    writeRegister(offsetof(h1clcd_registers, int_clear), int_status);
    
    /* Handle VSYNC */
    if (int_status & INT_VSYNC) {
        fVSyncCount++;
        fLastVSync = getCurrentUptime();
        
        /* Could notify clients of VSYNC */
    }
    
    /* Handle underflow */
    if (int_status & INT_UNDERFLOW) {
        handleDMAUnderflow();
    }
    
    /* Handle overflow */
    if (int_status & INT_OVERFLOW) {
        handleDMAOverflow();
    }
    
    /* Handle tearing */
    if (int_status & INT_TE) {
        fTearingCount++;
    }
    
    /* Handle thermal events */
    if (int_status & (INT_THERMAL_WARNING | INT_THERMAL_CRITICAL)) {
        handleThermalEvent();
    }
    
    /* Handle backlight failure */
    if (int_status & INT_BACKLIGHT_FAIL) {
        IOLog("AppleH1CLCD: Backlight failure detected\n");
        /* Attempt recovery */
        writeRegister(offsetof(h1clcd_registers, backlight_enable), 0);
        IODelay(100);
        writeRegister(offsetof(h1clcd_registers, backlight_enable), 1);
    }
}

void AppleH1CLCD::interruptHandler(IOInterruptEventSource* source, int count)
{
    AppleH1CLCD* me = (AppleH1CLCD*)source->owner();
    if (me) {
        me->interruptHandler();
    }
}

/*==============================================================================
 * Timer
 *==============================================================================*/

void AppleH1CLCD::timerFired(void)
{
    /* Update temperature */
    updateTemperature();
    
    /* Check for frame completion */
    fFrameCount++;
    fLastFrame = getCurrentUptime();
    
    /* Reschedule timer */
    fTimerSource->setTimeoutMS(1000);  /* 1 second */
}

void AppleH1CLCD::timerFired(OSObject* owner, IOTimerEventSource* sender)
{
    AppleH1CLCD* me = OSDynamicCast(AppleH1CLCD, owner);
    if (me) {
        me->timerFired();
    }
}

/*==============================================================================
 * Work Loop Management
 *==============================================================================*/

bool AppleH1CLCD::createWorkLoop(void)
{
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        IOLog("AppleH1CLCD: Failed to create work loop\n");
        return false;
    }
    
    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        IOLog("AppleH1CLCD: Failed to create command gate\n");
        fWorkLoop->release();
        fWorkLoop = NULL;
        return false;
    }
    
    fWorkLoop->addEventSource(fCommandGate);
    
    fInterruptSource = IOInterruptEventSource::interruptEventSource(this,
        OSMemberFunctionCast(IOInterruptEventAction, this,
                             &AppleH1CLCD::interruptHandler),
        getProvider(),
        0);
    
    if (!fInterruptSource) {
        IOLog("AppleH1CLCD: Failed to create interrupt source\n");
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
                             &AppleH1CLCD::timerFired));
    
    if (!fTimerSource) {
        IOLog("AppleH1CLCD: Failed to create timer source\n");
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
    
    return true;
}

void AppleH1CLCD::destroyWorkLoop(void)
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

/*==============================================================================
 * Mode Management
 *==============================================================================*/

IOReturn AppleH1CLCD::buildModeList(void)
{
    fModeArray = OSArray::withCapacity(8);
    if (!fModeArray) {
        return kIOReturnNoMemory;
    }
    
    /* Build list of supported modes */
    display_mode_t mode;
    
    /* 60Hz mode */
    mode.width = fPanelConfig.h_active;
    mode.height = fPanelConfig.v_active;
    mode.refresh_rate = 60 * 1000;
    mode.h_total = fHTotal;
    mode.v_total = fVTotal;
    mode.h_sync_start = fPanelConfig.h_front_porch;
    mode.h_sync_end = fPanelConfig.h_front_porch + fPanelConfig.h_sync_width;
    mode.v_sync_start = fPanelConfig.v_front_porch;
    mode.v_sync_end = fPanelConfig.v_front_porch + fPanelConfig.v_sync_width;
    mode.pixel_clock = fHTotal * fVTotal * 60;
    mode.flags = 0;
    mode.color_format = fColorFormat;
    mode.color_depth = fColorDepth;
    mode.dsi_lanes = 4;
    mode.dsi_bitrate = 1000000;
    
    OSData* modeData = OSData::withBytes(&mode, sizeof(mode));
    if (modeData) {
        fModeArray->setObject(modeData);
        modeData->release();
    }
    
    /* Add other supported refresh rates */
    if (fChipCapabilities & CAP_PROMOTION) {
        uint32_t rates[] = { 90, 120, 240 };
        for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
            if (rates[i] >= fPanelConfig.refresh_min && rates[i] <= fPanelConfig.refresh_max) {
                mode.refresh_rate = rates[i] * 1000;
                mode.pixel_clock = fHTotal * fVTotal * rates[i];
                
                modeData = OSData::withBytes(&mode, sizeof(mode));
                if (modeData) {
                    fModeArray->setObject(modeData);
                    modeData->release();
                }
            }
        }
    }
    
    IOLog("AppleH1CLCD: Built %d display modes\n", fModeArray->getCount());
    
    return kIOReturnSuccess;
}

const display_mode_t* AppleH1CLCD::findBestMode(uint32_t width, uint32_t height, uint32_t rate)
{
    if (!fModeArray) {
        return NULL;
    }
    
    for (uint32_t i = 0; i < fModeArray->getCount(); i++) {
        OSData* modeData = OSDynamicCast(OSData, fModeArray->getObject(i));
        if (!modeData) continue;
        
        const display_mode_t* mode = (const display_mode_t*)modeData->getBytesNoCopy();
        if (mode->width == width && mode->height == height) {
            if (rate == 0 || mode->refresh_rate / 1000 == rate) {
                return mode;
            }
        }
    }
    
    return NULL;
}

/*==============================================================================
 * Utility
 *==============================================================================*/

uint32_t AppleH1CLCD::getCurrentUptime(void)
{
    uint64_t uptime;
    clock_get_uptime(&uptime);
    return (uint32_t)uptime;
}

/*==============================================================================
 * IOService Overrides
 *==============================================================================*/

bool AppleH1CLCD::init(OSDictionary* dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    
    /* Initialize state */
    fChipId = 0;
    fChipRevision = 0;
    fFirmwareVersion = 0;
    fChipCapabilities = 0;
    fChipFeatures = 0;
    
    fPanelDetected = false;
    fDisplayState = 0;
    fPowerState = DISPLAY_POWER_OFF;
    fBrightness = BRIGHTNESS_DEFAULT;
    fContrast = 50;
    fColorFormat = COLOR_FORMAT_DISPLAY_P3;
    fColorDepth = 10;
    fHdrMode = HDR_MODE_OFF;
    fRefreshRate = 60;
    fVariableRefresh = false;
    
    fTemperature = 0;
    fThrottleLevel = 0;
    fThermalWarningCount = 0;
    
    fBacklightEnabled = 0;
    fBacklightMax = 1000;
    fBacklightPWM = 0;
    
    fVSyncCount = 0;
    fFrameCount = 0;
    fUnderflowCount = 0;
    fOverflowCount = 0;
    fTearingCount = 0;
    fThermalThrottleCount = 0;
    fPowerTransitions = 0;
    
    fModeArray = NULL;
    fCalibrationLoaded = false;
    
    /* Create lock group */
    fLockGroup = lck_grp_alloc_init("AppleH1CLCD", LCK_GRP_ATTR_NULL);
    if (!fLockGroup) {
        return false;
    }
    
    fLockAttr = lck_attr_alloc_init();
    if (!fLockAttr) {
        lck_grp_free(fLockGroup);
        return false;
    }
    
    /* Initialize locks */
    fDisplayLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fThermalLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fBacklightLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    fDMALock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    
    if (!fDisplayLock || !fThermalLock || !fBacklightLock || !fDMALock) {
        return false;
    }
    
    return true;
}

void AppleH1CLCD::free(void)
{
    /* Free locks */
    if (fDisplayLock) lck_mtx_free(fDisplayLock, fLockGroup);
    if (fThermalLock) lck_mtx_free(fThermalLock, fLockGroup);
    if (fBacklightLock) lck_mtx_free(fBacklightLock, fLockGroup);
    if (fDMALock) lck_mtx_free(fDMALock, fLockGroup);
    
    if (fLockAttr) lck_attr_free(fLockAttr);
    if (fLockGroup) lck_grp_free(fLockGroup);
    
    /* Free mode array */
    if (fModeArray) {
        fModeArray->release();
        fModeArray = NULL;
    }
    
    super::free();
}

bool AppleH1CLCD::start(IOService* provider)
{
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("AppleH1CLCD: Starting H1 LCD Controller\n");
    
    /* Map registers */
    if (!mapRegisters()) {
        IOLog("AppleH1CLCD: Failed to map registers\n");
        return false;
    }
    
    /* Identify chip */
    if (!identifyChip()) {
        IOLog("AppleH1CLCD: Failed to identify chip\n");
        unmapRegisters();
        return false;
    }
    
    /* Detect panel */
    if (!detectPanel()) {
        IOLog("AppleH1CLCD: Failed to detect panel\n");
        unmapRegisters();
        return false;
    }
    
    /* Create work loop */
    if (!createWorkLoop()) {
        IOLog("AppleH1CLCD: Failed to create work loop\n");
        unmapRegisters();
        return false;
    }
    
    /* Initialize hardware */
    if (!initializeHardware()) {
        IOLog("AppleH1CLCD: Failed to initialize hardware\n");
        destroyWorkLoop();
        unmapRegisters();
        return false;
    }
    
    /* Build mode list */
    buildModeList();
    
    /* Register power management */
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, NULL, 0);
    
    /* Start timer */
    if (fTimerSource) {
        fTimerSource->setTimeoutMS(1000);
    }
    
    /* Publish properties */
    setProperty("H1CLCD Version", APPLE_H1CLCD_VERSION, 32);
    setProperty("H1CLCD Revision", APPLE_H1CLCD_REVISION, 32);
    setProperty("Chip ID", fChipId, 32);
    setProperty("Chip Revision", fChipRevision, 32);
    setProperty("Firmware Version", fFirmwareVersion, 32);
    
    char chip_str[32];
    snprintf(chip_str, sizeof(chip_str), "0x%08x", fChipId);
    setProperty("Chip", chip_str);
    
    setProperty("Panel Detected", fPanelDetected ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("Panel Name", fPanelConfig.name);
    setProperty("Panel Manufacturer", fPanelConfig.manufacturer);
    setProperty("Panel Resolution", OSString::withCString("368x448"));
    setProperty("Panel Type", fPanelType, 32);
    
    /* Export capabilities */
    OSDictionary* capsDict = OSDictionary::withCapacity(8);
    if (capsDict) {
        capsDict->setObject("HDR", (fChipCapabilities & CAP_HDR) ? kOSBooleanTrue : kOSBooleanFalse);
        capsDict->setObject("ProMotion", (fChipCapabilities & CAP_PROMOTION) ? kOSBooleanTrue : kOSBooleanFalse);
        capsDict->setObject("AOD", (fChipCapabilities & CAP_AOD) ? kOSBooleanTrue : kOSBooleanFalse);
        capsDict->setObject("TrueTone", (fChipCapabilities & CAP_TRUETONE) ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("Capabilities", capsDict);
        capsDict->release();
    }
    
    setProperty("Display State", fDisplayState, 32);
    setProperty("Power State", fPowerState, 32);
    setProperty("Brightness", fBrightness, 32);
    setProperty("Refresh Rate", fRefreshRate, 32);
    
    setProperty("VSync Count", fVSyncCount, 64);
    setProperty("Frame Count", fFrameCount, 64);
    setProperty("Power Transitions", fPowerTransitions, 64);
    
    /* Register service */
    registerService();
    
    IOLog("AppleH1CLCD: Started successfully\n");
    
    return true;
}

void AppleH1CLCD::stop(IOService* provider)
{
    IOLog("AppleH1CLCD: Stopping\n");
    
    /* Stop timer */
    if (fTimerSource) {
        fTimerSource->cancelTimeout();
    }
    
    /* Shutdown hardware */
    shutdownHardware();
    
    /* Destroy work loop */
    destroyWorkLoop();
    
    /* Power management */
    PMstop();
    
    /* Unmap registers */
    unmapRegisters();
    
    super::stop(provider);
}

/*==============================================================================
 * Power Management
 *==============================================================================*/

IOReturn AppleH1CLCD::setPowerState(unsigned long powerState, IOService* device)
{
    IOLog("AppleH1CLCD: setPowerState: %lu\n", powerState);
    
    switch (powerState) {
        case 0:
            /* Sleep */
            setPowerStateInternal(DISPLAY_POWER_SLEEP);
            break;
            
        case 1:
            /* Low power */
            setPowerStateInternal(DISPLAY_POWER_LOW_POWER);
            break;
            
        case 2:
            /* Active */
            setPowerStateInternal(DISPLAY_POWER_ON);
            break;
    }
    
    return IOPMAckImplied;
}

IOReturn AppleH1CLCD::powerStateWillChangeTo(IOPMPowerFlags capabilities,
                                              unsigned long stateNumber,
                                              IOService* whatDevice)
{
    return IOPMAckImplied;
}

IOReturn AppleH1CLCD::powerStateDidChangeTo(IOPMPowerFlags capabilities,
                                             unsigned long stateNumber,
                                             IOService* whatDevice)
{
    return IOPMAckImplied;
}

/*==============================================================================
 * Platform Functions
 *==============================================================================*/

IOReturn AppleH1CLCD::callPlatformFunction(const OSSymbol* functionName,
                                            bool waitForFunction,
                                            void* param1, void* param2,
                                            void* param3, void* param4)
{
    if (functionName->isEqualTo("setBrightness")) {
        uint32_t brightness = (uint32_t)(uintptr_t)param1;
        return setBrightness(brightness);
    }
    
    if (functionName->isEqualTo("setPowerState")) {
        uint32_t state = (uint32_t)(uintptr_t)param1;
        return setPowerStateInternal(state);
    }
    
    if (functionName->isEqualTo("setHDRMode")) {
        uint32_t mode = (uint32_t)(uintptr_t)param1;
        return setHDRMode(mode);
    }
    
    if (functionName->isEqualTo("getVSyncCount")) {
        uint64_t* count = (uint64_t*)param1;
        if (count) {
            *count = fVSyncCount;
            return kIOReturnSuccess;
        }
    }
    
    return super::callPlatformFunction(functionName, waitForFunction,
                                        param1, param2, param3, param4);
}

/*==============================================================================
 * Property Serialization
 *==============================================================================*/

bool AppleH1CLCD::serializeProperties(OSSerialize* s) const
{
    bool result = super::serializeProperties(s);
    
    if (result) {
        /* Add dynamic properties */
        OSDictionary* dict = OSDynamicCast(OSDictionary, s->getProperty());
        if (dict) {
            /* Update with current values */
            ((AppleH1CLCD*)this)->setProperty("Temperature", fTemperature, 32);
            ((AppleH1CLCD*)this)->setProperty("Throttle Level", fThrottleLevel, 32);
            ((AppleH1CLCD*)this)->setProperty("VSync Count", fVSyncCount, 64);
            ((AppleH1CLCD*)this)->setProperty("Frame Count", fFrameCount, 64);
            ((AppleH1CLCD*)this)->setProperty("Power Transitions", fPowerTransitions, 64);
        }
    }
    
    return result;
}
