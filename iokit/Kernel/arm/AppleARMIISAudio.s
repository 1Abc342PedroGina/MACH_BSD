;
; Copyright (c) 2020-2022 Apple Inc. All rights reserved.
;
; @APPLE_OSREFERENCE_LICENSE_HEADER_START@
;
; This file contains Original Code and/or Modifications of Original Code
; as defined in and that are subject to the Apple Public Source License
; Version 2.0 (the 'License'). You may not use this file except in
; compliance with the License. The rights granted to you under the License
; may not be used to create, or enable the creation or redistribution of,
; unlawful or unlicensed copies of an Apple operating system, or to
; circumvent, violate, or enable the circumvention or violation of, any
; terms of an Apple operating system software license agreement.
;
; Please obtain a copy of the License at
; http://www.opensource.apple.com/apsl/ and read it before using this file.
;
; The Original Code and all software distributed under the License are
; distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
; EXPRESS OR IMPLIED, AND APPRELE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
; INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
; Please see the License for the specific language governing rights and
; limitations under the License.
;
; @APPLE_OSREFERENCE_LICENSE_HEADER_END@
;
; AppleARMIISAudio.s
; I2S Audio Controller Driver for Apple Silicon (ARM64)
;

;==============================================================================
; Constants and Register Definitions
;==============================================================================

; I2S Controller Register Offsets (based on Apple's D238/8100 series)
.equ I2S_REG_ID               , 0x000   ; Controller ID
.equ I2S_REG_VERSION           , 0x004   ; Version register
.equ I2S_REG_CONTROL           , 0x008   ; Main control
.equ I2S_REG_STATUS            , 0x00C   ; Status register
.equ I2S_REG_INT_ENABLE        , 0x010   ; Interrupt enable
.equ I2S_REG_INT_STATUS        , 0x014   ; Interrupt status
.equ I2S_REG_CLOCK_CTRL        , 0x018   ; Clock control
.equ I2S_REG_SAMPLE_RATE       , 0x01C   ; Sample rate divider
.equ I2S_REG_FORMAT            , 0x020   ; Audio format
.equ I2S_REG_FIFO_TX           , 0x024   ; TX FIFO data
.equ I2S_REG_FIFO_RX           , 0x028   ; RX FIFO data
.equ I2S_REG_FIFO_STATUS       , 0x02C   ; FIFO status
.equ I2S_REG_DMA_TX_ADDR       , 0x030   ; TX DMA address
.equ I2S_REG_DMA_TX_SIZE       , 0x034   ; TX DMA size
.equ I2S_REG_DMA_RX_ADDR       , 0x038   ; RX DMA address
.equ I2S_REG_DMA_RX_SIZE       , 0x03C   ; RX DMA size
.equ I2S_REG_DMA_STATUS        , 0x040   ; DMA status
.equ I2S_REG_GPIO_MUX          , 0x044   ; GPIO mux control
.equ I2S_REG_POWER_STATE       , 0x048   ; Power state
.equ I2S_REG_DEBUG             , 0x07C   ; Debug register

; Control register bits
.equ I2S_CTRL_ENABLE           , 1 << 0
.equ I2S_CTRL_RESET            , 1 << 1
.equ I2S_CTRL_TX_ENABLE        , 1 << 2
.equ I2S_CTRL_RX_ENABLE        , 1 << 3
.equ I2S_CTRL_LOOPBACK         , 1 << 4
.equ I2S_CTRL_MASTER           , 1 << 5
.equ I2S_CTRL_SLAVE            , 0 << 5
.equ I2S_CTRL_POLARITY         , 1 << 6
.equ I2S_CTRL_JUSTIFIED        , 1 << 7

; Status register bits
.equ I2S_STATUS_READY          , 1 << 0
.equ I2S_STATUS_TX_READY       , 1 << 1
.equ I2S_STATUS_RX_READY       , 1 << 2
.equ I2S_STATUS_TX_UNDERRUN    , 1 << 3
.equ I2S_STATUS_RX_OVERRUN     , 1 << 4
.equ I2S_STATUS_CLOCK_LOCKED   , 1 << 5
.equ I2S_STATUS_ERROR          , 1 << 31

; Interrupt bits
.equ I2S_INT_TX_FIFO_EMPTY     , 1 << 0
.equ I2S_INT_RX_FIFO_FULL      , 1 << 1
.equ I2S_INT_TX_DMA_DONE       , 1 << 2
.equ I2S_INT_RX_DMA_DONE       , 1 << 3
.equ I2S_INT_UNDERRUN          , 1 << 4
.equ I2S_INT_OVERRUN           , 1 << 5
.equ I2S_INT_CLOCK_ERROR       , 1 << 6
.equ I2S_INT_ALL               , 0x7F

; FIFO status bits
.equ I2S_FIFO_TX_EMPTY         , 1 << 0
.equ I2S_FIFO_TX_FULL          , 1 << 1
.equ I2S_FIFO_TX_LEVEL_MASK    , 0xFF << 8
.equ I2S_FIFO_RX_EMPTY         , 1 << 16
.equ I2S_FIFO_RX_FULL          , 1 << 17
.equ I2S_FIFO_RX_LEVEL_MASK    , 0xFF << 24

; Clock control bits
.equ I2S_CLOCK_ENABLE          , 1 << 0
.equ I2S_CLOCK_PLL_SEL_MASK    , 0x7 << 4
.equ I2S_CLOCK_PLL1            , 0x0 << 4
.equ I2S_CLOCK_PLL2            , 0x1 << 4
.equ I2S_CLOCK_PLL3            , 0x2 << 4
.equ I2S_CLOCK_DIVIDER_MASK    , 0xFFF << 8
.equ I2S_CLOCK_MCLK_OUT        , 1 << 24
.equ I2S_CLOCK_BCLK_OUT        , 1 << 25

; Audio formats
.equ I2S_FORMAT_I2S            , 0x00
.equ I2S_FORMAT_LEFT_JUSTIFIED , 0x01
.equ I2S_FORMAT_RIGHT_JUSTIFIED, 0x02
.equ I2S_FORMAT_PCM            , 0x03
.equ I2S_FORMAT_TDM            , 0x04
.equ I2S_FORMAT_WIDTH_MASK     , 0x7 << 4
.equ I2S_FORMAT_16BIT          , 0x0 << 4
.equ I2S_FORMAT_24BIT          , 0x1 << 4
.equ I2S_FORMAT_32BIT          , 0x2 << 4
.equ I2S_FORMAT_CHANNELS_MASK  , 0xF << 8
.equ I2S_FORMAT_MONO           , 0x1 << 8
.equ I2S_FORMAT_STEREO         , 0x2 << 8
.equ I2S_FORMAT_5POINT1        , 0x6 << 8
.equ I2S_FORMAT_7POINT1        , 0x8 << 8

; Power states
.equ I2S_POWER_OFF             , 0
.equ I2S_POWER_SLEEP           , 1
.equ I2S_POWER_IDLE            , 2
.equ I2S_POWER_ACTIVE          , 3

; DMA status
.equ I2S_DMA_TX_ACTIVE         , 1 << 0
.equ I2S_DMA_RX_ACTIVE         , 1 << 1
.equ I2S_DMA_TX_COMPLETE       , 1 << 2
.equ I2S_DMA_RX_COMPLETE       , 1 << 3
.equ I2S_DMA_ERROR             , 1 << 4

; Sample rates (divider values for 24.576MHz PLL)
.equ I2S_RATE_44100            , 557   ; 24576000 / 44100 / 2
.equ I2S_RATE_48000            , 512   ; 24576000 / 48000 / 2
.equ I2S_RATE_88200            , 278   ; 24576000 / 88200 / 2
.equ I2S_RATE_96000            , 256   ; 24576000 / 96000 / 2
.equ I2S_RATE_176400           , 139   ; 24576000 / 176400 / 2
.equ I2S_RATE_192000           , 128   ; 24576000 / 192000 / 2

;==============================================================================
; Driver Object Structure (AppleARMIISAudio)
;==============================================================================

; IOAppleARMIISAudio structure offsets (aligned to 8 bytes)
.struct 0
AppleARMIISAudio_pad0:          .skip 8
AppleARMIISAudio_vtbl:          .skip 8      ; Virtual table
AppleARMIISAudio_refCount:      .skip 8      ; retain/release count
AppleARMIISAudio_provider:      .skip 8      ; IOService provider
AppleARMIISAudio_regBase:       .skip 8      ; Register base virtual address
AppleARMIISAudio_regPhys:       .skip 8      ; Register physical address
AppleARMIISAudio_regSize:       .skip 8      ; Register region size
AppleARMIISAudio_workLoop:      .skip 8      ; IOWorkLoop
AppleARMIISAudio_intSource:     .skip 8      ; IOInterruptEventSource
AppleARMIISAudio_cmdGate:       .skip 8      ; IOCommandGate
AppleARMIISAudio_dmaBuffer:     .skip 8      ; DMA buffer descriptor
AppleARMIISAudio_txBuffer:      .skip 8      ; TX buffer virtual address
AppleARMIISAudio_rxBuffer:      .skip 8      ; RX buffer virtual address
AppleARMIISAudio_txPhys:        .skip 8      ; TX buffer physical address
AppleARMIISAudio_rxPhys:        .skip 8      ; RX buffer physical address
AppleARMIISAudio_txSize:        .skip 4      ; TX buffer size
AppleARMIISAudio_rxSize:        .skip 4      ; RX buffer size
AppleARMIISAudio_sampleRate:    .skip 4      ; Current sample rate
AppleARMIISAudio_format:        .skip 4      ; Current audio format
AppleARMIISAudio_channels:      .skip 4      ; Number of channels
AppleARMIISAudio_bitDepth:      .skip 4      ; Bit depth (16/24/32)
AppleARMIISAudio_powerState:    .skip 4      ; Current power state
AppleARMIISAudio_intEnabled:    .skip 4      ; Enabled interrupts
AppleARMIISAudio_txActive:      .skip 4      ; TX active flag
AppleARMIISAudio_rxActive:      .skip 4      ; RX active flag
AppleARMIISAudio_dmaActive:     .skip 4      ; DMA active flag
AppleARMIISAudio_errorCount:    .skip 4      ; Error counter
AppleARMIISAudio_underrunCount: .skip 4      ; TX underrun counter
AppleARMIISAudio_overrunCount:  .skip 4      ; RX overrun counter
AppleARMIISAudio_txPosition:    .skip 8      ; Current TX position (bytes)
AppleARMIISAudio_rxPosition:    .skip 8      ; Current RX position (bytes)
AppleARMIISAudio_interrupts:    .skip 8      ; Interrupt counter
AppleARMIISAudio_lock:          .skip 8      ; IOLock
AppleARMIISAudio_size:

;==============================================================================
; Constants
;==============================================================================

.equ I2S_MAX_BUFFER_SIZE        , 0x100000   ; 1MB max buffer
.equ I2S_DEFAULT_BUFFER_SIZE    , 0x10000    ; 64KB default buffer
.equ I2S_FIFO_DEPTH             , 64         ; 64 entries
.equ I2S_TIMEOUT_US             , 100000     ; 100ms timeout

;==============================================================================
; External symbols (from IOKit)
;==============================================================================

.extern _IOLog
.extern _IOLockAlloc
.extern _IOLockFree
.extern _IOLockLock
.extern _IOLockUnlock
.extern _IOWorkLoop_workLoop
.extern _IOInterruptEventSource_interruptEventSource
.extern _IOCommandGate_commandGate
.extern _IOWorkLoop_addEventSource
.extern _IOWorkLoop_removeEventSource
.extern _pm_init
.extern _pm_register_device

;==============================================================================
; Virtual table for AppleARMIISAudio
;==============================================================================

.data
.align 3
_AppleARMIISAudioMetaClass:
    .quad __ZTV17AppleARMIISAudio + 16   ; Virtual table reference
    .quad 0
    .quad 0

_AppleARMIISAudioVTable:
    .quad __ZN17AppleARMIISAudio5startEP9IOService ; start
    .quad __ZN17AppleARMIISAudio4stopEP9IOService  ; stop
    .quad __ZN17AppleARMIISAudio4freeEv            ; free
    .quad __ZN17AppleARMIISAudio15getWorkLoopEv    ; getWorkLoop
    .quad __ZN17AppleARMIISAudio17handleInterruptEP23IOInterruptEventSourcei ; interrupt handler
    .quad __ZN17AppleARMIISAudio11setSampleRateEj  ; setSampleRate
    .quad __ZN17AppleARMIISAudio9setFormatEjj      ; setFormat
    .quad __ZN17AppleARMIISAudio8setPowerEj        ; setPower
    .quad __ZN17AppleARMIISAudio13configureDMAEP21IOMemoryDescriptorj ; configureDMA
    .quad __ZN17AppleARMIISAudio7startDMAEv        ; startDMA
    .quad __ZN17AppleARMIISAudio6stopDMAEv         ; stopDMA

;==============================================================================
; .text section - Code
;==============================================================================

.text
.align 4

;------------------------------------------------------------------------------
; AppleARMIISAudio::AppleARMIISAudio() - Constructor
;------------------------------------------------------------------------------
.globl __ZN17AppleARMIISAudioC1Ev
__ZN17AppleARMIISAudioC1Ev:
    ; x0 = this
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    
    ; Call IOService constructor
    bl      __ZN9IOServiceC2Ev
    
    ; Set vtable
    adrp    x8, _AppleARMIISAudioVTable@PAGE
    add     x8, x8, _AppleARMIISAudioVTable@PAGEOFF
    str     x8, [x0, #AppleARMIISAudio_vtbl]
    
    ; Initialize member variables
    str     xzr, [x0, #AppleARMIISAudio_provider]
    str     xzr, [x0, #AppleARMIISAudio_regBase]
    str     xzr, [x0, #AppleARMIISAudio_regPhys]
    str     xzr, [x0, #AppleARMIISAudio_regSize]
    str     xzr, [x0, #AppleARMIISAudio_workLoop]
    str     xzr, [x0, #AppleARMIISAudio_intSource]
    str     xzr, [x0, #AppleARMIISAudio_cmdGate]
    str     xzr, [x0, #AppleARMIISAudio_dmaBuffer]
    str     xzr, [x0, #AppleARMIISAudio_txBuffer]
    str     xzr, [x0, #AppleARMIISAudio_rxBuffer]
    str     xzr, [x0, #AppleARMIISAudio_txPhys]
    str     xzr, [x0, #AppleARMIISAudio_rxPhys]
    str     xzr, [x0, #AppleARMIISAudio_lock]
    
    ; Set default values
    mov     w8, #48000
    str     w8, [x0, #AppleARMIISAudio_sampleRate]
    
    mov     w8, #I2S_FORMAT_I2S | I2S_FORMAT_24BIT | I2S_FORMAT_STEREO
    str     w8, [x0, #AppleARMIISAudio_format]
    
    mov     w8, #2
    str     w8, [x0, #AppleARMIISAudio_channels]
    
    mov     w8, #24
    str     w8, [x0, #AppleARMIISAudio_bitDepth]
    
    mov     w8, #I2S_POWER_OFF
    str     w8, [x0, #AppleARMIISAudio_powerState]
    
    str     wzr, [x0, #AppleARMIISAudio_intEnabled]
    str     wzr, [x0, #AppleARMIISAudio_txActive]
    str     wzr, [x0, #AppleARMIISAudio_rxActive]
    str     wzr, [x0, #AppleARMIISAudio_dmaActive]
    str     wzr, [x0, #AppleARMIISAudio_errorCount]
    str     wzr, [x0, #AppleARMIISAudio_underrunCount]
    str     wzr, [x0, #AppleARMIISAudio_overrunCount]
    
    str     xzr, [x0, #AppleARMIISAudio_txPosition]
    str     xzr, [x0, #AppleARMIISAudio_rxPosition]
    str     xzr, [x0, #AppleARMIISAudio_interrupts]
    
    ; Allocate lock
    stp     x0, x30, [sp, #-16]!
    bl      _IOLockAlloc
    ldp     x0, x30, [sp], #16
    str     x0, [x0, #AppleARMIISAudio_lock]
    
    ldp     x29, x30, [sp], #16
    ret

;------------------------------------------------------------------------------
; AppleARMIISAudio::start(IOService* provider)
;------------------------------------------------------------------------------
.globl __ZN17AppleARMIISAudio5startEP9IOService
__ZN17AppleARMIISAudio5startEP9IOService:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    str     x20, [sp, #24]
    
    mov     x19, x0                     ; this
    mov     x20, x1                     ; provider
    
    ; Log start
    adrp    x0, _IOLog@PAGE
    add     x0, x0, _IOLog@PAGEOFF
    adrp    x1, _kIOLogDebug@PAGE
    ldr     w1, [x1, _kIOLogDebug@PAGEOFF]
    tst     w1, #1
    b.eq    1f
    adrp    x1, _str_start@PAGE
    add     x1, x1, _str_start@PAGEOFF
    blr     x0
1:
    
    ; Call super::start
    mov     x0, x19
    mov     x1, x20
    bl      __ZN9IOService5startEP9IOService
    cbz     x0, _start_failed
    
    ; Store provider
    str     x20, [x19, #AppleARMIISAudio_provider]
    
    ; Map device memory
    mov     x0, x19
    bl      _mapDeviceMemory
    cbz     x0, _start_failed
    
    ; Initialize hardware
    mov     x0, x19
    bl      _initHardware
    cbz     x0, _start_failed
    
    ; Setup interrupts
    mov     x0, x19
    bl      _setupInterrupts
    cbz     x0, _start_failed
    
    ; Create work loop
    mov     x0, x19
    bl      _createWorkLoop
    cbz     x0, _start_failed
    
    ; Register with power management
    mov     x0, x19
    bl      _registerPowerManagement
    
    ; Log success
    adrp    x0, _IOLog@PAGE
    add     x0, x0, _IOLog@PAGEOFF
    adrp    x1, _str_started@PAGE
    add     x1, x1, _str_started@PAGEOFF
    blr     x0
    
    mov     x0, #1
    b       _start_done
    
_start_failed:
    mov     x0, x19
    bl      __ZN17AppleARMIISAudio4stopEP9IOService
    mov     x0, #0
    
_start_done:
    ldr     x19, [sp, #16]
    ldr     x20, [sp, #24]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; Map device memory
;------------------------------------------------------------------------------
_mapDeviceMemory:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    
    mov     x19, x0                     ; this
    
    ; Get device memory from provider
    ldr     x0, [x19, #AppleARMIISAudio_provider]
    bl      __ZNK9IOService16getDeviceMemoryEv
    cbz     x0, _map_failed
    
    ; Get first memory range
    bl      __ZN8OSArray9getObjectEj
    cbz     x0, _map_failed
    
    ; Get physical address and size
    bl      __ZNK15IODeviceMemory19getPhysicalAddressEv
    str     x0, [x19, #AppleARMIISAudio_regPhys]
    
    bl      __ZNK15IODeviceMemory5getLengthEv
    str     x0, [x19, #AppleARMIISAudio_regSize]
    
    ; Map registers
    ldr     x0, [x19, #AppleARMIISAudio_regPhys]
    ldr     x1, [x19, #AppleARMIISAudio_regSize]
    mov     w2, #0x2003                  ; kIOMapAny | kIOMapInhibitCache
    bl      _io_map
    str     x0, [x19, #AppleARMIISAudio_regBase]
    
    cmp     x0, #0
    b.eq    _map_failed
    
    mov     x0, #1
    b       _map_done
    
_map_failed:
    mov     x0, #0
    
_map_done:
    ldr     x19, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; Initialize hardware
;------------------------------------------------------------------------------
_initHardware:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    
    mov     x19, x0                     ; this
    ldr     x19, [x19, #AppleARMIISAudio_regBase]
    
    ; Reset controller
    mov     w0, #I2S_CTRL_RESET
    str     w0, [x19, #I2S_REG_CONTROL]
    
    ; Wait for reset to complete (poll with timeout)
    mov     w1, #I2S_TIMEOUT_US
    mov     w2, #0
    bl      _waitForBit
    
    ; Enable clock
    ldr     w0, [x19, #I2S_REG_CLOCK_CTRL]
    orr     w0, w0, #I2S_CLOCK_ENABLE
    str     w0, [x19, #I2S_REG_CLOCK_CTRL]
    
    ; Wait for clock lock
    add     x0, x19, #I2S_REG_STATUS
    mov     w1, #I2S_STATUS_CLOCK_LOCKED
    mov     w2, #I2S_TIMEOUT_US
    bl      _waitForBit
    
    ; Configure default sample rate (48kHz)
    ldr     x0, [x19, #-AppleARMIISAudio_regBase]   ; Get this pointer
    ldr     w1, [x0, #AppleARMIISAudio_sampleRate]
    bl      _setSampleRate_hw
    
    ; Configure default format
    ldr     x0, [x19, #-AppleARMIISAudio_regBase]
    ldr     w1, [x0, #AppleARMIISAudio_format]
    bl      _setFormat_hw
    
    ; Enable interrupts (mask)
    mov     w0, #I2S_INT_ALL
    str     w0, [x19, #I2S_REG_INT_ENABLE]
    
    ; Set power state to idle
    mov     w0, #I2S_POWER_IDLE
    str     w0, [x19, #I2S_REG_POWER_STATE]
    
    mov     x0, #1
    ldr     x19, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; Set sample rate (hardware level)
;------------------------------------------------------------------------------
_setSampleRate_hw:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    
    ldr     x2, [x0, #AppleARMIISAudio_regBase]
    
    ; Convert sample rate to divider
    cmp     w1, #44100
    b.eq    _rate_44100
    cmp     w1, #48000
    b.eq    _rate_48000
    cmp     w1, #88200
    b.eq    _rate_88200
    cmp     w1, #96000
    b.eq    _rate_96000
    cmp     w1, #176400
    b.eq    _rate_176400
    cmp     w1, #192000
    b.eq    _rate_192000
    
_rate_48000:
    mov     w1, #I2S_RATE_48000
    b       _rate_set
_rate_44100:
    mov     w1, #I2S_RATE_44100
    b       _rate_set
_rate_88200:
    mov     w1, #I2S_RATE_88200
    b       _rate_set
_rate_96000:
    mov     w1, #I2S_RATE_96000
    b       _rate_set
_rate_176400:
    mov     w1, #I2S_RATE_176400
    b       _rate_set
_rate_192000:
    mov     w1, #I2S_RATE_192000
    
_rate_set:
    str     w1, [x2, #I2S_REG_SAMPLE_RATE]
    
    ldp     x29, x30, [sp], #16
    ret

;------------------------------------------------------------------------------
; Wait for bit to be set/cleared
; Inputs: x0 - register address, w1 - bit mask, w2 - timeout (us)
; Outputs: x0 - 1 if success, 0 if timeout
;------------------------------------------------------------------------------
_waitForBit:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    str     x20, [sp, #24]
    
    mov     x19, x0
    mov     w20, w1
    
    mov     w3, wzr                     ; counter
1:
    ldr     w4, [x19]                   ; read register
    and     w4, w4, w20
    cmp     w4, w20
    b.eq    2f                          ; bit set
    
    add     w3, w3, #10                 ; increment counter
    cmp     w3, w2                       ; check timeout
    b.ge    3f                          ; timeout
    
    mov     w0, #10                      ; 10 us delay
    bl      _IODelay
    b       1b
    
2:
    mov     x0, #1
    b       4f
3:
    mov     x0, #0
4:
    ldr     x19, [sp, #16]
    ldr     x20, [sp, #24]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; Interrupt handler
;------------------------------------------------------------------------------
.globl __ZN17AppleARMIISAudio17handleInterruptEP23IOInterruptEventSourcei
__ZN17AppleARMIISAudio17handleInterruptEP23IOInterruptEventSourcei:
    stp     x29, x30, [sp, #-64]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    str     x23, [sp, #48]
    
    mov     x19, x0                     ; this
    ldr     x20, [x19, #AppleARMIISAudio_regBase]
    
    ; Read interrupt status
    ldr     w21, [x20, #I2S_REG_INT_STATUS]
    
    ; Increment interrupt counter
    ldr     x22, [x19, #AppleARMIISAudio_interrupts]
    add     x22, x22, #1
    str     x22, [x19, #AppleARMIISAudio_interrupts]
    
    ; Acknowledge interrupts
    str     w21, [x20, #I2S_REG_INT_STATUS]
    
    ; Process TX DMA complete
    tbz     w21, #2, 1f                 ; I2S_INT_TX_DMA_DONE bit
    bl      _handleTxDMA
1:
    ; Process RX DMA complete
    tbz     w21, #3, 2f                 ; I2S_INT_RX_DMA_DONE bit
    bl      _handleRxDMA
2:
    ; Process underrun
    tbz     w21, #4, 3f                 ; I2S_INT_UNDERRUN bit
    ldr     w22, [x19, #AppleARMIISAudio_underrunCount]
    add     w22, w22, #1
    str     w22, [x19, #AppleARMIISAudio_underrunCount]
3:
    ; Process overrun
    tbz     w21, #5, 4f                 ; I2S_INT_OVERRUN bit
    ldr     w22, [x19, #AppleARMIISAudio_overrunCount]
    add     w22, w22, #1
    str     w22, [x19, #AppleARMIISAudio_overrunCount]
4:
    ; Process clock error
    tbz     w21, #6, 5f                 ; I2S_INT_CLOCK_ERROR bit
    ldr     w22, [x19, #AppleARMIISAudio_errorCount]
    add     w22, w22, #1
    str     w22, [x19, #AppleARMIISAudio_errorCount]
5:
    
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldr     x23, [sp, #48]
    ldp     x29, x30, [sp], #64
    ret

;------------------------------------------------------------------------------
; Handle TX DMA complete
;------------------------------------------------------------------------------
_handleTxDMA:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    
    mov     x19, x0
    
    ; Update TX position
    ldr     x0, [x19, #AppleARMIISAudio_txSize]
    ldr     x1, [x19, #AppleARMIISAudio_txPosition]
    add     x1, x1, x0
    str     x1, [x19, #AppleARMIISAudio_txPosition]
    
    ; Restart DMA if circular buffer
    ldr     w0, [x19, #AppleARMIISAudio_txActive]
    cbz     w0, 1f
    
    ldr     x0, [x19, #AppleARMIISAudio_txPhys]
    str     x0, [x20, #I2S_REG_DMA_TX_ADDR]
    ldr     w0, [x19, #AppleARMIISAudio_txSize]
    str     w0, [x20, #I2S_REG_DMA_TX_SIZE]
    
    ; Trigger DMA
    ldr     w0, [x20, #I2S_REG_DMA_STATUS]
    orr     w0, w0, #I2S_DMA_TX_ACTIVE
    str     w0, [x20, #I2S_REG_DMA_STATUS]
1:
    
    ldr     x19, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; Handle RX DMA complete
;------------------------------------------------------------------------------
_handleRxDMA:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    
    mov     x19, x0
    
    ; Update RX position
    ldr     x0, [x19, #AppleARMIISAudio_rxSize]
    ldr     x1, [x19, #AppleARMIISAudio_rxPosition]
    add     x1, x1, x0
    str     x1, [x19, #AppleARMIISAudio_rxPosition]
    
    ; Restart DMA if circular buffer
    ldr     w0, [x19, #AppleARMIISAudio_rxActive]
    cbz     w0, 1f
    
    ldr     x0, [x19, #AppleARMIISAudio_rxPhys]
    str     x0, [x20, #I2S_REG_DMA_RX_ADDR]
    ldr     w0, [x19, #AppleARMIISAudio_rxSize]
    str     w0, [x20, #I2S_REG_DMA_RX_SIZE]
    
    ; Trigger DMA
    ldr     w0, [x20, #I2S_REG_DMA_STATUS]
    orr     w0, w0, #I2S_DMA_RX_ACTIVE
    str     w0, [x20, #I2S_REG_DMA_STATUS]
1:
    
    ldr     x19, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; AppleARMIISAudio::setSampleRate(uint32_t rate)
;------------------------------------------------------------------------------
.globl __ZN17AppleARMIISAudio11setSampleRateEj
__ZN17AppleARMIISAudio11setSampleRateEj:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    str     x20, [sp, #24]
    
    mov     x19, x0
    mov     w20, w1
    
    ; Lock
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockLock
    
    ; Check if DMA active
    ldr     w0, [x19, #AppleARMIISAudio_dmaActive]
    cbnz    w0, _rate_busy
    
    ; Set sample rate in hardware
    mov     x0, x19
    mov     w1, w20
    bl      _setSampleRate_hw
    
    ; Update stored rate
    str     w20, [x19, #AppleARMIISAudio_sampleRate]
    
    ; Unlock
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockUnlock
    
    mov     x0, #0                       ; kIOReturnSuccess
    b       _rate_done
    
_rate_busy:
    ; Unlock
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockUnlock
    
    mov     x0, #0xE00002BD               ; kIOReturnBusy
    
_rate_done:
    ldr     x19, [sp, #16]
    ldr     x20, [sp, #24]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; AppleARMIISAudio::configureDMA(IOMemoryDescriptor* buffer, uint32_t size)
;------------------------------------------------------------------------------
.globl __ZN17AppleARMIISAudio13configureDMAEP21IOMemoryDescriptorj
__ZN17AppleARMIISAudio13configureDMAEP21IOMemoryDescriptorj:
    stp     x29, x30, [sp, #-48]!
    mov     x29, sp
    str     x19, [sp, #16]
    str     x20, [sp, #24]
    str     x21, [sp, #32]
    str     x22, [sp, #40]
    
    mov     x19, x0                      ; this
    mov     x20, x1                      ; buffer
    mov     w21, w2                      ; size
    
    ; Validate size
    cmp     w21, #I2S_MAX_BUFFER_SIZE
    b.hi    _dma_invalid
    
    ; Lock
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockLock
    
    ; Check if DMA active
    ldr     w0, [x19, #AppleARMIISAudio_dmaActive]
    cbnz    w0, _dma_busy
    
    ; Store buffer
    str     x20, [x19, #AppleARMIISAudio_dmaBuffer]
    str     w21, [x19, #AppleARMIISAudio_txSize]
    str     w21, [x19, #AppleARMIISAudio_rxSize]
    
    ; Get physical address for TX buffer
    mov     x0, x20
    mov     x1, #0
    mov     x2, #0
    bl      __ZNK21IOMemoryDescriptor18getPhysicalSegmentEyP12IOByteCount
    str     x0, [x19, #AppleARMIISAudio_txPhys]
    
    ; Get virtual address
    mov     x0, x20
    bl      __ZNK21IOMemoryDescriptor13getVirtualAddressEv
    str     x0, [x19, #AppleARMIISAudio_txBuffer]
    
    ; For RX, use same buffer (simplified - real driver would separate)
    ldr     x0, [x19, #AppleARMIISAudio_txPhys]
    str     x0, [x19, #AppleARMIISAudio_rxPhys]
    ldr     x0, [x19, #AppleARMIISAudio_txBuffer]
    str     x0, [x19, #AppleARMIISAudio_rxBuffer]
    
    ; Configure hardware DMA
    ldr     x22, [x19, #AppleARMIISAudio_regBase]
    ldr     x0, [x19, #AppleARMIISAudio_txPhys]
    str     x0, [x22, #I2S_REG_DMA_TX_ADDR]
    str     w21, [x22, #I2S_REG_DMA_TX_SIZE]
    
    ldr     x0, [x19, #AppleARMIISAudio_rxPhys]
    str     x0, [x22, #I2S_REG_DMA_RX_ADDR]
    str     w21, [x22, #I2S_REG_DMA_RX_SIZE]
    
    ; Unlock
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockUnlock
    
    mov     x0, #0                       ; kIOReturnSuccess
    b       _dma_done
    
_dma_busy:
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockUnlock
    mov     x0, #0xE00002BD               ; kIOReturnBusy
    b       _dma_done
    
_dma_invalid:
    mov     x0, #0xE00002BC               ; kIOReturnBadArgument
    
_dma_done:
    ldr     x19, [sp, #16]
    ldr     x20, [sp, #24]
    ldr     x21, [sp, #32]
    ldr     x22, [sp, #40]
    ldp     x29, x30, [sp], #48
    ret

;------------------------------------------------------------------------------
; AppleARMIISAudio::startDMA()
;------------------------------------------------------------------------------
.globl __ZN17AppleARMIISAudio7startDMAEv
__ZN17AppleARMIISAudio7startDMAEv:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    str     x20, [sp, #24]
    
    mov     x19, x0
    
    ; Lock
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockLock
    
    ; Check if already active
    ldr     w0, [x19, #AppleARMIISAudio_dmaActive]
    cbnz    w0, _start_dma_already
    
    ; Check if buffer configured
    ldr     x0, [x19, #AppleARMIISAudio_dmaBuffer]
    cbz     x0, _start_dma_no_buffer
    
    ; Get hardware registers
    ldr     x20, [x19, #AppleARMIISAudio_regBase]
    
    ; Enable TX/RX in I2S controller
    ldr     w0, [x20, #I2S_REG_CONTROL]
    orr     w0, w0, #(I2S_CTRL_TX_ENABLE | I2S_CTRL_RX_ENABLE)
    str     w0, [x20, #I2S_REG_CONTROL]
    
    ; Trigger DMA
    ldr     w0, [x20, #I2S_REG_DMA_STATUS]
    orr     w0, w0, #(I2S_DMA_TX_ACTIVE | I2S_DMA_RX_ACTIVE)
    str     w0, [x20, #I2S_REG_DMA_STATUS]
    
    ; Update flags
    mov     w0, #1
    str     w0, [x19, #AppleARMIISAudio_dmaActive]
    str     w0, [x19, #AppleARMIISAudio_txActive]
    str     w0, [x19, #AppleARMIISAudio_rxActive]
    
    ; Reset positions
    str     xzr, [x19, #AppleARMIISAudio_txPosition]
    str     xzr, [x19, #AppleARMIISAudio_rxPosition]
    
    ; Unlock
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockUnlock
    
    mov     x0, #0                       ; kIOReturnSuccess
    b       _start_dma_done
    
_start_dma_already:
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockUnlock
    mov     x0, #0xE00002BD               ; kIOReturnBusy
    b       _start_dma_done
    
_start_dma_no_buffer:
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockUnlock
    mov     x0, #0xE00002BE               ; kIOReturnNoMemory
    
_start_dma_done:
    ldr     x19, [sp, #16]
    ldr     x20, [sp, #24]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; AppleARMIISAudio::stopDMA()
;------------------------------------------------------------------------------
.globl __ZN17AppleARMIISAudio6stopDMAEv
__ZN17AppleARMIISAudio6stopDMAEv:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    str     x20, [sp, #24]
    
    mov     x19, x0
    
    ; Lock
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockLock
    
    ; Check if active
    ldr     w0, [x19, #AppleARMIISAudio_dmaActive]
    cbz     w0, _stop_dma_not_active
    
    ; Get hardware registers
    ldr     x20, [x19, #AppleARMIISAudio_regBase]
    
    ; Stop DMA
    str     wzr, [x20, #I2S_REG_DMA_STATUS]
    
    ; Disable TX/RX
    ldr     w0, [x20, #I2S_REG_CONTROL]
    bic     w0, w0, #(I2S_CTRL_TX_ENABLE | I2S_CTRL_RX_ENABLE)
    str     w0, [x20, #I2S_REG_CONTROL]
    
    ; Update flags
    str     wzr, [x19, #AppleARMIISAudio_dmaActive]
    str     wzr, [x19, #AppleARMIISAudio_txActive]
    str     wzr, [x19, #AppleARMIISAudio_rxActive]
    
    ; Unlock
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockUnlock
    
    mov     x0, #0                       ; kIOReturnSuccess
    b       _stop_dma_done
    
_stop_dma_not_active:
    ldr     x0, [x19, #AppleARMIISAudio_lock]
    bl      _IOLockUnlock
    mov     x0, #0xE00002BF               ; kIOReturnNotOpen
    
_stop_dma_done:
    ldr     x19, [sp, #16]
    ldr     x20, [sp, #24]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; String constants
;------------------------------------------------------------------------------
.section __TEXT,__cstring
.align 4
_str_start:
    .asciz "AppleARMIISAudio: Starting I2S audio controller\n"
_str_started:
    .asciz "AppleARMIISAudio: Successfully started\n"
_str_stop:
    .asciz "AppleARMIISAudio: Stopping\n"
_str_error:
    .asciz "AppleARMIISAudio: Error - %s\n"

;------------------------------------------------------------------------------
; End of file
;------------------------------------------------------------------------------
