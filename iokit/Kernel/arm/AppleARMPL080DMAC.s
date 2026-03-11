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
; EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
; INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
; Please see the License for the specific language governing rights and
; limitations under the License.
;
; @APPLE_OSREFERENCE_LICENSE_HEADER_END@
;
; AppleARMPL080DMAC.s
; ARM PrimeCell DMA Controller PL080 Driver for Apple Silicon
;
; Reference: ARM PrimeCell DMA Controller PL080 Technical Reference Manual
;

;==============================================================================
; PL080 DMA Controller Register Definitions
; Based on ARM PrimeCell DMA Controller PL080 (Revision r1p0)
;==============================================================================

; PL080 Register Map (base offset)
.equ DMAC_REG_INT_STATUS        , 0x000   ; Interrupt Status Register
.equ DMAC_REG_INT_TC_STATUS     , 0x004   ; Interrupt Terminal Count Status
.equ DMAC_REG_INT_TC_CLEAR      , 0x008   ; Interrupt Terminal Count Clear
.equ DMAC_REG_INT_ERROR_STATUS  , 0x00C   ; Interrupt Error Status
.equ DMAC_REG_INT_ERROR_CLEAR   , 0x010   ; Interrupt Error Clear
.equ DMAC_REG_RAW_INT_TC_STATUS , 0x014   ; Raw Interrupt Terminal Count Status
.equ DMAC_REG_RAW_INT_ERROR_STATUS,0x018 ; Raw Interrupt Error Status
.equ DMAC_REG_ENABLED_CHANNELS  , 0x01C   ; Enabled Channels
.equ DMAC_REG_SOFT_BREQ         , 0x020   ; Software Burst Request
.equ DMAC_REG_SOFT_SREQ         , 0x024   ; Software Single Request
.equ DMAC_REG_SOFT_LBREQ        , 0x028   ; Software Last Burst Request
.equ DMAC_REG_SOFT_LSREQ        , 0x02C   ; Software Last Single Request
.equ DMAC_REG_CONFIG             , 0x030   ; Configuration Register
.equ DMAC_REG_SYNC               , 0x034   ; Synchronization Register

; Channel Registers (offset = 0x100 + (channel * 0x20))
.equ DMAC_REG_CHAN_SRC_ADDR      , 0x000   ; Channel Source Address
.equ DMAC_REG_CHAN_DEST_ADDR     , 0x004   ; Channel Destination Address
.equ DMAC_REG_CHAN_LLI           , 0x008   ; Channel Linked List Item
.equ DMAC_REG_CHAN_CONTROL       , 0x00C   ; Channel Control
.equ DMAC_REG_CHAN_CONFIG        , 0x010   ; Channel Configuration

; Configuration Register Bits
.equ DMAC_CONFIG_ENABLE          , 1 << 0
.equ DMAC_CONFIG_M1_BIT          , 1 << 1
.equ DMAC_CONFIG_M2_BIT          , 1 << 2
.equ DMAC_CONFIG_LITTLE_ENDIAN   , 0 << 6
.equ DMAC_CONFIG_BIG_ENDIAN      , 1 << 6
.equ DMAC_CONFIG_AHB_PROT_MASK   , 0x7 << 8

; Channel Control Register Bits
.equ DMAC_CTRL_TRANSFER_SIZE_MASK, 0xFFF   ; Transfer size (bits 0-11)
.equ DMAC_CTRL_SBSIZE_MASK       , 0x7 << 12 ; Source burst size
.equ DMAC_CTRL_DBSIZE_MASK       , 0x7 << 15 ; Destination burst size
.equ DMAC_CTRL_SWIDTH_MASK       , 0x7 << 18 ; Source transfer width
.equ DMAC_CTRL_DWIDTH_MASK       , 0x7 << 21 ; Destination transfer width
.equ DMAC_CTRL_SI                , 1 << 26   ; Source increment
.equ DMAC_CTRL_DI                , 1 << 27   ; Destination increment
.equ DMAC_CTRL_PROT_MASK         , 0x7 << 28 ; Protection
.equ DMAC_CTRL_TCIE              , 1 << 31   ; Terminal Count Interrupt Enable

; Burst sizes
.equ DMAC_BSIZE_1                , 0x0      ; 1 transfer
.equ DMAC_BSIZE_4                , 0x1      ; 4 transfers
.equ DMAC_BSIZE_8                , 0x2      ; 8 transfers
.equ DMAC_BSIZE_16               , 0x3      ; 16 transfers
.equ DMAC_BSIZE_32               , 0x4      ; 32 transfers
.equ DMAC_BSIZE_64               , 0x5      ; 64 transfers
.equ DMAC_BSIZE_128              , 0x6      ; 128 transfers
.equ DMAC_BSIZE_256              , 0x7      ; 256 transfers

; Transfer widths
.equ DMAC_WIDTH_BYTE             , 0x0      ; 8-bit
.equ DMAC_WIDTH_HALFWORD         , 0x1      ; 16-bit
.equ DMAC_WIDTH_WORD             , 0x2      ; 32-bit
.equ DMAC_WIDTH_DWORD            , 0x3      ; 64-bit
.equ DMAC_WIDTH_QWORD            , 0x4      ; 128-bit
.equ DMAC_WIDTH_OWORD            , 0x5      ; 256-bit

; Channel Configuration Register Bits
.equ DMAC_CONFIG_ENABLE          , 1 << 0   ; Channel enable
.equ DMAC_CONFIG_SRC_PERIPH_MASK , 0x1F << 1 ; Source peripheral
.equ DMAC_CONFIG_DEST_PERIPH_MASK, 0x1F << 6 ; Destination peripheral
.equ DMAC_CONFIG_FLOW_CTRL_MASK  , 0x7 << 11 ; Flow control
.equ DMAC_CONFIG_IE              , 1 << 14   ; Interrupt error mask
.equ DMAC_CONFIG_ITC             , 1 << 15   ; Terminal count interrupt mask
.equ DMAC_CONFIG_LOCK            , 1 << 16   ; Lock
.equ DMAC_CONFIG_ACTIVE          , 1 << 17   ; Active
.equ DMAC_CONFIG_HALT            , 1 << 18   ; Halt

; Flow control types
.equ DMAC_FLOW_MEM_TO_MEM        , 0x0      ; Memory to memory
.equ DMAC_FLOW_MEM_TO_PERIPH     , 0x1      ; Memory to peripheral
.equ DMAC_FLOW_PERIPH_TO_MEM     , 0x2      ; Peripheral to memory
.equ DMAC_FLOW_PERIPH_TO_PERIPH  , 0x3      ; Peripheral to peripheral
.equ DMAC_FLOW_PERIPH_TO_MEM_DST  , 0x4     ; Peripheral to memory (dest control)
.equ DMAC_FLOW_MEM_TO_PERIPH_SRC  , 0x5     ; Memory to peripheral (src control)
.equ DMAC_FLOW_PERIPH_TO_PERIPH_SRC,0x6     ; Peripheral to peripheral (src control)

; Linked List Item Structure
.struct 0
LLI_SrcAddr:    .skip 8   ; Source address
LLI_DestAddr:   .skip 8   ; Destination address
LLI_NextLLI:    .skip 8   ; Next LLI address
LLI_Control:    .skip 4   ; Control
LLI_Pad:        .skip 4   ; Padding
LLI_SIZE:

;==============================================================================
; Driver Constants
;==============================================================================

.equ DMAC_MAX_CHANNELS           , 8        ; PL080 supports up to 8 channels
.equ DMAC_MAX_TRANSFER_SIZE      , 4095     ; 12-bit transfer size
.equ DMAC_LLI_ALIGN              , 16       ; LLI must be 16-byte aligned
.equ DMAC_TIMEOUT_US             , 100000   ; 100ms timeout

; Channel states
.equ DMAC_CHAN_FREE              , 0
.equ DMAC_CHAN_ALLOCATED         , 1
.equ DMAC_CHAN_ACTIVE            , 2
.equ DMAC_CHAN_PAUSED            , 3
.equ DMAC_CHAN_ERROR             , 4

; Transfer directions
.equ DMAC_DIR_MEM_TO_MEM         , 0
.equ DMAC_DIR_MEM_TO_DEV         , 1
.equ DMAC_DIR_DEV_TO_MEM         , 2
.equ DMAC_DIR_DEV_TO_DEV         , 3

;==============================================================================
; AppleARMPL080DMAC Structure
;==============================================================================

.struct 0
AppleDMAC_pad0:                 .skip 8
AppleDMAC_vtbl:                 .skip 8      ; Virtual table
AppleDMAC_refCount:             .skip 8      ; retain/release count
AppleDMAC_provider:             .skip 8      ; IOService provider
AppleDMAC_regBase:              .skip 8      ; Register base virtual
AppleDMAC_regPhys:              .skip 8      ; Register physical
AppleDMAC_regSize:              .skip 8      ; Register size
AppleDMAC_workLoop:             .skip 8      ; IOWorkLoop
AppleDMAC_intSource:            .skip 8      ; Interrupt source
AppleDMAC_cmdGate:              .skip 8      ; Command gate
AppleDMAC_lock:                 .skip 8      ; IOLock
AppleDMAC_channels:             .skip 8 * DMAC_MAX_CHANNELS ; Channel descriptors
AppleDMAC_lliPool:               .skip 8      ; LLI pool
AppleDMAC_lliCount:              .skip 4      ; Number of LLIs in pool
AppleDMAC_irqCount:              .skip 8      ; Interrupt counter
AppleDMAC_errorCount:            .skip 4      ; Error counter
AppleDMAC_config:                .skip 4      ; Current configuration
AppleDMAC_version:               .skip 4      ; Hardware version
AppleDMAC_size:

; Channel descriptor structure
.struct 0
DMACChan_state:                 .skip 4      ; Channel state
DMACChan_phyCh:                 .skip 4      ; Physical channel number
DMACChan_srcAddr:               .skip 8      ; Current source address
DMACChan_destAddr:              .skip 8      ; Current destination address
DMACChan_lliHead:               .skip 8      ; First LLI in chain
DMACChan_lliTail:               .skip 8      ; Last LLI in chain
DMACChan_lliCurrent:            .skip 8      ; Current LLI
DMACChan_transferSize:          .skip 4      ; Total transfer size
DMACChan_completed:             .skip 4      ; Completed bytes
DMACChan_control:               .skip 4      ; Control register value
DMACChan_config:                 .skip 4      ; Channel config
DMACChan_userData:               .skip 8      ; User data for callback
DMACChan_callback:               .skip 8      ; Completion callback
DMACChan_size:

;==============================================================================
; External symbols
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
.extern _IODelay
.extern _IOAlloc
.extern _IOFree
.extern _kernel_memory_allocate

;==============================================================================
; Virtual table
;==============================================================================

.data
.align 3
_AppleARMPL080DMACMetaClass:
    .quad __ZTV20AppleARMPL080DMAC + 16
    .quad 0
    .quad 0

_AppleARMPL080DMACVTable:
    .quad __ZN20AppleARMPL080DMAC5startEP9IOService  ; start
    .quad __ZN20AppleARMPL080DMAC4stopEP9IOService   ; stop
    .quad __ZN20AppleARMPL080DMAC4freeEv             ; free
    .quad __ZN20AppleARMPL080DMAC15getWorkLoopEv     ; getWorkLoop
    .quad __ZN20AppleARMPL080DMAC17handleInterruptEP23IOInterruptEventSourcei ; interrupt
    .quad __ZN20AppleARMPL080DMAC8allocateEj         ; allocate channel
    .quad __ZN20AppleARMPL080DMAC6freeChEj           ; free channel
    .quad __ZN20AppleARMPL080DMAC11configureEP20DMACChannelConfig ; configure
    .quad __ZN20AppleARMPL080DMAC9startDMAP20DMACTransfer ; start transfer
    .quad __ZN20AppleARMPL080DMAC8stopDMAEj          ; stop transfer
    .quad __ZN20AppleARMPL080DMAC8pauseDMAEj         ; pause transfer
    .quad __ZN20AppleARMPL080DMAC9resumeDMAEj        ; resume transfer

;==============================================================================
; .text section
;==============================================================================

.text
.align 4

;------------------------------------------------------------------------------
; Constructor
;------------------------------------------------------------------------------
.globl __ZN20AppleARMPL080DMACC1Ev
__ZN20AppleARMPL080DMACC1Ev:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    
    ; Call IOService constructor
    bl      __ZN9IOServiceC2Ev
    
    ; Set vtable
    adrp    x8, _AppleARMPL080DMACVTable@PAGE
    add     x8, x8, _AppleARMPL080DMACVTable@PAGEOFF
    str     x8, [x0, #AppleDMAC_vtbl]
    
    ; Zero initialize all fields
    add     x1, x0, #AppleDMAC_provider
    mov     x2, #(AppleDMAC_size - AppleDMAC_provider)
    bl      _bzero
    
    ; Allocate lock
    stp     x0, x30, [sp, #-16]!
    bl      _IOLockAlloc
    ldp     x1, x30, [sp], #16
    str     x0, [x1, #AppleDMAC_lock]
    
    ; Initialize channel descriptors
    add     x2, x1, #AppleDMAC_channels
    mov     w3, #DMAC_MAX_CHANNELS
    mov     w4, #DMAC_CHAN_FREE
1:
    str     w4, [x2]
    add     x2, x2, #DMACChan_size
    subs    w3, w3, #1
    b.ne    1b
    
    ldp     x29, x30, [sp], #16
    ret

;------------------------------------------------------------------------------
; Start - Main entry point
;------------------------------------------------------------------------------
.globl __ZN20AppleARMPL080DMAC5startEP9IOService
__ZN20AppleARMPL080DMAC5startEP9IOService:
    stp     x29, x30, [sp, #-48]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    str     x21, [sp, #32]
    
    mov     x19, x0                     ; this
    mov     x20, x1                     ; provider
    
    ; Log start
    adrp    x0, _IOLog@PAGE
    add     x0, x0, _IOLog@PAGEOFF
    adrp    x1, _str_dmac_start@PAGE
    add     x1, x1, _str_dmac_start@PAGEOFF
    blr     x0
    
    ; Call super::start
    mov     x0, x19
    mov     x1, x20
    bl      __ZN9IOService5startEP9IOService
    cbz     x0, _start_failed
    
    ; Store provider
    str     x20, [x19, #AppleDMAC_provider]
    
    ; Map device memory
    mov     x0, x19
    bl      _mapDMACMemory
    cbz     x0, _start_failed
    
    ; Reset and initialize hardware
    mov     x0, x19
    bl      _resetDMAC
    cbz     x0, _start_failed
    
    ; Read version/ID
    mov     x0, x19
    bl      _readDMACVersion
    
    ; Setup interrupts
    mov     x0, x19
    bl      _setupDMACInterrupts
    cbz     x0, _start_failed
    
    ; Allocate LLI pool
    mov     x0, x19
    bl      _allocateLLIPool
    cbz     x0, _start_failed
    
    ; Enable DMAC
    mov     x0, x19
    bl      _enableDMAC
    
    ; Log success
    adrp    x0, _IOLog@PAGE
    add     x0, x0, _IOLog@PAGEOFF
    adrp    x1, _str_dmac_started@PAGE
    add     x1, x1, _str_dmac_started@PAGEOFF
    blr     x0
    
    mov     x0, #1
    b       _start_done
    
_start_failed:
    mov     x0, x19
    bl      __ZN20AppleARMPL080DMAC4stopEP9IOService
    mov     x0, #0
    
_start_done:
    ldp     x19, x20, [sp, #16]
    ldr     x21, [sp, #32]
    ldp     x29, x30, [sp], #48
    ret

;------------------------------------------------------------------------------
; Map DMAC memory
;------------------------------------------------------------------------------
_mapDMACMemory:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    
    mov     x19, x0
    
    ; Get device memory from provider
    ldr     x0, [x19, #AppleDMAC_provider]
    bl      __ZNK9IOService16getDeviceMemoryEv
    cbz     x0, _map_failed
    
    ; Get first memory range
    bl      __ZN8OSArray9getObjectEj
    cbz     x0, _map_failed
    
    ; Get physical address and size
    bl      __ZNK15IODeviceMemory19getPhysicalAddressEv
    str     x0, [x19, #AppleDMAC_regPhys]
    
    bl      __ZNK15IODeviceMemory5getLengthEv
    str     x0, [x19, #AppleDMAC_regSize]
    
    ; Map registers (cached, device memory)
    ldr     x0, [x19, #AppleDMAC_regPhys]
    ldr     x1, [x19, #AppleDMAC_regSize]
    mov     w2, #0x2003                  ; kIOMapAny | kIOMapInhibitCache
    bl      _io_map
    str     x0, [x19, #AppleDMAC_regBase]
    
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
; Reset DMAC hardware
;------------------------------------------------------------------------------
_resetDMAC:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    
    mov     x19, x0
    ldr     x19, [x19, #AppleDMAC_regBase]
    
    ; Disable DMAC
    str     wzr, [x19, #DMAC_REG_CONFIG]
    
    ; Clear all interrupts
    str     wzr, [x19, #DMAC_REG_INT_TC_CLEAR]
    str     wzr, [x19, #DMAC_REG_INT_ERROR_CLEAR]
    
    ; Wait for DMAC to stop
    mov     w1, #DMAC_TIMEOUT_US
    bl      _waitDMACIdle
    cbz     x0, _reset_failed
    
    ; Reset all channels
    mov     w2, #DMAC_MAX_CHANNELS
    mov     w3, #0
1:
    ; Disable channel
    add     x4, x19, #0x100
    add     x4, x4, w3, lsl #5
    add     x4, x4, #DMAC_REG_CHAN_CONFIG
    str     wzr, [x4]
    
    add     w3, w3, #1
    cmp     w3, w2
    b.lo    1b
    
    mov     x0, #1
    b       _reset_done
    
_reset_failed:
    mov     x0, #0
    
_reset_done:
    ldr     x19, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; Read DMAC version/peripheral ID
;------------------------------------------------------------------------------
_readDMACVersion:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    
    ldr     x1, [x0, #AppleDMAC_regBase]
    
    ; Read Peripheral ID registers (ARM standard)
    ldr     w2, [x1, #0xFE0]            ; Peripheral ID0
    ldr     w3, [x1, #0xFE4]            ; Peripheral ID1
    ldr     w4, [x1, #0xFE8]            ; Peripheral ID2
    ldr     w5, [x1, #0xFEC]            ; Peripheral ID3
    
    ; Combine into version
    orr     w2, w2, w3, lsl #8
    orr     w2, w2, w4, lsl #16
    orr     w2, w2, w5, lsl #24
    str     w2, [x0, #AppleDMAC_version]
    
    ; Log version
    adrp    x0, _IOLog@PAGE
    add     x0, x0, _IOLog@PAGEOFF
    adrp    x1, _str_dmac_version@PAGE
    add     x1, x1, _str_dmac_version@PAGEOFF
    mov     x2, x2
    blr     x0
    
    ldp     x29, x30, [sp], #16
    ret

;------------------------------------------------------------------------------
; Wait for DMAC to become idle
;------------------------------------------------------------------------------
_waitDMACIdle:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    
    mov     x19, x1                     ; timeout
    
    mov     w3, #0
1:
    ; Check if any channels enabled
    ldr     w2, [x0, #DMAC_REG_ENABLED_CHANNELS]
    cbz     w2, 2f
    
    add     w3, w3, #10
    cmp     w3, w19
    b.ge    3f
    
    mov     w0, #10
    bl      _IODelay
    b       1b
    
2:
    mov     x0, #1
    b       4f
3:
    mov     x0, #0
4:
    ldr     x19, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; Setup interrupts
;------------------------------------------------------------------------------
_setupDMACInterrupts:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    
    mov     x19, x0
    
    ; Create work loop if needed
    bl      __ZN20AppleARMPL080DMAC15getWorkLoopEv
    str     x0, [x19, #AppleDMAC_workLoop]
    cbz     x0, _int_failed
    
    ; Create interrupt event source
    mov     x0, x19
    adrp    x1, _intHandler@PAGE
    add     x1, x1, _intHandler@PAGEOFF
    mov     x2, x19
    ldr     x3, [x19, #AppleDMAC_provider]
    mov     w4, #0
    bl      _IOInterruptEventSource_interruptEventSource
    str     x0, [x19, #AppleDMAC_intSource]
    cbz     x0, _int_failed
    
    ; Add to work loop
    ldr     x0, [x19, #AppleDMAC_workLoop]
    ldr     x1, [x19, #AppleDMAC_intSource]
    bl      _IOWorkLoop_addEventSource
    
    ; Enable interrupts in DMAC
    ldr     x2, [x19, #AppleDMAC_regBase]
    
    ; Enable terminal count and error interrupts for all channels
    mov     w3, #0xFF                    ; All channels
    str     w3, [x2, #DMAC_REG_INT_TC_CLEAR]   ; Clear any pending
    str     w3, [x2, #DMAC_REG_INT_ERROR_CLEAR]
    
    ; Enable DMAC global interrupts
    ldr     w3, [x2, #DMAC_REG_CONFIG]
    orr     w3, w3, #DMAC_CONFIG_ENABLE
    str     w3, [x2, #DMAC_REG_CONFIG]
    
    mov     x0, #1
    b       _int_done
    
_int_failed:
    mov     x0, #0
    
_int_done:
    ldr     x19, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; Interrupt handler
;------------------------------------------------------------------------------
_intHandler:
    stp     x29, x30, [sp, #-64]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    str     x23, [sp, #48]
    
    mov     x19, x0                     ; this
    ldr     x20, [x19, #AppleDMAC_regBase]
    
    ; Increment interrupt counter
    ldr     x21, [x19, #AppleDMAC_irqCount]
    add     x21, x21, #1
    str     x21, [x19, #AppleDMAC_irqCount]
    
    ; Read interrupt status
    ldr     w21, [x20, #DMAC_REG_INT_TC_STATUS]
    ldr     w22, [x20, #DMAC_REG_INT_ERROR_STATUS]
    
    ; Clear interrupts
    str     w21, [x20, #DMAC_REG_INT_TC_CLEAR]
    str     w22, [x20, #DMAC_REG_INT_ERROR_CLEAR]
    
    ; Process terminal count interrupts
    mov     w23, w21
    mov     w2, #0
1:
    tst     w23, #1
    b.eq    2f
    
    ; Channel w2 completed
    mov     x0, x19
    mov     w1, w2
    bl      _handleChannelComplete
    
2:
    lsr     w23, w23, #1
    add     w2, w2, #1
    cmp     w2, #DMAC_MAX_CHANNELS
    b.lo    1b
    
    ; Process error interrupts
    mov     w23, w22
    mov     w2, #0
3:
    tst     w23, #1
    b.eq    4f
    
    ; Channel w2 error
    mov     x0, x19
    mov     w1, w2
    bl      _handleChannelError
    
4:
    lsr     w23, w23, #1
    add     w2, w2, #1
    cmp     w2, #DMAC_MAX_CHANNELS
    b.lo    3b
    
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldr     x23, [sp, #48]
    ldp     x29, x30, [sp], #64
    ret

;------------------------------------------------------------------------------
; Handle channel complete
;------------------------------------------------------------------------------
_handleChannelComplete:
    stp     x29, x30, [sp, #-48]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    str     x21, [sp, #32]
    
    mov     x19, x0
    mov     w20, w1
    
    ; Get channel descriptor
    add     x21, x19, #AppleDMAC_channels
    mov     w2, w20
    mul     x2, x2, #DMACChan_size
    add     x21, x21, x2
    
    ; Update state
    mov     w3, #DMAC_CHAN_FREE
    str     w3, [x21, #DMACChan_state]
    
    ; Call callback if present
    ldr     x4, [x21, #DMACChan_callback]
    cbz     x4, 1f
    
    ldr     x0, [x21, #DMACChan_userData]
    mov     w1, w20
    mov     w2, #0                       ; no error
    blr     x4
    
1:
    ldp     x19, x20, [sp, #16]
    ldr     x21, [sp, #32]
    ldp     x29, x30, [sp], #48
    ret

;------------------------------------------------------------------------------
; Handle channel error
;------------------------------------------------------------------------------
_handleChannelError:
    stp     x29, x30, [sp, #-48]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    str     x21, [sp, #32]
    
    mov     x19, x0
    mov     w20, w1
    
    ; Increment error count
    ldr     w2, [x19, #AppleDMAC_errorCount]
    add     w2, w2, #1
    str     w2, [x19, #AppleDMAC_errorCount]
    
    ; Get channel descriptor
    add     x21, x19, #AppleDMAC_channels
    mov     w2, w20
    mul     x2, x2, #DMACChan_size
    add     x21, x21, x2
    
    ; Update state
    mov     w3, #DMAC_CHAN_ERROR
    str     w3, [x21, #DMACChan_state]
    
    ; Log error
    adrp    x0, _IOLog@PAGE
    add     x0, x0, _IOLog@PAGEOFF
    adrp    x1, _str_dmac_error@PAGE
    add     x1, x1, _str_dmac_error@PAGEOFF
    mov     w2, w20
    blr     x0
    
    ; Call callback with error
    ldr     x4, [x21, #DMACChan_callback]
    cbz     x4, 1f
    
    ldr     x0, [x21, #DMACChan_userData]
    mov     w1, w20
    mov     w2, #1                       ; error
    blr     x4
    
1:
    ldp     x19, x20, [sp, #16]
    ldr     x21, [sp, #32]
    ldp     x29, x30, [sp], #48
    ret

;------------------------------------------------------------------------------
; Allocate DMA channel
;------------------------------------------------------------------------------
.globl __ZN20AppleARMPL080DMAC8allocateEj
__ZN20AppleARMPL080DMAC8allocateEj:
    stp     x29, x30, [sp, #-48]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    str     x21, [sp, #32]
    
    mov     x19, x0
    mov     w20, w1                     ; peripheral ID (if any)
    
    ; Lock
    ldr     x0, [x19, #AppleDMAC_lock]
    bl      _IOLockLock
    
    ; Find free channel
    add     x21, x19, #AppleDMAC_channels
    mov     w2, #DMAC_MAX_CHANNELS
    mov     w3, #0
1:
    ldr     w4, [x21]
    cmp     w4, #DMAC_CHAN_FREE
    b.eq    2f
    
    add     x21, x21, #DMACChan_size
    add     w3, w3, #1
    cmp     w3, w2
    b.lo    1b
    
    ; No free channels
    mov     x0, #-1
    b       _alloc_done
    
2:
    ; Found free channel w3
    mov     w4, #DMAC_CHAN_ALLOCATED
    str     w4, [x21]
    str     w20, [x21, #DMACChan_phyCh]
    
    ; Clear other fields
    str     xzr, [x21, #DMACChan_srcAddr]
    str     xzr, [x21, #DMACChan_destAddr]
    str     xzr, [x21, #DMACChan_lliHead]
    str     xzr, [x21, #DMACChan_lliTail]
    str     xzr, [x21, #DMACChan_lliCurrent]
    str     wzr, [x21, #DMACChan_transferSize]
    str     wzr, [x21, #DMACChan_completed]
    str     xzr, [x21, #DMACChan_userData]
    str     xzr, [x21, #DMACChan_callback]
    
    mov     x0, w3
    
_alloc_done:
    ; Unlock
    ldr     x1, [x19, #AppleDMAC_lock]
    bl      _IOLockUnlock
    
    ldp     x19, x20, [sp, #16]
    ldr     x21, [sp, #32]
    ldp     x29, x30, [sp], #48
    ret

;------------------------------------------------------------------------------
; Configure DMA transfer
;------------------------------------------------------------------------------
.globl __ZN20AppleARMPL080DMAC11configureEP20DMACChannelConfig
__ZN20AppleARMPL080DMAC11configureEP20DMACChannelConfig:
    stp     x29, x30, [sp, #-48]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    str     x21, [sp, #32]
    
    mov     x19, x0
    mov     x20, x1                     ; config structure
    
    ; Get channel number and descriptor
    ldr     w21, [x20]                  ; channel
    add     x2, x19, #AppleDMAC_channels
    mov     w3, w21
    mul     x3, x3, #DMACChan_size
    add     x22, x2, x3
    
    ; Lock
    ldr     x0, [x19, #AppleDMAC_lock]
    bl      _IOLockLock
    
    ; Check channel state
    ldr     w2, [x22]
    cmp     w2, #DMAC_CHAN_ALLOCATED
    b.ne    _config_invalid
    
    ; Build control word
    ldr     w2, [x20, #8]               ; transfer size
    ldr     w3, [x20, #12]              ; src burst size
    ldr     w4, [x20, #16]              ; dest burst size
    ldr     w5, [x20, #20]              ; src width
    ldr     w6, [x20, #24]              ; dest width
    ldr     w7, [x20, #28]              ; flags (inc src, inc dest)
    
    ; Control = size | (burst_src << 12) | (burst_dest << 15) |
    ;          (width_src << 18) | (width_dest << 21) | flags | TCIE
    mov     w8, w2
    and     w8, w8, #DMAC_CTRL_TRANSFER_SIZE_MASK
    
    mov     w9, w3
    and     w9, w9, #0x7
    lsl     w9, w9, #12
    orr     w8, w8, w9
    
    mov     w9, w4
    and     w9, w9, #0x7
    lsl     w9, w9, #15
    orr     w8, w8, w9
    
    mov     w9, w5
    and     w9, w9, #0x7
    lsl     w9, w9, #18
    orr     w8, w8, w9
    
    mov     w9, w6
    and     w9, w9, #0x7
    lsl     w9, w9, #21
    orr     w8, w8, w9
    
    and     w7, w7, #(DMAC_CTRL_SI | DMAC_CTRL_DI)
    orr     w8, w8, w7
    
    orr     w8, w8, #DMAC_CTRL_TCIE      ; enable TC interrupt
    
    str     w8, [x22, #DMACChan_control]
    
    ; Build config word
    ldr     w2, [x20, #32]               ; flow control
    ldr     w3, [x20, #36]               ; src peripheral
    ldr     w4, [x20, #40]               ; dest peripheral
    
    ; Config = enable | (src_periph << 1) | (dest_periph << 6) |
    ;          (flow << 11) | IE | ITC
    mov     w8, #DMAC_CONFIG_ENABLE
    
    mov     w9, w3
    and     w9, w9, #0x1F
    lsl     w9, w9, #1
    orr     w8, w8, w9
    
    mov     w9, w4
    and     w9, w9, #0x1F
    lsl     w9, w9, #6
    orr     w8, w8, w9
    
    mov     w9, w2
    and     w9, w9, #0x7
    lsl     w9, w9, #11
    orr     w8, w8, w9
    
    orr     w8, w8, #(DMAC_CONFIG_IE | DMAC_CONFIG_ITC)
    
    str     w8, [x22, #DMACChan_config]
    
    ; Unlock
    ldr     x0, [x19, #AppleDMAC_lock]
    bl      _IOLockUnlock
    
    mov     x0, #0                       ; kIOReturnSuccess
    b       _config_done
    
_config_invalid:
    ldr     x0, [x19, #AppleDMAC_lock]
    bl      _IOLockUnlock
    mov     x0, #0xE00002BD               ; kIOReturnBusy
    
_config_done:
    ldp     x19, x20, [sp, #16]
    ldr     x21, [sp, #32]
    ldp     x29, x30, [sp], #48
    ret

;------------------------------------------------------------------------------
; Start DMA transfer
;------------------------------------------------------------------------------
.globl __ZN20AppleARMPL080DMAC9startDMAP20DMACTransfer
__ZN20AppleARMPL080DMAC9startDMAP20DMACTransfer:
    stp     x29, x30, [sp, #-64]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    str     x23, [sp, #48]
    
    mov     x19, x0
    mov     x20, x1                     ; transfer descriptor
    
    ; Get channel number
    ldr     w21, [x20]                  ; channel
    
    ; Get channel descriptor
    add     x2, x19, #AppleDMAC_channels
    mov     w3, w21
    mul     x3, x3, #DMACChan_size
    add     x22, x2, x3
    
    ; Lock
    ldr     x0, [x19, #AppleDMAC_lock]
    bl      _IOLockLock
    
    ; Check channel state
    ldr     w2, [x22]
    cmp     w2, #DMAC_CHAN_ALLOCATED
    b.ne    _start_busy
    
    ; Get source/destination addresses
    ldr     x2, [x20, #8]               ; src address
    ldr     x3, [x20, #16]              ; dest address
    str     x2, [x22, #DMACChan_srcAddr]
    str     x3, [x22, #DMACChan_destAddr]
    
    ; Store callback and user data
    ldr     x4, [x20, #24]              ; callback
    ldr     x5, [x20, #32]              ; user data
    str     x4, [x22, #DMACChan_callback]
    str     x5, [x22, #DMACChan_userData]
    
    ; Get control and config
    ldr     w6, [x22, #DMACChan_control]
    ldr     w7, [x22, #DMACChan_config]
    
    ; Get hardware registers
    ldr     x23, [x19, #AppleDMAC_regBase]
    
    ; Calculate channel register base
    add     x23, x23, #0x100
    add     x23, x23, w21, lsl #5
    
    ; Program channel registers
    str     x2, [x23, #DMAC_REG_CHAN_SRC_ADDR]
    str     x3, [x23, #DMAC_REG_CHAN_DEST_ADDR]
    str     wzr, [x23, #DMAC_REG_CHAN_LLI]      ; No LLI for now
    str     w6, [x23, #DMAC_REG_CHAN_CONTROL]
    
    ; Enable channel (last - triggers transfer)
    str     w7, [x23, #DMAC_REG_CHAN_CONFIG]
    
    ; Update state
    mov     w2, #DMAC_CHAN_ACTIVE
    str     w2, [x22]
    
    ; Unlock
    ldr     x0, [x19, #AppleDMAC_lock]
    bl      _IOLockUnlock
    
    mov     x0, #0
    b       _start_done
    
_start_busy:
    ldr     x0, [x19, #AppleDMAC_lock]
    bl      _IOLockUnlock
    mov     x0, #0xE00002BD
    
_start_done:
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldr     x23, [sp, #48]
    ldp     x29, x30, [sp], #64
    ret

;------------------------------------------------------------------------------
; Stop DMA transfer
;------------------------------------------------------------------------------
.globl __ZN20AppleARMPL080DMAC8stopDMAEj
__ZN20AppleARMPL080DMAC8stopDMAEj:
    stp     x29, x30, [sp, #-48]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    str     x21, [sp, #32]
    
    mov     x19, x0
    mov     w20, w1                     ; channel
    
    ; Get channel descriptor
    add     x2, x19, #AppleDMAC_channels
    mov     w3, w20
    mul     x3, x3, #DMACChan_size
    add     x21, x2, x3
    
    ; Lock
    ldr     x0, [x19, #AppleDMAC_lock]
    bl      _IOLockLock
    
    ; Disable channel in hardware
    ldr     x2, [x19, #AppleDMAC_regBase]
    add     x2, x2, #0x100
    add     x2, x2, w20, lsl #5
    add     x2, x2, #DMAC_REG_CHAN_CONFIG
    
    str     wzr, [x2]                   ; Disable channel
    
    ; Update state
    mov     w3, #DMAC_CHAN_FREE
    str     w3, [x21]
    
    ; Unlock
    ldr     x0, [x19, #AppleDMAC_lock]
    bl      _IOLockUnlock
    
    mov     x0, #0
    ldp     x19, x20, [sp, #16]
    ldr     x21, [sp, #32]
    ldp     x29, x30, [sp], #48
    ret

;------------------------------------------------------------------------------
; Allocate LLI pool
;------------------------------------------------------------------------------
_allocateLLIPool:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    
    mov     x19, x0
    
    ; Allocate memory for LLIs (256 entries)
    mov     x0, #LLI_SIZE * 256
    mov     x1, #DMAC_LLI_ALIGN
    mov     x2, #0x1000000               ; kIOMemoryKernel | kIOMemoryPhysContiguous
    bl      _kernel_memory_allocate
    str     x0, [x19, #AppleDMAC_lliPool]
    
    cmp     x0, #0
    b.eq    _lli_failed
    
    mov     w1, #256
    str     w1, [x19, #AppleDMAC_lliCount]
    
    ; Initialize LLI pool (clear)
    mov     x1, #LLI_SIZE * 256
    bl      _bzero
    
    mov     x0, #1
    b       _lli_done
    
_lli_failed:
    mov     x0, #0
    
_lli_done:
    ldr     x19, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

;------------------------------------------------------------------------------
; Enable DMAC
;------------------------------------------------------------------------------
_enableDMAC:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    
    ldr     x1, [x0, #AppleDMAC_regBase]
    
    ; Enable DMAC
    ldr     w2, [x1, #DMAC_REG_CONFIG]
    orr     w2, w2, #DMAC_CONFIG_ENABLE
    str     w2, [x1, #DMAC_REG_CONFIG]
    
    ldp     x29, x30, [sp], #16
    ret

;------------------------------------------------------------------------------
; String constants
;------------------------------------------------------------------------------
.section __TEXT,__cstring
.align 4
_str_dmac_start:
    .asciz "AppleARMPL080DMAC: Starting PL080 DMA Controller\n"
_str_dmac_started:
    .asciz "AppleARMPL080DMAC: Successfully started (version: 0x%08x)\n"
_str_dmac_version:
    .asciz "AppleARMPL080DMAC: Hardware version: 0x%08x\n"
_str_dmac_error:
    .asciz "AppleARMPL080DMAC: Channel %d error\n"

;------------------------------------------------------------------------------
; End of file
;------------------------------------------------------------------------------
