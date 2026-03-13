/*
 * Copyright (c) 2005-2022 Apple Inc. All rights reserved.
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
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOGraphicsInterface.h>
#include <IOKit/graphics/IOGraphicsEngine.h>
#include <IOKit/graphics/IOGraphicsAccelerator.h>
#include <IOKit/graphics/IOGraphicsTypes.h>

#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSDebug.h>

#include <mach/vm_param.h>
#include <mach/vm_map.h>
#include <mach/memory_object_types.h>

/*==============================================================================
 * AppleMBX - Graphics Controller Driver for MBX/Mobile Graphics
 * 
 * This driver supports the MBX graphics core found in:
 * - iPhone/iPod touch (first generation)
 * - iPhone 3G
 * - iPhone 3GS (with MBX Lite)
 * - Various ARM-based devices
 * 
 * The MBX (Media eXtension) is a 3D graphics core licensed from Imagination
 * Technologies, part of the PowerVR family.
 *==============================================================================*/

/*==============================================================================
 * Version Information
 *==============================================================================*/

#define APPLE_MBX_VERSION               0x00030002  /* Version 3.2 */
#define APPLE_MBX_REVISION              0x00000003

/*==============================================================================
 * Hardware Register Definitions (MBX Graphics Core)
 * 
 * Based on Imagination Technologies MBX/MBX Lite architecture
 *==============================================================================*/

/* Register base offsets */
#define MBX_REG_ID                       0x0000  /* Core ID register */
#define MBX_REG_REVISION                 0x0004  /* Core revision */
#define MBX_REG_STATUS                   0x0008  /* Core status */
#define MBX_REG_CONTROL                   0x000C  /* Core control */
#define MBX_REG_INTERRUPT                 0x0010  /* Interrupt control */
#define MBX_REG_INTERRUPT_STATUS          0x0014  /* Interrupt status */
#define MBX_REG_INTERRUPT_ENABLE          0x0018  /* Interrupt enable */
#define MBX_REG_INTERRUPT_CLEAR           0x001C  /* Interrupt clear */
#define MBX_REG_CLOCK_CONTROL             0x0020  /* Clock control */
#define MBX_REG_POWER_CONTROL             0x0024  /* Power control */
#define MBX_REG_RESET_CONTROL             0x0028  /* Reset control */
#define MBX_REG_DEBUG                     0x002C  /* Debug register */

/* Memory management registers */
#define MBX_REG_MMU_CONTROL               0x0100  /* MMU control */
#define MBX_REG_MMU_STATUS                0x0104  /* MMU status */
#define MBX_REG_MMU_PAGE_TABLE_BASE       0x0108  /* Page table base */
#define MBX_REG_MMU_PAGE_SIZE              0x010C  /* Page size */
#define MBX_REG_MMU_TLB_INVALIDATE        0x0110  /* TLB invalidate */

/* Memory interface registers */
#define MBX_REG_MEMIF_CONTROL             0x0200  /* Memory interface control */
#define MBX_REG_MEMIF_STATUS              0x0204  /* Memory interface status */
#define MBX_REG_MEMIF_BURST_SIZE          0x0208  /* Burst size */
#define MBX_REG_MEMIF_READ_QUEUE           0x020C  /* Read queue depth */
#define MBX_REG_MEMIF_WRITE_QUEUE          0x0210  /* Write queue depth */

/* Pixel pipeline registers */
#define MBX_REG_PIXEL_CONTROL             0x0300  /* Pixel pipeline control */
#define MBX_REG_PIXEL_STATUS              0x0304  /* Pixel pipeline status */
#define MBX_REG_PIXEL_FIFO                0x0308  /* Pixel FIFO status */
#define MBX_REG_PIXEL_WIDTH                0x030C  /* Pixel width */
#define MBX_REG_PIXEL_FORMAT               0x0310  /* Pixel format */

/* Geometry registers */
#define MBX_REG_GEOM_CONTROL              0x0400  /* Geometry control */
#define MBX_REG_GEOM_STATUS               0x0404  /* Geometry status */
#define MBX_REG_GEOM_FIFO                 0x0408  /* Geometry FIFO */
#define MBX_REG_VERTEX_COUNT              0x040C  /* Vertex count */
#define MBX_REG_TRIANGLE_COUNT             0x0410  /* Triangle count */

/* Display interface registers */
#define MBX_REG_DISPLAY_CONTROL           0x0500  /* Display control */
#define MBX_REG_DISPLAY_STATUS            0x0504  /* Display status */
#define MBX_REG_DISPLAY_BASE              0x0508  /* Display buffer base */
#define MBX_REG_DISPLAY_STRIDE            0x050C  /* Display stride */
#define MBX_REG_DISPLAY_WIDTH             0x0510  /* Display width */
#define MBX_REG_DISPLAY_HEIGHT            0x0514  /* Display height */
#define MBX_REG_DISPLAY_FORMAT            0x0518  /* Display format */
#define MBX_REG_DISPLAY_TIMING_H          0x051C  /* Horizontal timing */
#define MBX_REG_DISPLAY_TIMING_V          0x0520  /* Vertical timing */
#define MBX_REG_DISPLAY_HSYNC             0x0524  /* HSYNC control */
#define MBX_REG_DISPLAY_VSYNC             0x0528  /* VSYNC control */
#define MBX_REG_DISPLAY_BRIGHTNESS        0x052C  /* Brightness control */
#define MBX_REG_DISPLAY_GAMMA              0x0530  /* Gamma control */

/* 2D Engine registers */
#define MBX_REG_2D_CONTROL                0x0600  /* 2D engine control */
#define MBX_REG_2D_STATUS                 0x0604  /* 2D engine status */
#define MBX_REG_2D_SRC_BASE               0x0608  /* Source base */
#define MBX_REG_2D_DST_BASE               0x060C  /* Destination base */
#define MBX_REG_2D_SRC_STRIDE             0x0610  /* Source stride */
#define MBX_REG_2D_DST_STRIDE             0x0614  /* Destination stride */
#define MBX_REG_2D_SRC_FORMAT              0x0618  /* Source format */
#define MBX_REG_2D_DST_FORMAT              0x061C  /* Destination format */
#define MBX_REG_2D_WIDTH                  0x0620  /* Operation width */
#define MBX_REG_2D_HEIGHT                 0x0624  /* Operation height */
#define MBX_REG_2D_ROP                    0x0628  /* Raster operation */
#define MBX_REG_2D_COLOR                   0x062C  /* Solid color */

/* 3D Engine registers (TA - Tile Accelerator) */
#define MBX_REG_TA_CONTROL                0x0700  /* TA control */
#define MBX_REG_TA_STATUS                 0x0704  /* TA status */
#define MBX_REG_TA_PARAM_BASE             0x0708  /* Parameter base */
#define MBX_REG_TA_PARAM_SIZE              0x070C  /* Parameter size */
#define MBX_REG_TA_VERTEX_BASE            0x0710  /* Vertex base */
#define MBX_REG_TA_VERTEX_COUNT            0x0714  /* Vertex count */
#define MBX_REG_TA_INDEX_BASE              0x0718  /* Index base */
#define MBX_REG_TA_INDEX_COUNT             0x071C  /* Index count */
#define MBX_REG_TA_TILE_SIZE               0x0720  /* Tile size */
#define MBX_REG_TA_DEPTH_BIAS              0x0724  /* Depth bias */

/* 3D Engine registers (SP - Shader Processor) */
#define MBX_REG_SP_CONTROL                0x0800  /* SP control */
#define MBX_REG_SP_STATUS                 0x0804  /* SP status */
#define MBX_REG_SP_TEXTURE_BASE           0x0808  /* Texture base */
#define MBX_REG_SP_TEXTURE_SIZE           0x080C  /* Texture size */
#define MBX_REG_SP_TEXTURE_FORMAT         0x0810  /* Texture format */
#define MBX_REG_SP_TEXTURE_WRAP           0x0814  /* Texture wrap */
#define MBX_REG_SP_TEXTURE_FILTER         0x0818  /* Texture filter */
#define MBX_REG_SP_COLOR_BASE             0x081C  /* Color buffer base */
#define MBX_REG_SP_DEPTH_BASE              0x0820  /* Depth buffer base */
#define MBX_REG_SP_STENCIL_BASE            0x0824  /* Stencil buffer base */
#define MBX_REG_SP_VIEWPORT_X             0x0828  /* Viewport X */
#define MBX_REG_SP_VIEWPORT_Y             0x082C  /* Viewport Y */
#define MBX_REG_SP_VIEWPORT_WIDTH         0x0830  /* Viewport width */
#define MBX_REG_SP_VIEWPORT_HEIGHT        0x0834  /* Viewport height */

/* Performance counters */
#define MBX_REG_PERF_CONTROL              0x0900  /* Performance control */
#define MBX_REG_PERF_COUNTER0             0x0904  /* Performance counter 0 */
#define MBX_REG_PERF_COUNTER1             0x0908  /* Performance counter 1 */
#define MBX_REG_PERF_COUNTER2             0x090C  /* Performance counter 2 */
#define MBX_REG_PERF_COUNTER3             0x0910  /* Performance counter 3 */

/*==============================================================================
 * Register Bit Definitions
 *==============================================================================*/

/* Status bits */
#define MBX_STATUS_READY                  (1 << 0)
#define MBX_STATUS_BUSY                   (1 << 1)
#define MBX_STATUS_MEMORY_ERROR            (1 << 2)
#define MBX_STATUS_MMU_ERROR               (1 << 3)
#define MBX_STATUS_DISPLAY_ACTIVE          (1 << 4)
#define MBX_STATUS_2D_BUSY                 (1 << 5)
#define MBX_STATUS_3D_BUSY                 (1 << 6)
#define MBX_STATUS_VSYNC                    (1 << 7)

/* Control bits */
#define MBX_CONTROL_ENABLE                 (1 << 0)
#define MBX_CONTROL_RESET                  (1 << 1)
#define MBX_CONTROL_SOFT_RESET             (1 << 2)
#define MBX_CONTROL_IDLE                   (1 << 3)
#define MBX_CONTROL_SUSPEND                 (1 << 4)

/* Interrupt bits */
#define MBX_INTR_VSYNC                     (1 << 0)
#define MBX_INTR_2D_COMPLETE               (1 << 1)
#define MBX_INTR_3D_COMPLETE               (1 << 2)
#define MBX_INTR_DMA_COMPLETE              (1 << 3)
#define MBX_INTR_ERROR                     (1 << 4)
#define MBX_INTR_MMU_PAGE_FAULT            (1 << 5)
#define MBX_INTR_BUS_ERROR                  (1 << 6)

/* Clock control bits */
#define MBX_CLOCK_CORE_ENABLE              (1 << 0)
#define MBX_CLOCK_MEMORY_ENABLE            (1 << 1)
#define MBX_CLOCK_2D_ENABLE                (1 << 2)
#define MBX_CLOCK_3D_ENABLE                (1 << 3)
#define MBX_CLOCK_DISPLAY_ENABLE           (1 << 4)
#define MBX_CLOCK_PLL_LOCKED               (1 << 8)
#define MBX_CLOCK_DIVIDER_MASK             0x00FF0000
#define MBX_CLOCK_DIVIDER_SHIFT            16

/* Power control bits */
#define MBX_POWER_ON                       (1 << 0)
#define MBX_POWER_SLEEP                    (1 << 1)
#define MBX_POWER_DEEP_SLEEP                (1 << 2)
#define MBX_POWER_OFF                      (1 << 3)
#define MBX_POWER_STATUS_MASK              0x0000000F

/* MMU control bits */
#define MBX_MMU_ENABLE                     (1 << 0)
#define MBX_MMU_PAGE_SIZE_4K               (0 << 4)
#define MBX_MMU_PAGE_SIZE_16K              (1 << 4)
#define MBX_MMU_PAGE_SIZE_64K              (2 << 4)
#define MBX_MMU_CACHE_ENABLE               (1 << 8)

/* Pixel formats */
#define MBX_PIXEL_FORMAT_RGB565            0x00
#define MBX_PIXEL_FORMAT_RGB888            0x01
#define MBX_PIXEL_FORMAT_ARGB8888          0x02
#define MBX_PIXEL_FORMAT_ABGR8888          0x03
#define MBX_PIXEL_FORMAT_YUV420            0x04
#define MBX_PIXEL_FORMAT_YVU420            0x05
#define MBX_PIXEL_FORMAT_NV12              0x06
#define MBX_PIXEL_FORMAT_NV21              0x07

/* 2D Raster Operations */
#define MBX_ROP_COPY                       0x00
#define MBX_ROP_AND                        0x01
#define MBX_ROP_OR                         0x02
#define MBX_ROP_XOR                        0x03
#define MBX_ROP_BLEND                      0x04
#define MBX_ROP_BLEND_SRC_OVER             0x05
#define MBX_ROP_BLEND_DST_OVER             0x06
#define MBX_ROP_BLEND_SRC_IN               0x07
#define MBX_ROP_BLEND_DST_IN               0x08
#define MBX_ROP_BLEND_SRC_OUT              0x09
#define MBX_ROP_BLEND_DST_OUT              0x0A
#define MBX_ROP_BLEND_SRC_ATOP             0x0B
#define MBX_ROP_BLEND_DST_ATOP             0x0C
#define MBX_ROP_BLEND_XOR                   0x0D

/* Texture formats */
#define MBX_TEX_FORMAT_RGB565              0x00
#define MBX_TEX_FORMAT_RGB888              0x01
#define MBX_TEX_FORMAT_ARGB8888            0x02
#define MBX_TEX_FORMAT_ABGR8888            0x03
#define MBX_TEX_FORMAT_DXT1                0x04
#define MBX_TEX_FORMAT_DXT3                0x05
#define MBX_TEX_FORMAT_DXT5                0x06
#define MBX_TEX_FORMAT_ETC1                0x07
#define MBX_TEX_FORMAT_PVRTC2              0x08
#define MBX_TEX_FORMAT_PVRTC4              0x09

/* Texture wrap modes */
#define MBX_TEX_WRAP_REPEAT                0x00
#define MBX_TEX_WRAP_CLAMP                 0x01
#define MBX_TEX_WRAP_MIRROR                0x02
#define MBX_TEX_WRAP_BORDER                0x03

/* Texture filter modes */
#define MBX_TEX_FILTER_POINT               0x00
#define MBX_TEX_FILTER_LINEAR              0x01
#define MBX_TEX_FILTER_BILINEAR            0x02
#define MBX_TEX_FILTER_TRILINEAR            0x03
#define MBX_TEX_FILTER_ANISOTROPIC         0x04

/*==============================================================================
 * Display Timing Structure
 *==============================================================================*/

typedef struct {
    uint32_t    width;                      /* Display width in pixels */
    uint32_t    height;                     /* Display height in pixels */
    uint32_t    h_front_porch;              /* Horizontal front porch */
    uint32_t    h_sync_width;               /* Horizontal sync width */
    uint32_t    h_back_porch;               /* Horizontal back porch */
    uint32_t    v_front_porch;              /* Vertical front porch */
    uint32_t    v_sync_width;               /* Vertical sync width */
    uint32_t    v_back_porch;               /* Vertical back porch */
    uint32_t    h_polarity;                 /* Horizontal sync polarity */
    uint32_t    v_polarity;                 /* Vertical sync polarity */
    uint32_t    pixel_clock;                 /* Pixel clock in kHz */
    uint32_t    refresh_rate;                /* Refresh rate in Hz */
} display_timing_t;

/*==============================================================================
 * Color Lookup Table (CLUT)
 *==============================================================================*/

#define CLUT_SIZE                        256

typedef struct {
    uint8_t     red[CLUT_SIZE];
    uint8_t     green[CLUT_SIZE];
    uint8_t     blue[CLUT_SIZE];
} clut_t;

/*==============================================================================
 * Surface Structure
 *==============================================================================*/

typedef struct {
    uint32_t    width;                      /* Surface width */
    uint32_t    height;                     /* Surface height */
    uint32_t    stride;                     /* Surface stride in bytes */
    uint32_t    format;                     /* Pixel format */
    uint64_t    base_addr;                   /* Physical base address */
    uint64_t    size;                        /* Surface size in bytes */
    void*       virtual_addr;                /* Kernel virtual address */
    uint32_t    flags;                       /* Surface flags */
    uint32_t    refcount;                    /* Reference count */
} surface_t;

/* Surface flags */
#define SURFACE_FLAG_FRAMEBUFFER         (1 << 0)
#define SURFACE_FLAG_DISPLAY              (1 << 1)
#define SURFACE_FLAG_RENDER_TARGET        (1 << 2)
#define SURFACE_FLAG_TEXTURE               (1 << 3)
#define SURFACE_FLAG_LINEAR                (1 << 4)
#define SURFACE_FLAG_TILED                 (1 << 5)
#define SURFACE_FLAG_CACHED                (1 << 6)
#define SURFACE_FLAG_IO_COHERENT           (1 << 7)

/*==============================================================================
 * Command Buffer Structure
 *==============================================================================*/

typedef struct {
    uint32_t*   buffer;                     /* Command buffer */
    uint32_t    size;                       /* Buffer size in dwords */
    uint32_t    write_ptr;                  /* Write pointer */
    uint32_t    read_ptr;                   /* Read pointer */
    uint32_t    count;                       /* Number of commands */
    uint64_t    phys_addr;                   /* Physical address */
    lck_mtx_t*  lock;                        /* Buffer lock */
} cmd_buffer_t;

/*==============================================================================
 * 3D Pipeline State
 *==============================================================================*/

typedef struct {
    /* Vertex processing */
    uint32_t    vertex_shader[256];          /* Vertex shader code */
    uint32_t    vertex_shader_size;          /* Shader size */
    float       modelview_matrix[16];        /* Modelview matrix */
    float       projection_matrix[16];       /* Projection matrix */
    float       texture_matrix[16];           /* Texture matrix */
    
    /* Fragment processing */
    uint32_t    fragment_shader[256];        /* Fragment shader code */
    uint32_t    fragment_shader_size;        /* Shader size */
    uint32_t    blend_mode;                   /* Blend mode */
    uint32_t    depth_test;                   /* Depth test enable */
    uint32_t    depth_func;                    /* Depth function */
    uint32_t    stencil_test;                  /* Stencil test enable */
    uint32_t    stencil_func;                  /* Stencil function */
    uint32_t    stencil_mask;                  /* Stencil mask */
    
    /* Texture state */
    surface_t*  textures[16];                 /* Active textures */
    uint32_t    tex_coord_gen[16];             /* Texture coordinate generation */
    uint32_t    tex_env_mode[16];              /* Texture environment mode */
    
    /* Rasterizer state */
    uint32_t    cull_mode;                     /* Cull mode */
    uint32_t    front_face;                    /* Front face orientation */
    uint32_t    polygon_mode;                  /* Polygon mode */
    float       line_width;                     /* Line width */
    float       point_size;                     /* Point size */
    
    /* Viewport state */
    int32_t     viewport_x;                    /* Viewport X offset */
    int32_t     viewport_y;                    /* Viewport Y offset */
    uint32_t    viewport_width;                /* Viewport width */
    uint32_t    viewport_height;               /* Viewport height */
    float       depth_range_near;               /* Depth range near */
    float       depth_range_far;                /* Depth range far */
} pipeline_state_t;

/*==============================================================================
 * AppleMBX Main Class
 *==============================================================================*/

class AppleMBX : public IOFramebuffer, public IOGraphicsAccelerator
{
    OSDeclareDefaultStructors(AppleMBX)
    
private:
    /* IOKit resources */
    IOWorkLoop*                 fWorkLoop;
    IOCommandGate*              fCommandGate;
    IOTimerEventSource*         fVBlankTimer;
    IOInterruptEventSource*     fInterruptSource;
    
    /* Hardware resources */
    IOMemoryMap*                fMemoryMap;
    volatile uint32_t*          fRegisters;
    IOPhysicalAddress           fPhysicalAddress;
    IOPhysicalLength            fPhysicalLength;
    
    /* Hardware identification */
    uint32_t                    fCoreID;
    uint32_t                    fCoreRevision;
    uint32_t                    fFeatures;
    uint32_t                    fClockSpeed;
    
    /* Display state */
    bool                        fDisplayActive;
    uint32_t                    fDisplayWidth;
    uint32_t                    fDisplayHeight;
    uint32_t                    fDisplayStride;
    uint32_t                    fDisplayFormat;
    surface_t*                  fFramebuffer;
    clut_t                      fCLUT;
    display_timing_t            fTiming;
    uint32_t                    fBrightness;
    
    /* Framebuffer info (for IOFramebuffer) */
    IOFramebufferInfo           fFBInfo;
    IODisplayModeInformation    fDisplayMode;
    IOPixelInformation          fPixelInfo;
    
    /* 2D engine */
    bool                        f2DAvailable;
    uint32_t                    f2DCapabilities;
    cmd_buffer_t*               f2DCmdBuffer;
    lck_mtx_t*                  f2DLock;
    
    /* 3D engine */
    bool                        f3DAvailable;
    uint32_t                    f3DCapabilities;
    cmd_buffer_t*               f3DCmdBuffer;
    pipeline_state_t*           fPipelineState;
    lck_mtx_t*                  f3DLock;
    
    /* Memory management */
    bool                        fMMUEnabled;
    uint64_t                    fPageTableBase;
    uint32_t                    fPageSize;
    OSDictionary*               fSurfaceDict;
    lck_mtx_t*                  fSurfaceLock;
    uint32_t                    fNextSurfaceID;
    
    /* DMA engine */
    bool                        fDMAAvailable;
    cmd_buffer_t*               fDMACmdBuffer;
    
    /* Power management */
    uint32_t                    fPowerState;
    uint32_t                    fWakeLevel;
    uint64_t                    fLastActivity;
    bool                        fSuspended;
    
    /* Statistics */
    uint64_t                    fVBlankCount;
    uint64_t                    fIRQCount;
    uint64_t                    fDrawCalls2D;
    uint64_t                    fDrawCalls3D;
    uint64_t                    fTrianglesRendered;
    uint64_t                    fVerticesProcessed;
    uint64_t                    fTexelsFiltered;
    uint64_t                    fPageFaults;
    uint64_t                    fErrors;
    uint64_t                    fPerformanceCounters[4];
    
    /* Configuration */
    uint32_t                    fMaxWidth;
    uint32_t                    fMaxHeight;
    uint32_t                    fMaxTextureSize;
    uint32_t                    fMaxAnisotropy;
    uint32_t                    fVRAMSize;
    uint64_t                    fVRAMBase;
    
    /*==============================================================================
     * Private Methods
     *==============================================================================*/
    
    /* Hardware access */
    uint32_t                    readReg(uint32_t offset);
    void                        writeReg(uint32_t offset, uint32_t value);
    bool                        waitForBit(uint32_t offset, uint32_t bit, bool set, uint32_t timeout);
    
    /* Hardware initialization */
    bool                        mapRegisters(void);
    void                        unmapRegisters(void);
    bool                        resetController(void);
    bool                        readHardwareVersion(void);
    bool                        initializeMemory(void);
    bool                        initialize2DEngine(void);
    bool                        initialize3DEngine(void);
    bool                        initializeDisplay(void);
    
    /* Display management */
    bool                        setDisplayMode(uint32_t width, uint32_t height, uint32_t format);
    bool                        setDisplayTiming(display_timing_t* timing);
    void                        vBlankHandler(void);
    static void                 vBlankHandler(OSObject* owner, IOTimerEventSource* sender);
    
    /* Interrupt handling */
    void                        interruptHandler(void);
    static void                 interruptHandler(OSObject* owner, IOInterruptEventSource* src, int count);
    
    /* Command processing */
    bool                        createCommandBuffer(cmd_buffer_t** buffer, uint32_t size);
    void                        destroyCommandBuffer(cmd_buffer_t* buffer);
    bool                        submitCommands(cmd_buffer_t* buffer, bool wait);
    uint32_t                    waitForCommandCompletion(cmd_buffer_t* buffer, uint32_t timeout);
    
    /* Surface management */
    surface_t*                  createSurface(uint32_t width, uint32_t height, uint32_t format, uint32_t flags);
    void                        destroySurface(surface_t* surface);
    surface_t*                  findSurface(uint32_t surfaceID);
    bool                        surfaceMakeResident(surface_t* surface);
    void                        surfaceEvict(surface_t* surface);
    
    /* 2D acceleration */
    bool                        blit2D(surface_t* src, surface_t* dst,
                                        uint32_t srcX, uint32_t srcY,
                                        uint32_t dstX, uint32_t dstY,
                                        uint32_t width, uint32_t height,
                                        uint32_t rop);
    bool                        fill2D(surface_t* dst, uint32_t color,
                                       uint32_t x, uint32_t y,
                                       uint32_t width, uint32_t height);
    bool                        copy2D(surface_t* dst, surface_t* src,
                                       uint32_t rop);
    
    /* 3D acceleration */
    bool                        set3DPipeline(pipeline_state_t* state);
    bool                        drawTriangles(uint32_t vertexCount, uint32_t indexCount);
    bool                        drawArrays(uint32_t mode, uint32_t first, uint32_t count);
    bool                        drawElements(uint32_t mode, uint32_t count, uint32_t type, void* indices);
    
    /* MMU management */
    bool                        enableMMU(void);
    void                        disableMMU(void);
    bool                        mapSurface(surface_t* surface);
    void                        unmapSurface(surface_t* surface);
    bool                        handlePageFault(uint64_t address);
    
    /* Power management */
    void                        suspendDevice(void);
    void                        resumeDevice(void);
    bool                        saveContext(void);
    bool                        restoreContext(void);
    
    /* Utility */
    uint64_t                    getTimeUptime(void);
    uint32_t                    getPixelBytes(uint32_t format);
    uint32_t                    alignUp(uint32_t value, uint32_t alignment);
    
protected:
    bool                        init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                        free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                        start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                        stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    /* IOFramebuffer overrides */
    virtual IOReturn            enableController(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            disableController(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            setDisplayMode(IODisplayModeID mode, IOIndex depth) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            setStartupDisplayMode(IODisplayModeID mode, IOIndex depth) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation* info) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation* info) APPLE_KEXT_OVERRIDE;
    virtual IOItemCount         getDisplayModeCount(void) APPLE_KEXT_OVERRIDE;
    virtual IODisplayModeID     getCurrentDisplayMode(void) APPLE_KEXT_OVERRIDE;
    virtual IOIndex             getCurrentDepth(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            getFramebufferInfo(IOFramebufferInfo* info) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            setCLUT(IOColorEntry* colors, UInt32 index, UInt32 num) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            getCLUT(IOColorEntry* colors, UInt32 index, UInt32 num) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            setBrightness(UInt32 brightness) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            getBrightness(UInt32* brightness) APPLE_KEXT_OVERRIDE;
    
    /* IOGraphicsAccelerator overrides */
    virtual IOReturn            performAccelOperation(UInt32 operation, void* reference, void* arguments) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            completeAccelOperation(UInt32 operation, void* reference, void* arguments) APPLE_KEXT_OVERRIDE;
    
    /* Power management */
    virtual IOReturn            setPowerState(unsigned long powerState, IOService* device) APPLE_KEXT_OVERRIDE;
    virtual IOReturn            powerStateWillChangeTo(IOPMPowerFlags capabilities, unsigned long stateNumber, IOService* whatDevice) APPLE_KEXT_OVERRIDE;
    
    /* User client access */
    virtual IOReturn            newUserClient(task_t owningTask, void* securityID, UInt32 type, OSDictionary* properties, IOUserClient** handler) APPLE_KEXT_OVERRIDE;
    
    /* Public API for graphics acceleration */
    virtual bool                is2DAvailable(void) { return f2DAvailable; }
    virtual bool                is3DAvailable(void) { return f3DAvailable; }
    virtual uint32_t            getVRAMSize(void) { return fVRAMSize; }
    virtual uint32_t            getMaxTextureSize(void) { return fMaxTextureSize; }
};

OSDefineMetaClassAndStructors(AppleMBX, IOFramebuffer)

/*==============================================================================
 * AppleMBX Implementation
 *==============================================================================*/

#pragma mark - AppleMBX::Initialization

bool AppleMBX::init(OSDictionary* dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    
    /* Initialize member variables */
    fWorkLoop = NULL;
    fCommandGate = NULL;
    fVBlankTimer = NULL;
    fInterruptSource = NULL;
    
    fMemoryMap = NULL;
    fRegisters = NULL;
    fPhysicalAddress = 0;
    fPhysicalLength = 0;
    
    fCoreID = 0;
    fCoreRevision = 0;
    fFeatures = 0;
    fClockSpeed = 0;
    
    fDisplayActive = false;
    fDisplayWidth = 0;
    fDisplayHeight = 0;
    fDisplayStride = 0;
    fDisplayFormat = 0;
    fFramebuffer = NULL;
    bzero(&fCLUT, sizeof(fCLUT));
    bzero(&fTiming, sizeof(fTiming));
    fBrightness = 128;  /* 50% default */
    
    bzero(&fFBInfo, sizeof(fFBInfo));
    bzero(&fDisplayMode, sizeof(fDisplayMode));
    bzero(&fPixelInfo, sizeof(fPixelInfo));
    
    f2DAvailable = false;
    f2DCapabilities = 0;
    f2DCmdBuffer = NULL;
    
    f3DAvailable = false;
    f3DCapabilities = 0;
    f3DCmdBuffer = NULL;
    fPipelineState = NULL;
    
    fMMUEnabled = false;
    fPageTableBase = 0;
    fPageSize = PAGE_SIZE;
    fSurfaceDict = NULL;
    fNextSurfaceID = 1;
    
    fDMAAvailable = false;
    fDMACmdBuffer = NULL;
    
    fPowerState = 0;
    fWakeLevel = 0;
    fLastActivity = 0;
    fSuspended = false;
    
    fVBlankCount = 0;
    fIRQCount = 0;
    fDrawCalls2D = 0;
    fDrawCalls3D = 0;
    fTrianglesRendered = 0;
    fVerticesProcessed = 0;
    fTexelsFiltered = 0;
    fPageFaults = 0;
    fErrors = 0;
    bzero(fPerformanceCounters, sizeof(fPerformanceCounters));
    
    fMaxWidth = 1024;
    fMaxHeight = 768;
    fMaxTextureSize = 1024;
    fMaxAnisotropy = 16;
    fVRAMSize = 8 * 1024 * 1024;  /* 8MB default */
    fVRAMBase = 0;
    
    /* Create locks */
    f2DLock = lck_mtx_alloc_init(IOService::getKernelLockGroup(), LCK_ATTR_NULL);
    f3DLock = lck_mtx_alloc_init(IOService::getKernelLockGroup(), LCK_ATTR_NULL);
    fSurfaceLock = lck_mtx_alloc_init(IOService::getKernelLockGroup(), LCK_ATTR_NULL);
    
    if (!f2DLock || !f3DLock || !fSurfaceLock) {
        return false;
    }
    
    /* Create surface dictionary */
    fSurfaceDict = OSDictionary::withCapacity(32);
    if (!fSurfaceDict) {
        return false;
    }
    
    return true;
}

void AppleMBX::free(void)
{
    /* Free surfaces */
    if (fSurfaceDict) {
        fSurfaceDict->release();
        fSurfaceDict = NULL;
    }
    
    /* Free command buffers */
    if (f2DCmdBuffer) {
        destroyCommandBuffer(f2DCmdBuffer);
        f2DCmdBuffer = NULL;
    }
    
    if (f3DCmdBuffer) {
        destroyCommandBuffer(f3DCmdBuffer);
        f3DCmdBuffer = NULL;
    }
    
    if (fDMACmdBuffer) {
        destroyCommandBuffer(fDMACmdBuffer);
        fDMACmdBuffer = NULL;
    }
    
    /* Free pipeline state */
    if (fPipelineState) {
        IOFree(fPipelineState, sizeof(pipeline_state_t));
        fPipelineState = NULL;
    }
    
    /* Free framebuffer */
    if (fFramebuffer) {
        destroySurface(fFramebuffer);
        fFramebuffer = NULL;
    }
    
    /* Free locks */
    if (f2DLock) {
        lck_mtx_free(f2DLock, IOService::getKernelLockGroup());
        f2DLock = NULL;
    }
    
    if (f3DLock) {
        lck_mtx_free(f3DLock, IOService::getKernelLockGroup());
        f3DLock = NULL;
    }
    
    if (fSurfaceLock) {
        lck_mtx_free(fSurfaceLock, IOService::getKernelLockGroup());
        fSurfaceLock = NULL;
    }
    
    super::free();
}

#pragma mark - AppleMBX::Hardware Access

uint32_t AppleMBX::readReg(uint32_t offset)
{
    if (!fRegisters || offset >= fPhysicalLength) {
        return 0xFFFFFFFF;
    }
    
    uint32_t value = fRegisters[offset / 4];
    
    /* Add delay for slow hardware if needed */
    if (fCoreRevision < 0x100) {
        IODelay(1);
    }
    
    return value;
}

void AppleMBX::writeReg(uint32_t offset, uint32_t value)
{
    if (!fRegisters || offset >= fPhysicalLength) {
        return;
    }
    
    fRegisters[offset / 4] = value;
    
    /* Add delay for slow hardware if needed */
    if (fCoreRevision < 0x100) {
        IODelay(1);
    }
}

bool AppleMBX::waitForBit(uint32_t offset, uint32_t bit, bool set, uint32_t timeout)
{
    uint32_t ticks = 0;
    uint32_t delay = 10;  /* 10 us */
    
    while (ticks < (timeout * 1000)) {
        uint32_t value = readReg(offset);
        
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

#pragma mark - AppleMBX::Hardware Initialization

bool AppleMBX::mapRegisters(void)
{
    IOService* provider = getProvider();
    if (!provider) {
        IOLog("AppleMBX: No provider\n");
        return false;
    }
    
    IODeviceMemory* memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory) {
        IOLog("AppleMBX: No device memory\n");
        return false;
    }
    
    fPhysicalAddress = memory->getPhysicalAddress();
    fPhysicalLength = memory->getLength();
    
    fMemoryMap = memory->makeMapping(kIOMapAny | kIOMapInhibitCache);
    if (!fMemoryMap) {
        IOLog("AppleMBX: Failed to map registers\n");
        return false;
    }
    
    fRegisters = (volatile uint32_t*)fMemoryMap->getVirtualAddress();
    if (!fRegisters) {
        IOLog("AppleMBX: Invalid virtual address\n");
        fMemoryMap->release();
        fMemoryMap = NULL;
        return false;
    }
    
    IOLog("AppleMBX: Mapped registers at %p (phys: 0x%llx, size: %lld)\n",
          fRegisters, fPhysicalAddress, fPhysicalLength);
    
    return true;
}

void AppleMBX::unmapRegisters(void)
{
    if (fMemoryMap) {
        fMemoryMap->release();
        fMemoryMap = NULL;
        fRegisters = NULL;
    }
}

bool AppleMBX::resetController(void)
{
    IOLog("AppleMBX: Resetting graphics controller\n");
    
    /* Soft reset */
    writeReg(MBX_REG_CONTROL, MBX_CONTROL_SOFT_RESET);
    IODelay(10000);  /* 10ms */
    
    /* Clear reset */
    writeReg(MBX_REG_CONTROL, 0);
    IODelay(1000);
    
    /* Enable core */
    writeReg(MBX_REG_CONTROL, MBX_CONTROL_ENABLE);
    
    /* Wait for ready */
    if (!waitForBit(MBX_REG_STATUS, MBX_STATUS_READY, true, 100)) {
        IOLog("AppleMBX: Controller failed to become ready\n");
        return false;
    }
    
    IOLog("AppleMBX: Controller reset complete\n");
    
    return true;
}

bool AppleMBX::readHardwareVersion(void)
{
    fCoreID = readReg(MBX_REG_ID);
    fCoreRevision = readReg(MBX_REG_REVISION);
    
    IOLog("AppleMBX: Core ID: 0x%08x, Revision: 0x%08x\n", fCoreID, fCoreRevision);
    
    /* Decode features */
    if (fCoreRevision >= 0x200) {
        f2DAvailable = true;
        f3DAvailable = true;
        fDMAAvailable = true;
        fMMUEnabled = true;
        fMaxTextureSize = 2048;
        fVRAMSize = 32 * 1024 * 1024;
    } else if (fCoreRevision >= 0x100) {
        f2DAvailable = true;
        f3DAvailable = true;
        fDMAAvailable = true;
        fMMUEnabled = false;
        fMaxTextureSize = 1024;
        fVRAMSize = 16 * 1024 * 1024;
    } else {
        f2DAvailable = true;
        f3DAvailable = false;
        fDMAAvailable = false;
        fMMUEnabled = false;
        fMaxTextureSize = 512;
        fVRAMSize = 4 * 1024 * 1024;
    }
    
    IOLog("AppleMBX: 2D: %s, 3D: %s, DMA: %s, MMU: %s, VRAM: %dMB\n",
          f2DAvailable ? "yes" : "no",
          f3DAvailable ? "yes" : "no",
          fDMAAvailable ? "yes" : "no",
          fMMUEnabled ? "yes" : "no",
          fVRAMSize / (1024 * 1024));
    
    return true;
}

bool AppleMBX::initializeMemory(void)
{
    if (fMMUEnabled) {
        /* Enable MMU */
        writeReg(MBX_REG_MMU_CONTROL, MBX_MMU_ENABLE | MBX_MMU_PAGE_SIZE_4K);
        
        /* Allocate page table */
        fPageTableBase = fVRAMBase + fVRAMSize - PAGE_SIZE;
        
        writeReg(MBX_REG_MMU_PAGE_TABLE_BASE, fPageTableBase);
        
        IOLog("AppleMBX: MMU enabled, page table at 0x%llx\n", fPageTableBase);
    }
    
    /* Configure memory interface */
    writeReg(MBX_REG_MEMIF_BURST_SIZE, 16);  /* 16-word bursts */
    writeReg(MBX_REG_MEMIF_READ_QUEUE, 4);    /* 4-deep read queue */
    writeReg(MBX_REG_MEMIF_WRITE_QUEUE, 4);   /* 4-deep write queue */
    
    return true;
}

bool AppleMBX::initialize2DEngine(void)
{
    if (!f2DAvailable) {
        return false;
    }
    
    /* Create command buffer */
    if (!createCommandBuffer(&f2DCmdBuffer, 64 * 1024)) {
        IOLog("AppleMBX: Failed to create 2D command buffer\n");
        return false;
    }
    
    /* Enable 2D clock */
    uint32_t clock = readReg(MBX_REG_CLOCK_CONTROL);
    clock |= MBX_CLOCK_2D_ENABLE;
    writeReg(MBX_REG_CLOCK_CONTROL, clock);
    
    /* Reset 2D engine */
    writeReg(MBX_REG_2D_CONTROL, MBX_CONTROL_RESET);
    IODelay(1000);
    writeReg(MBX_REG_2D_CONTROL, 0);
    
    /* Check capabilities */
    f2DCapabilities = 0;
    
    IOLog("AppleMBX: 2D engine initialized\n");
    
    return true;
}

bool AppleMBX::initialize3DEngine(void)
{
    if (!f3DAvailable) {
        return false;
    }
    
    /* Create command buffer */
    if (!createCommandBuffer(&f3DCmdBuffer, 256 * 1024)) {
        IOLog("AppleMBX: Failed to create 3D command buffer\n");
        return false;
    }
    
    /* Allocate pipeline state */
    fPipelineState = (pipeline_state_t*)IOMalloc(sizeof(pipeline_state_t));
    if (!fPipelineState) {
        IOLog("AppleMBX: Failed to allocate pipeline state\n");
        return false;
    }
    
    bzero(fPipelineState, sizeof(pipeline_state_t));
    
    /* Enable 3D clock */
    uint32_t clock = readReg(MBX_REG_CLOCK_CONTROL);
    clock |= MBX_CLOCK_3D_ENABLE;
    writeReg(MBX_REG_CLOCK_CONTROL, clock);
    
    /* Reset 3D engine */
    writeReg(MBX_REG_TA_CONTROL, MBX_CONTROL_RESET);
    writeReg(MBX_REG_SP_CONTROL, MBX_CONTROL_RESET);
    IODelay(1000);
    writeReg(MBX_REG_TA_CONTROL, 0);
    writeReg(MBX_REG_SP_CONTROL, 0);
    
    /* Set default pipeline state */
    fPipelineState->depth_test = 1;
    fPipelineState->depth_func = 1;  /* Less */
    fPipelineState->cull_mode = 0;
    fPipelineState->front_face = 0;
    fPipelineState->polygon_mode = 0;
    fPipelineState->line_width = 1.0f;
    fPipelineState->point_size = 1.0f;
    
    /* Set default matrices */
    for (int i = 0; i < 16; i++) {
        fPipelineState->modelview_matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        fPipelineState->projection_matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        fPipelineState->texture_matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    
    IOLog("AppleMBX: 3D engine initialized\n");
    
    return true;
}

bool AppleMBX::initializeDisplay(void)
{
    /* Set default display mode */
    fDisplayWidth = 320;
    fDisplayHeight = 480;
    fDisplayStride = fDisplayWidth * 2;  /* RGB565 */
    fDisplayFormat = MBX_PIXEL_FORMAT_RGB565;
    
    /* Create framebuffer surface */
    fFramebuffer = createSurface(fDisplayWidth, fDisplayHeight,
                                  fDisplayFormat, SURFACE_FLAG_FRAMEBUFFER | SURFACE_FLAG_DISPLAY);
    
    if (!fFramebuffer) {
        IOLog("AppleMBX: Failed to create framebuffer\n");
        return false;
    }
    
    /* Configure display */
    writeReg(MBX_REG_DISPLAY_BASE, fFramebuffer->base_addr);
    writeReg(MBX_REG_DISPLAY_STRIDE, fFramebuffer->stride);
    writeReg(MBX_REG_DISPLAY_WIDTH, fDisplayWidth);
    writeReg(MBX_REG_DISPLAY_HEIGHT, fDisplayHeight);
    writeReg(MBX_REG_DISPLAY_FORMAT, fDisplayFormat);
    
    /* Set default timing (60Hz) */
    fTiming.width = fDisplayWidth;
    fTiming.height = fDisplayHeight;
    fTiming.h_front_porch = 16;
    fTiming.h_sync_width = 96;
    fTiming.h_back_porch = 48;
    fTiming.v_front_porch = 2;
    fTiming.v_sync_width = 2;
    fTiming.v_back_porch = 13;
    fTiming.h_polarity = 0;
    fTiming.v_polarity = 0;
    fTiming.pixel_clock = 25000;  /* 25 MHz */
    fTiming.refresh_rate = 60;
    
    setDisplayTiming(&fTiming);
    
    /* Enable display */
    writeReg(MBX_REG_DISPLAY_CONTROL, 1);
    fDisplayActive = true;
    
    IOLog("AppleMBX: Display initialized (%dx%d)\n", fDisplayWidth, fDisplayHeight);
    
    return true;
}

#pragma mark - AppleMBX::Command Buffer Management

bool AppleMBX::createCommandBuffer(cmd_buffer_t** buffer, uint32_t size)
{
    cmd_buffer_t* cmd;
    void* mem;
    IOBufferMemoryDescriptor* memDesc;
    
    cmd = (cmd_buffer_t*)IOMalloc(sizeof(cmd_buffer_t));
    if (!cmd) {
        return false;
    }
    
    bzero(cmd, sizeof(cmd_buffer_t));
    
    /* Allocate memory for commands */
    memDesc = IOBufferMemoryDescriptor::withOptions(
        kIODirectionOutIn | kIOMemoryPhysicallyContiguous,
        size, PAGE_SIZE);
    
    if (!memDesc) {
        IOFree(cmd, sizeof(cmd_buffer_t));
        return false;
    }
    
    memDesc->prepare();
    
    cmd->buffer = (uint32_t*)memDesc->getBytesNoCopy();
    cmd->size = size / 4;  /* In dwords */
    cmd->write_ptr = 0;
    cmd->read_ptr = 0;
    cmd->count = 0;
    cmd->phys_addr = memDesc->getPhysicalAddress();
    cmd->lock = lck_mtx_alloc_init(IOService::getKernelLockGroup(), LCK_ATTR_NULL);
    
    memDesc->complete();
    memDesc->release();
    
    *buffer = cmd;
    
    return true;
}

void AppleMBX::destroyCommandBuffer(cmd_buffer_t* buffer)
{
    if (buffer) {
        if (buffer->lock) {
            lck_mtx_free(buffer->lock, IOService::getKernelLockGroup());
        }
        
        /* Free memory descriptor */
        /* In real implementation, would free the actual memory */
        
        IOFree(buffer, sizeof(cmd_buffer_t));
    }
}

bool AppleMBX::submitCommands(cmd_buffer_t* buffer, bool wait)
{
    if (!buffer) {
        return false;
    }
    
    lck_mtx_lock(buffer->lock);
    
    /* Submit to hardware */
    /* In real implementation, would write to doorbell register */
    
    buffer->read_ptr = 0;
    buffer->count = buffer->write_ptr;
    
    lck_mtx_unlock(buffer->lock);
    
    if (wait) {
        waitForCommandCompletion(buffer, 1000);
    }
    
    return true;
}

uint32_t AppleMBX::waitForCommandCompletion(cmd_buffer_t* buffer, uint32_t timeout)
{
    uint32_t elapsed = 0;
    uint32_t delay = 10;  /* 10ms */
    
    while (elapsed < timeout) {
        /* Check if commands completed */
        uint32_t status = readReg(MBX_REG_2D_STATUS);
        
        if ((status & MBX_STATUS_2D_BUSY) == 0) {
            return 0;
        }
        
        IOSleep(delay);
        elapsed += delay;
    }
    
    return kIOReturnTimeout;
}

#pragma mark - AppleMBX::Surface Management

surface_t* AppleMBX::createSurface(uint32_t width, uint32_t height,
                                     uint32_t format, uint32_t flags)
{
    surface_t* surface;
    uint32_t bytesPerPixel = getPixelBytes(format);
    uint32_t stride = alignUp(width * bytesPerPixel, 64);  /* 64-byte aligned */
    uint32_t size = stride * height;
    uint32_t surfaceID;
    
    /* Check limits */
    if (width > fMaxWidth || height > fMaxHeight) {
        return NULL;
    }
    
    /* Allocate surface structure */
    surface = (surface_t*)IOMalloc(sizeof(surface_t));
    if (!surface) {
        return NULL;
    }
    
    bzero(surface, sizeof(surface_t));
    
    surface->width = width;
    surface->height = height;
    surface->stride = stride;
    surface->format = format;
    surface->size = size;
    surface->flags = flags;
    surface->refcount = 1;
    
    /* Allocate memory from VRAM */
    /* In real implementation, would allocate from VRAM heap */
    surface->base_addr = fVRAMBase + fNextSurfaceID * size;
    surface->virtual_addr = NULL;  /* Would map if needed */
    
    /* Add to dictionary */
    lck_mtx_lock(fSurfaceLock);
    
    surfaceID = fNextSurfaceID++;
    
    OSNumber* key = OSNumber::withNumber(surfaceID, 32);
    OSData* data = OSData::withBytes(&surface, sizeof(surface_t*));
    
    if (key && data) {
        fSurfaceDict->setObject(key, data);
    }
    
    if (key) key->release();
    if (data) data->release();
    
    lck_mtx_unlock(fSurfaceLock);
    
    /* Map to MMU if needed */
    if (fMMUEnabled) {
        mapSurface(surface);
    }
    
    return surface;
}

void AppleMBX::destroySurface(surface_t* surface)
{
    if (!surface) {
        return;
    }
    
    lck_mtx_lock(fSurfaceLock);
    
    /* Find and remove from dictionary */
    /* ... */
    
    lck_mtx_unlock(fSurfaceLock);
    
    /* Unmap from MMU */
    if (fMMUEnabled) {
        unmapSurface(surface);
    }
    
    /* Free memory */
    IOFree(surface, sizeof(surface_t));
}

surface_t* AppleMBX::findSurface(uint32_t surfaceID)
{
    surface_t* surface = NULL;
    
    lck_mtx_lock(fSurfaceLock);
    
    OSNumber* key = OSNumber::withNumber(surfaceID, 32);
    OSData* data = OSDynamicCast(OSData, fSurfaceDict->getObject(key));
    
    if (data && data->getLength() == sizeof(surface_t*)) {
        memcpy(&surface, data->getBytesNoCopy(), sizeof(surface_t*));
    }
    
    if (key) key->release();
    
    lck_mtx_unlock(fSurfaceLock);
    
    return surface;
}

bool AppleMBX::surfaceMakeResident(surface_t* surface)
{
    if (!surface) {
        return false;
    }
    
    /* Ensure surface is in VRAM and mapped */
    surface->refcount++;
    
    return true;
}

void AppleMBX::surfaceEvict(surface_t* surface)
{
    if (!surface) {
        return;
    }
    
    surface->refcount--;
    
    if (surface->refcount == 0) {
        /* Evict from VRAM if needed */
    }
}

bool AppleMBX::mapSurface(surface_t* surface)
{
    if (!surface || !fMMUEnabled) {
        return false;
    }
    
    /* Map surface into MMU address space */
    /* In real implementation, would program MMU page tables */
    
    return true;
}

void AppleMBX::unmapSurface(surface_t* surface)
{
    if (!surface || !fMMUEnabled) {
        return;
    }
    
    /* Unmap surface from MMU */
    /* In real implementation, would invalidate page table entries */
}

bool AppleMBX::handlePageFault(uint64_t address)
{
    fPageFaults++;
    
    /* Handle MMU page fault */
    IOLog("AppleMBX: Page fault at address 0x%llx\n", address);
    
    return false;  /* Unhandled */
}

#pragma mark - AppleMBX::Display Management

bool AppleMBX::setDisplayMode(uint32_t width, uint32_t height, uint32_t format)
{
    if (width > fMaxWidth || height > fMaxHeight) {
        return false;
    }
    
    fDisplayWidth = width;
    fDisplayHeight = height;
    fDisplayStride = width * getPixelBytes(format);
    fDisplayFormat = format;
    
    /* Update framebuffer if needed */
    if (fFramebuffer) {
        destroySurface(fFramebuffer);
    }
    
    fFramebuffer = createSurface(width, height, format, SURFACE_FLAG_FRAMEBUFFER | SURFACE_FLAG_DISPLAY);
    
    if (!fFramebuffer) {
        return false;
    }
    
    /* Update hardware */
    writeReg(MBX_REG_DISPLAY_BASE, fFramebuffer->base_addr);
    writeReg(MBX_REG_DISPLAY_STRIDE, fFramebuffer->stride);
    writeReg(MBX_REG_DISPLAY_WIDTH, width);
    writeReg(MBX_REG_DISPLAY_HEIGHT, height);
    writeReg(MBX_REG_DISPLAY_FORMAT, format);
    
    IOLog("AppleMBX: Display mode set to %dx%d\n", width, height);
    
    return true;
}

bool AppleMBX::setDisplayTiming(display_timing_t* timing)
{
    uint32_t h_total = timing->width + timing->h_front_porch + timing->h_sync_width + timing->h_back_porch;
    uint32_t v_total = timing->height + timing->v_front_porch + timing->v_sync_width + timing->v_back_porch;
    uint32_t h_timing = (timing->h_front_porch << 16) | (timing->h_sync_width << 8) | timing->h_back_porch;
    uint32_t v_timing = (timing->v_front_porch << 16) | (timing->v_sync_width << 8) | timing->v_back_porch;
    
    writeReg(MBX_REG_DISPLAY_TIMING_H, h_timing);
    writeReg(MBX_REG_DISPLAY_TIMING_V, v_timing);
    writeReg(MBX_REG_DISPLAY_HSYNC, timing->h_polarity);
    writeReg(MBX_REG_DISPLAY_VSYNC, timing->v_polarity);
    
    /* Set pixel clock (simplified) */
    /* In real implementation, would configure PLL */
    
    return true;
}

void AppleMBX::vBlankHandler(void)
{
    fVBlankCount++;
    
    /* Update performance counters */
    if (fPerformanceCounters[0]) {
        /* Update frame rate counter */
    }
    
    /* Trigger any pending swaps */
    /* Notify clients */
}

void AppleMBX::vBlankHandler(OSObject* owner, IOTimerEventSource* sender)
{
    AppleMBX* me = (AppleMBX*)owner;
    if (me) {
        me->vBlankHandler();
    }
}

#pragma mark - AppleMBX::2D Acceleration

bool AppleMBX::blit2D(surface_t* src, surface_t* dst,
                       uint32_t srcX, uint32_t srcY,
                       uint32_t dstX, uint32_t dstY,
                       uint32_t width, uint32_t height,
                       uint32_t rop)
{
    if (!src || !dst) {
        return false;
    }
    
    lck_mtx_lock(f2DLock);
    
    /* Ensure surfaces are resident */
    surfaceMakeResident(src);
    surfaceMakeResident(dst);
    
    /* Build command buffer */
    cmd_buffer_t* cmd = f2DCmdBuffer;
    
    cmd->buffer[cmd->write_ptr++] = 0x40000000 | (rop << 16) | 0x01;  /* BLIT command */
    cmd->buffer[cmd->write_ptr++] = src->base_addr + (srcY * src->stride) + (srcX * getPixelBytes(src->format));
    cmd->buffer[cmd->write_ptr++] = dst->base_addr + (dstY * dst->stride) + (dstX * getPixelBytes(dst->format));
    cmd->buffer[cmd->write_ptr++] = (height << 16) | width;
    cmd->buffer[cmd->write_ptr++] = (src->stride << 16) | dst->stride;
    
    /* Submit commands */
    submitCommands(cmd, true);
    
    fDrawCalls2D++;
    
    lck_mtx_unlock(f2DLock);
    
    return true;
}

bool AppleMBX::fill2D(surface_t* dst, uint32_t color,
                       uint32_t x, uint32_t y,
                       uint32_t width, uint32_t height)
{
    if (!dst) {
        return false;
    }
    
    lck_mtx_lock(f2DLock);
    
    surfaceMakeResident(dst);
    
    /* Build command buffer */
    cmd_buffer_t* cmd = f2DCmdBuffer;
    
    cmd->buffer[cmd->write_ptr++] = 0x48000000 | 0x01;  /* FILL command */
    cmd->buffer[cmd->write_ptr++] = color;
    cmd->buffer[cmd->write_ptr++] = dst->base_addr + (y * dst->stride) + (x * getPixelBytes(dst->format));
    cmd->buffer[cmd->write_ptr++] = (height << 16) | width;
    cmd->buffer[cmd->write_ptr++] = dst->stride;
    
    submitCommands(cmd, true);
    
    fDrawCalls2D++;
    
    lck_mtx_unlock(f2DLock);
    
    return true;
}

bool AppleMBX::copy2D(surface_t* dst, surface_t* src, uint32_t rop)
{
    return blit2D(src, dst, 0, 0, 0, 0, src->width, src->height, rop);
}

#pragma mark - AppleMBX::3D Acceleration

bool AppleMBX::set3DPipeline(pipeline_state_t* state)
{
    if (!state) {
        return false;
    }
    
    lck_mtx_lock(f3DLock);
    
    /* Copy state */
    memcpy(fPipelineState, state, sizeof(pipeline_state_t));
    
    /* Build command buffer */
    cmd_buffer_t* cmd = f3DCmdBuffer;
    
    cmd->write_ptr = 0;
    
    /* Set matrices */
    for (int i = 0; i < 16; i++) {
        if (fPipelineState->modelview_matrix[i] != 0.0f || fPipelineState->modelview_matrix[i] != 1.0f) {
            cmd->buffer[cmd->write_ptr++] = 0x50000000 | (i << 16) | 0x01;  /* SET_MODELVIEW */
            cmd->buffer[cmd->write_ptr++] = *(uint32_t*)&fPipelineState->modelview_matrix[i];
        }
    }
    
    /* Set viewport */
    cmd->buffer[cmd->write_ptr++] = 0x51000000 | 0x01;  /* SET_VIEWPORT */
    cmd->buffer[cmd->write_ptr++] = (fPipelineState->viewport_y << 16) | fPipelineState->viewport_x;
    cmd->buffer[cmd->write_ptr++] = (fPipelineState->viewport_height << 16) | fPipelineState->viewport_width;
    
    /* Set depth test */
    cmd->buffer[cmd->write_ptr++] = 0x52000000 | (fPipelineState->depth_func << 8) | (fPipelineState->depth_test << 16) | 0x01;
    
    /* Set cull mode */
    cmd->buffer[cmd->write_ptr++] = 0x53000000 | (fPipelineState->cull_mode << 8) | 0x01;
    
    submitCommands(cmd, false);
    
    lck_mtx_unlock(f3DLock);
    
    return true;
}

bool AppleMBX::drawTriangles(uint32_t vertexCount, uint32_t indexCount)
{
    lck_mtx_lock(f3DLock);
    
    cmd_buffer_t* cmd = f3DCmdBuffer;
    
    cmd->buffer[cmd->write_ptr++] = 0x60000000 | 0x01;  /* DRAW_TRIANGLES */
    cmd->buffer[cmd->write_ptr++] = vertexCount;
    cmd->buffer[cmd->write_ptr++] = indexCount;
    
    submitCommands(cmd, true);
    
    fDrawCalls3D++;
    fTrianglesRendered += indexCount / 3;
    
    lck_mtx_unlock(f3DLock);
    
    return true;
}

bool AppleMBX::drawArrays(uint32_t mode, uint32_t first, uint32_t count)
{
    lck_mtx_lock(f3DLock);
    
    cmd_buffer_t* cmd = f3DCmdBuffer;
    
    cmd->buffer[cmd->write_ptr++] = 0x61000000 | (mode << 8) | 0x01;  /* DRAW_ARRAYS */
    cmd->buffer[cmd->write_ptr++] = first;
    cmd->buffer[cmd->write_ptr++] = count;
    
    submitCommands(cmd, true);
    
    fDrawCalls3D++;
    fVerticesProcessed += count;
    
    lck_mtx_unlock(f3DLock);
    
    return true;
}

bool AppleMBX::drawElements(uint32_t mode, uint32_t count, uint32_t type, void* indices)
{
    lck_mtx_lock(f3DLock);
    
    cmd_buffer_t* cmd = f3DCmdBuffer;
    
    cmd->buffer[cmd->write_ptr++] = 0x62000000 | (mode << 8) | (type << 16) | 0x01;  /* DRAW_ELEMENTS */
    cmd->buffer[cmd->write_ptr++] = count;
    cmd->buffer[cmd->write_ptr++] = (uint32_t)(uintptr_t)indices;
    
    submitCommands(cmd, true);
    
    fDrawCalls3D++;
    fVerticesProcessed += count;
    
    lck_mtx_unlock(f3DLock);
    
    return true;
}

#pragma mark - AppleMBX::Interrupt Handling

void AppleMBX::interruptHandler(void)
{
    uint32_t intStatus;
    
    fIRQCount++;
    
    /* Read interrupt status */
    intStatus = readReg(MBX_REG_INTERRUPT_STATUS);
    
    /* Clear interrupts */
    writeReg(MBX_REG_INTERRUPT_CLEAR, intStatus);
    
    /* Handle VSYNC */
    if (intStatus & MBX_INTR_VSYNC) {
        vBlankHandler();
    }
    
    /* Handle 2D completion */
    if (intStatus & MBX_INTR_2D_COMPLETE) {
        /* Wake waiting threads */
    }
    
    /* Handle 3D completion */
    if (intStatus & MBX_INTR_3D_COMPLETE) {
        /* Wake waiting threads */
    }
    
    /* Handle errors */
    if (intStatus & (MBX_INTR_ERROR | MBX_INTR_MMU_PAGE_FAULT | MBX_INTR_BUS_ERROR)) {
        fErrors++;
        
        if (intStatus & MBX_INTR_MMU_PAGE_FAULT) {
            uint64_t faultAddr = readReg(MBX_REG_MMU_STATUS);  /* Simplified */
            handlePageFault(faultAddr);
        }
    }
}

void AppleMBX::interruptHandler(OSObject* owner, IOInterruptEventSource* src, int count)
{
    AppleMBX* me = (AppleMBX*)owner;
    if (me) {
        me->interruptHandler();
    }
}

#pragma mark - AppleMBX::Power Management

void AppleMBX::suspendDevice(void)
{
    if (fSuspended) {
        return;
    }
    
    IOLog("AppleMBX: Suspending graphics controller\n");
    
    /* Save context */
    saveContext();
    
    /* Disable interrupts */
    writeReg(MBX_REG_INTERRUPT_ENABLE, 0);
    
    /* Put core to sleep */
    writeReg(MBX_REG_POWER_CONTROL, MBX_POWER_SLEEP);
    
    fSuspended = true;
}

void AppleMBX::resumeDevice(void)
{
    if (!fSuspended) {
        return;
    }
    
    IOLog("AppleMBX: Resuming graphics controller\n");
    
    /* Wake core */
    writeReg(MBX_REG_POWER_CONTROL, MBX_POWER_ON);
    
    /* Wait for ready */
    waitForBit(MBX_REG_STATUS, MBX_STATUS_READY, true, 100);
    
    /* Restore context */
    restoreContext();
    
    /* Re-enable interrupts */
    writeReg(MBX_REG_INTERRUPT_ENABLE,
            MBX_INTR_VSYNC | MBX_INTR_2D_COMPLETE | MBX_INTR_3D_COMPLETE);
    
    fSuspended = false;
}

bool AppleMBX::saveContext(void)
{
    /* Save critical registers */
    /* In real implementation, would save all context */
    
    return true;
}

bool AppleMBX::restoreContext(void)
{
    /* Restore critical registers */
    
    /* Display */
    if (fFramebuffer) {
        writeReg(MBX_REG_DISPLAY_BASE, fFramebuffer->base_addr);
        writeReg(MBX_REG_DISPLAY_STRIDE, fFramebuffer->stride);
        writeReg(MBX_REG_DISPLAY_WIDTH, fDisplayWidth);
        writeReg(MBX_REG_DISPLAY_HEIGHT, fDisplayHeight);
        writeReg(MBX_REG_DISPLAY_FORMAT, fDisplayFormat);
    }
    
    /* 2D engine */
    if (f2DAvailable) {
        writeReg(MBX_REG_2D_CONTROL, 0);
    }
    
    /* 3D engine */
    if (f3DAvailable && fPipelineState) {
        set3DPipeline(fPipelineState);
    }
    
    return true;
}

#pragma mark - AppleMBX::Utility Methods

uint64_t AppleMBX::getTimeUptime(void)
{
    uint64_t time;
    clock_get_uptime(&time);
    return time;
}

uint32_t AppleMBX::getPixelBytes(uint32_t format)
{
    switch (format) {
        case MBX_PIXEL_FORMAT_RGB565:
            return 2;
        case MBX_PIXEL_FORMAT_RGB888:
            return 3;
        case MBX_PIXEL_FORMAT_ARGB8888:
        case MBX_PIXEL_FORMAT_ABGR8888:
            return 4;
        case MBX_PIXEL_FORMAT_YUV420:
        case MBX_PIXEL_FORMAT_YVU420:
        case MBX_PIXEL_FORMAT_NV12:
        case MBX_PIXEL_FORMAT_NV21:
            return 1;  /* Actually 12 bits per pixel, but simplified */
        default:
            return 2;
    }
}

uint32_t AppleMBX::alignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

#pragma mark - AppleMBX::IOService Overrides

bool AppleMBX::start(IOService* provider)
{
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("AppleMBX: Starting MBX Graphics Controller\n");
    
    /* Map device registers */
    if (!mapRegisters()) {
        IOLog("AppleMBX: Failed to map registers\n");
        return false;
    }
    
    /* Read hardware version */
    readHardwareVersion();
    
    /* Reset controller */
    if (!resetController()) {
        IOLog("AppleMBX: Failed to reset controller\n");
        unmapRegisters();
        return false;
    }
    
    /* Initialize memory system */
    if (!initializeMemory()) {
        IOLog("AppleMBX: Failed to initialize memory\n");
        unmapRegisters();
        return false;
    }
    
    /* Create work loop */
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        IOLog("AppleMBX: Failed to create work loop\n");
        unmapRegisters();
        return false;
    }
    
    /* Create command gate */
    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        IOLog("AppleMBX: Failed to create command gate\n");
        fWorkLoop->release();
        fWorkLoop = NULL;
        unmapRegisters();
        return false;
    }
    
    fWorkLoop->addEventSource(fCommandGate);
    
    /* Create VBlank timer */
    fVBlankTimer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &AppleMBX::vBlankHandler));
    
    if (fVBlankTimer) {
        fWorkLoop->addEventSource(fVBlankTimer);
        fVBlankTimer->setTimeoutMS(16);  /* ~60Hz */
    }
    
    /* Create interrupt source */
    fInterruptSource = IOInterruptEventSource::interruptEventSource(this,
        OSMemberFunctionCast(IOInterruptEventAction, this,
                             &AppleMBX::interruptHandler),
        provider, 0);
    
    if (fInterruptSource) {
        fWorkLoop->addEventSource(fInterruptSource);
    }
    
    /* Initialize 2D engine */
    if (f2DAvailable) {
        initialize2DEngine();
    }
    
    /* Initialize 3D engine */
    if (f3DAvailable) {
        initialize3DEngine();
    }
    
    /* Initialize display */
    if (!initializeDisplay()) {
        IOLog("AppleMBX: Failed to initialize display\n");
        destroyWorkLoop();
        unmapRegisters();
        return false;
    }
    
    /* Set up framebuffer info for IOFramebuffer */
    fFBInfo.version = kIOFramebufferInfoVersion;
    fFBInfo.baseAddress = (IOVirtualAddress)fFramebuffer->virtual_addr;
    fFBInfo.rowBytes = fFramebuffer->stride;
    fFBInfo.width = fDisplayWidth;
    fFBInfo.height = fDisplayHeight;
    fFBInfo.depth = getPixelBytes(fDisplayFormat) * 8;
    
    fDisplayMode.version = kIODisplayModeInformationVersion;
    fDisplayMode.nominalWidth = fDisplayWidth;
    fDisplayMode.nominalHeight = fDisplayHeight;
    fDisplayMode.refreshRate = fTiming.refresh_rate << 16;
    fDisplayMode.maxDepthIndex = 0;
    fDisplayMode.flags = 0;
    
    fPixelInfo.version = kIOPixelInformationVersion;
    fPixelInfo.bytesPerRow = fFramebuffer->stride;
    fPixelInfo.bytesPerPlane = 0;
    fPixelInfo.componentCount = 3;
    fPixelInfo.bitsPerPixel = getPixelBytes(fDisplayFormat) * 8;
    fPixelInfo.componentMasks[0] = 0x00FF0000;
    fPixelInfo.componentMasks[1] = 0x0000FF00;
    fPixelInfo.componentMasks[2] = 0x000000FF;
    
    /* Register power management */
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, NULL, 0);
    
    /* Publish properties */
    setProperty("CoreID", fCoreID, 32);
    setProperty("CoreRevision", fCoreRevision, 32);
    setProperty("VRAMSize", fVRAMSize, 32);
    setProperty("MaxWidth", fMaxWidth, 32);
    setProperty("MaxHeight", fMaxHeight, 32);
    setProperty("MaxTextureSize", fMaxTextureSize, 32);
    setProperty("2DAvailable", f2DAvailable ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("3DAvailable", f3DAvailable ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("DisplayWidth", fDisplayWidth, 32);
    setProperty("DisplayHeight", fDisplayHeight, 32);
    
    /* Register service */
    registerService();
    
    IOLog("AppleMBX: Started successfully\n");
    
    return true;
}

void AppleMBX::stop(IOService* provider)
{
    IOLog("AppleMBX: Stopping\n");
    
    /* Disable display */
    writeReg(MBX_REG_DISPLAY_CONTROL, 0);
    
    /* Disable interrupts */
    if (fRegisters) {
        writeReg(MBX_REG_INTERRUPT_ENABLE, 0);
    }
    
    /* Remove interrupt source */
    if (fWorkLoop && fInterruptSource) {
        fWorkLoop->removeEventSource(fInterruptSource);
        fInterruptSource->release();
        fInterruptSource = NULL;
    }
    
    /* Remove VBlank timer */
    if (fWorkLoop && fVBlankTimer) {
        fVBlankTimer->cancelTimeout();
        fWorkLoop->removeEventSource(fVBlankTimer);
        fVBlankTimer->release();
        fVBlankTimer = NULL;
    }
    
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

#pragma mark - AppleMBX::IOFramebuffer Overrides

IOReturn AppleMBX::enableController(void)
{
    IOLog("AppleMBX: Enabling controller\n");
    
    writeReg(MBX_REG_DISPLAY_CONTROL, 1);
    fDisplayActive = true;
    
    return kIOReturnSuccess;
}

IOReturn AppleMBX::disableController(void)
{
    IOLog("AppleMBX: Disabling controller\n");
    
    writeReg(MBX_REG_DISPLAY_CONTROL, 0);
    fDisplayActive = false;
    
    return kIOReturnSuccess;
}

IOReturn AppleMBX::setDisplayMode(IODisplayModeID mode, IOIndex depth)
{
    /* In real implementation, would set based on mode/depth */
    return kIOReturnSuccess;
}

IOReturn AppleMBX::setStartupDisplayMode(IODisplayModeID mode, IOIndex depth)
{
    return setDisplayMode(mode, depth);
}

IOReturn AppleMBX::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation* info)
{
    if (!info) {
        return kIOReturnBadArgument;
    }
    
    memcpy(info, &fDisplayMode, sizeof(fDisplayMode));
    
    return kIOReturnSuccess;
}

IOReturn AppleMBX::getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation* info)
{
    if (!info) {
        return kIOReturnBadArgument;
    }
    
    memcpy(info, &fPixelInfo, sizeof(fPixelInfo));
    
    return kIOReturnSuccess;
}

IOItemCount AppleMBX::getDisplayModeCount(void)
{
    return 1;  /* Single mode in this simplified version */
}

IODisplayModeID AppleMBX::getCurrentDisplayMode(void)
{
    return 0;  /* Default mode */
}

IOIndex AppleMBX::getCurrentDepth(void)
{
    return 0;  /* Default depth */
}

IOReturn AppleMBX::getFramebufferInfo(IOFramebufferInfo* info)
{
    if (!info) {
        return kIOReturnBadArgument;
    }
    
    memcpy(info, &fFBInfo, sizeof(fFBInfo));
    
    return kIOReturnSuccess;
}

IOReturn AppleMBX::setCLUT(IOColorEntry* colors, UInt32 index, UInt32 num)
{
    if (!colors || index + num > CLUT_SIZE) {
        return kIOReturnBadArgument;
    }
    
    for (UInt32 i = 0; i < num; i++) {
        fCLUT.red[index + i] = colors[i].red;
        fCLUT.green[index + i] = colors[i].green;
        fCLUT.blue[index + i] = colors[i].blue;
    }
    
    /* Update hardware CLUT */
    /* In real implementation, would write to gamma table */
    
    return kIOReturnSuccess;
}

IOReturn AppleMBX::getCLUT(IOColorEntry* colors, UInt32 index, UInt32 num)
{
    if (!colors || index + num > CLUT_SIZE) {
        return kIOReturnBadArgument;
    }
    
    for (UInt32 i = 0; i < num; i++) {
        colors[i].red = fCLUT.red[index + i];
        colors[i].green = fCLUT.green[index + i];
        colors[i].blue = fCLUT.blue[index + i];
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleMBX::setBrightness(UInt32 brightness)
{
    if (brightness > 255) {
        return kIOReturnBadArgument;
    }
    
    fBrightness = brightness;
    
    /* Update hardware brightness */
    writeReg(MBX_REG_DISPLAY_BRIGHTNESS, brightness);
    
    return kIOReturnSuccess;
}

IOReturn AppleMBX::getBrightness(UInt32* brightness)
{
    if (!brightness) {
        return kIOReturnBadArgument;
    }
    
    *brightness = fBrightness;
    
    return kIOReturnSuccess;
}

#pragma mark - AppleMBX::IOGraphicsAccelerator Overrides

IOReturn AppleMBX::performAccelOperation(UInt32 operation, void* reference, void* arguments)
{
    /* Handle accelerated graphics operations */
    /* This would be called from user space via IOUserClient */
    
    switch (operation) {
        case 0x01: {  /* 2D BLIT */
            /* Parse arguments and call blit2D */
            break;
        }
        case 0x02: {  /* 3D DRAW */
            /* Parse arguments and call drawTriangles */
            break;
        }
        default:
            return kIOReturnUnsupported;
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleMBX::completeAccelOperation(UInt32 operation, void* reference, void* arguments)
{
    /* Complete asynchronous operation */
    return kIOReturnSuccess;
}

#pragma mark - AppleMBX::Power Management

IOReturn AppleMBX::setPowerState(unsigned long powerState, IOService* device)
{
    if (powerState == 0) {
        /* Going to sleep */
        suspendDevice();
    } else {
        /* Waking up */
        resumeDevice();
    }
    
    fPowerState = powerState;
    
    return IOPMAckImplied;
}

IOReturn AppleMBX::powerStateWillChangeTo(IOPMPowerFlags capabilities, unsigned long stateNumber, IOService* whatDevice)
{
    return IOPMAckImplied;
}

#pragma mark - AppleMBX::User Client Access

IOReturn AppleMBX::newUserClient(task_t owningTask,
                                   void* securityID,
                                   UInt32 type,
                                   OSDictionary* properties,
                                   IOUserClient** handler)
{
    /* Create user client for graphics acceleration */
    /* Would return AppleMBXUserClient instance */
    
    return kIOReturnUnsupported;
}
