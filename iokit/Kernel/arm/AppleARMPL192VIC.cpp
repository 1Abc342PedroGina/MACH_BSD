/*
 * Copyright (c) 2020-2022 Apple Inc. All rights reserved.
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
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#include <IOKit/IOService.h>
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOKitDebug.h>
#include <IOKit/arm/ARMProcessor.h>

#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/OSAtomic.h>

#include <machine/machine_routines.h>
#include <kern/assert.h>
#include <kern/debug.h>
#include <kern/thread_call.h>
#include <sys/kdebug.h>

//==============================================================================
// PL192 Vectored Interrupt Controller Register Definitions
// Based on ARM PrimeCell Vectored Interrupt Controller PL192 Technical Reference Manual
//==============================================================================

// VIC Register Map (base offset)
#define VIC_REG_IRQ_STATUS          0x000   // IRQ Status Register
#define VIC_REG_FIQ_STATUS           0x004   // FIQ Status Register
#define VIC_REG_RAW_INTR             0x008   // Raw Interrupt Status
#define VIC_REG_INT_SELECT           0x00C   // Interrupt Select (IRQ/FIQ)
#define VIC_REG_INT_ENABLE            0x010   // Interrupt Enable
#define VIC_REG_INT_ENABLE_CLEAR      0x014   // Interrupt Enable Clear
#define VIC_REG_SOFT_INT              0x018   // Software Interrupt
#define VIC_REG_SOFT_INT_CLEAR        0x01C   // Software Interrupt Clear
#define VIC_REG_PROTECTION            0x020   // Protection Enable
#define VIC_REG_PREEMPT               0x024   // Preemption Control
#define VIC_REG_VECT_ADDR             0x030   // Vector Address
#define VIC_REG_DEF_VECT_ADDR         0x034   // Default Vector Address
#define VIC_REG_VECT_ADDR0            0x100   // Vector Address 0
#define VIC_REG_VECT_CTRL0            0x200   // Vector Control 0
#define VIC_REG_ITCR                  0x300   // Integration Test Control
#define VIC_REG_ITIP                   0x304   // Integration Test Input
#define VIC_REG_ITOP                   0x308   // Integration Test Output
#define VIC_REG_PERIPH_ID0             0xFE0   // Peripheral ID0
#define VIC_REG_PERIPH_ID1             0xFE4   // Peripheral ID1
#define VIC_REG_PERIPH_ID2             0xFE8   // Peripheral ID2
#define VIC_REG_PERIPH_ID3             0xFEC   // Peripheral ID3
#define VIC_REG_PRIME_CELL_ID0         0xFF0   // PrimeCell ID0
#define VIC_REG_PRIME_CELL_ID1         0xFF4   // PrimeCell ID1
#define VIC_REG_PRIME_CELL_ID2         0xFF8   // PrimeCell ID2
#define VIC_REG_PRIME_CELL_ID3         0xFFC   // PrimeCell ID3

// Vector Control Register bits
#define VIC_VECT_CTRL_ENABLE           (1 << 5)   // Vector enable
#define VIC_VECT_CTRL_SRC_MASK         0x1F       // Interrupt source mask

// Interrupt Select bits
#define VIC_INT_IRQ                    0          // IRQ
#define VIC_INT_FIQ                    1          // FIQ

// Protection bits
#define VIC_PROT_ENABLE                1          // Protection enabled

// Preemption bits
#define VIC_PREEMPT_ENABLE             1          // Preemption enabled
#define VIC_PREEMPT_PRIORITY_MASK      0x1F       // Priority mask

//==============================================================================
// PL192 Characteristics
//==============================================================================

#define PL192_MAX_INTERRUPTS            32         // 32 interrupts per VIC
#define PL192_MAX_VECTORS                16         // 16 vector slots
#define PL192_PRIORITY_LEVELS            16         // 16 priority levels
#define PL192_HARDWARE_REV               0x192      // PL192 revision

//==============================================================================
// Driver Constants
//==============================================================================

#define kAppleVICInterruptController     "AppleARMPL192VIC"
#define kMaxInterruptControllers         8
#define kMaxInterruptsPerController      32
#define kMaxTotalInterrupts              256

// Vector states
enum {
    kVicVectorFree                      = 0,
    kVicVectorAllocated                 = 1,
    kVicVectorActive                    = 2,
    kVicVectorShared                    = 3
};

//==============================================================================
// AppleARMPL192VIC - Vectored Interrupt Controller for Apple Silicon
//==============================================================================

class AppleARMPL192VIC : public IOInterruptController
{
    OSDeclareDefaultStructors(AppleARMPL192VIC)
    
private:
    // Hardware resources
    IOMemoryMap*                    fMemoryMap;
    volatile uint32_t*               fRegisters;
    IOPhysicalAddress               fPhysicalAddress;
    IOPhysicalLength                 fPhysicalLength;
    
    // Interrupt handling
    IOWorkLoop*                      fWorkLoop;
    IOTimerEventSource*              fTimerSource;
    IOInterruptAction                 fInterruptHandler;
    
    // VIC configuration
    uint32_t                         fVICNumber;          // Which VIC in the system
    uint32_t                         fHardwareVersion;    // PL192 revision
    uint32_t                         fNumInterrupts;      // Number of interrupts supported
    uint32_t                         fNumVectors;         // Number of vector slots
    uint32_t                         fBaseInterrupt;      // Base interrupt number in system
    
    // Vector management
    struct VectorEntry {
        uint32_t                     state;               // Vector state
        uint32_t                     source;              // Interrupt source number
        uint32_t                     priority;            // Priority level (0-15)
        IOInterruptHandler            handler;            // Primary handler
        void*                         refCon;             // Handler reference
        IOService*                     nub;                // Service nub
        uint32_t                     interruptCount;      // Statistics
        uint64_t                     lastFire;            // Timestamp of last fire
    } fVectors[PL192_MAX_VECTORS];
    
    // Interrupt source mapping
    struct InterruptSource {
        uint32_t                     vector;              // Mapped vector (-1 if none)
        uint32_t                     flags;               // IRQ/FIQ, edge/level
        uint32_t                     cpuAffinity;         // CPU affinity mask
        IOInterruptHandler            sharedHandler;      // For shared interrupts
        void*                         sharedRefCon;
        uint32_t                     shareCount;          // Number of sharers
    } fSources[PL192_MAX_INTERRUPTS];
    
    // Locking
    IOLock*                          fLock;
    
    // Statistics
    uint64_t                         fTotalInterrupts;
    uint32_t                         fSpuriousInterrupts;
    uint32_t                         fVectorOverflows;
    uint64_t                         fLastErrorTime;
    
    // Private methods
    bool                             mapRegisters(void);
    void                             unmapRegisters(void);
    bool                             initializeHardware(void);
    void                             resetController(void);
    uint32_t                         readHardwareVersion(void);
    
    // Interrupt management
    void                             enableInterruptSource(uint32_t source);
    void                             disableInterruptSource(uint32_t source);
    void                             clearInterrupt(uint32_t source);
    void                             setInterruptPriority(uint32_t source, uint32_t priority);
    void                             setInterruptType(uint32_t source, uint32_t type);
    
    // Vector management
    int                              allocateVectorSlot(void);
    void                             freeVectorSlot(int slot);
    void                             configureVector(int slot, uint32_t source, uint32_t priority);
    void                             programVectorTable(void);
    
    // Interrupt handling
    static void                      primaryInterruptHandler(IOInterruptController* controller);
    void                             handleInterrupt(void);
    uint32_t                         getPendingInterrupt(void);
    void                             dispatchInterrupt(uint32_t vector, uint32_t source);
    
    // Debug and diagnostics
    void                             logVICState(void);
    void                             dumpRegisters(void);
    
protected:
    bool                             init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                             free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                             start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                             stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    // IOInterruptController overrides
    virtual IOReturn                  getInterruptType(int source, int* interruptType) APPLE_KEXT_OVERRIDE;
    virtual IOReturn                  registerInterrupt(int source, OSObject* target,
                                                         IOInterruptAction handler, void* refCon) APPLE_KEXT_OVERRIDE;
    virtual IOReturn                  unregisterInterrupt(int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn                  enableInterrupt(int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn                  disableInterrupt(int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn                  causeInterrupt(int source) APPLE_KEXT_OVERRIDE;
    
    // Power management
    virtual IOReturn                  setPowerState(unsigned long powerState, IOService* device) APPLE_KEXT_OVERRIDE;
    
    // Platform interface
    void                              setVICNumber(uint32_t number) { fVICNumber = number; }
    void                              setBaseInterrupt(uint32_t base) { fBaseInterrupt = base; }
    uint32_t                          getVICNumber(void) { return fVICNumber; }
    uint32_t                          getBaseInterrupt(void) { return fBaseInterrupt; }
};

OSDefineMetaClassAndStructors(AppleARMPL192VIC, IOInterruptController)

//==============================================================================
// Initialization and cleanup
//==============================================================================

bool AppleARMPL192VIC::init(OSDictionary* dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    
    fLock = IOLockAlloc();
    if (!fLock) {
        return false;
    }
    
    // Initialize member variables
    fMemoryMap = nullptr;
    fRegisters = nullptr;
    fWorkLoop = nullptr;
    fTimerSource = nullptr;
    fInterruptHandler = nullptr;
    
    fVICNumber = 0;
    fHardwareVersion = 0;
    fNumInterrupts = PL192_MAX_INTERRUPTS;
    fNumVectors = PL192_MAX_VECTORS;
    fBaseInterrupt = 0;
    
    // Initialize vector table
    for (int i = 0; i < PL192_MAX_VECTORS; i++) {
        fVectors[i].state = kVicVectorFree;
        fVectors[i].source = -1;
        fVectors[i].priority = 0;
        fVectors[i].handler = nullptr;
        fVectors[i].refCon = nullptr;
        fVectors[i].nub = nullptr;
        fVectors[i].interruptCount = 0;
        fVectors[i].lastFire = 0;
    }
    
    // Initialize source mapping
    for (int i = 0; i < PL192_MAX_INTERRUPTS; i++) {
        fSources[i].vector = -1;
        fSources[i].flags = VIC_INT_IRQ;
        fSources[i].cpuAffinity = -1;
        fSources[i].sharedHandler = nullptr;
        fSources[i].sharedRefCon = nullptr;
        fSources[i].shareCount = 0;
    }
    
    fTotalInterrupts = 0;
    fSpuriousInterrupts = 0;
    fVectorOverflows = 0;
    fLastErrorTime = 0;
    
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("AppleARMPL192VIC: Initializing VIC controller\n");
    }
    
    return true;
}

void AppleARMPL192VIC::free(void)
{
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
    
    if (fMemoryMap) {
        fMemoryMap->release();
        fMemoryMap = nullptr;
    }
    
    super::free();
}

//==============================================================================
// Hardware register access
//==============================================================================

bool AppleARMPL192VIC::mapRegisters(void)
{
    IOService* provider = getProvider();
    if (!provider) {
        IOLog("AppleARMPL192VIC: No provider\n");
        return false;
    }
    
    IODeviceMemory* memory = provider->getDeviceMemoryWithIndex(0);
    if (!memory) {
        IOLog("AppleARMPL192VIC: No device memory\n");
        return false;
    }
    
    fPhysicalAddress = memory->getPhysicalAddress();
    fPhysicalLength = memory->getLength();
    
    fMemoryMap = memory->makeMapping(kIOMapAny | kIOMapInhibitCache);
    if (!fMemoryMap) {
        IOLog("AppleARMPL192VIC: Failed to map registers\n");
        return false;
    }
    
    fRegisters = (volatile uint32_t*)fMemoryMap->getVirtualAddress();
    if (!fRegisters) {
        IOLog("AppleARMPL192VIC: Invalid virtual address\n");
        fMemoryMap->release();
        fMemoryMap = nullptr;
        return false;
    }
    
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("AppleARMPL192VIC: Mapped registers at %p (phys: 0x%llx, size: %lld)\n",
              fRegisters, fPhysicalAddress, fPhysicalLength);
    }
    
    return true;
}

void AppleARMPL192VIC::unmapRegisters(void)
{
    if (fMemoryMap) {
        fMemoryMap->release();
        fMemoryMap = nullptr;
        fRegisters = nullptr;
    }
}

//==============================================================================
// Hardware initialization
//==============================================================================

uint32_t AppleARMPL192VIC::readHardwareVersion(void)
{
    uint32_t id0, id1, id2, id3;
    
    // Read Peripheral ID registers (ARM standard)
    id0 = fRegisters[VIC_REG_PERIPH_ID0 / 4];
    id1 = fRegisters[VIC_REG_PERIPH_ID1 / 4];
    id2 = fRegisters[VIC_REG_PERIPH_ID2 / 4];
    id3 = fRegisters[VIC_REG_PERIPH_ID3 / 4];
    
    // Read PrimeCell ID
    uint32_t primecell0 = fRegisters[VIC_REG_PRIME_CELL_ID0 / 4];
    uint32_t primecell1 = fRegisters[VIC_REG_PRIME_CELL_ID1 / 4];
    uint32_t primecell2 = fRegisters[VIC_REG_PRIME_CELL_ID2 / 4];
    uint32_t primecell3 = fRegisters[VIC_REG_PRIME_CELL_ID3 / 4];
    
    // Verify PrimeCell ID (should be 0xB105F00D for ARM peripherals)
    uint32_t primecell = (primecell3 << 24) | (primecell2 << 16) | 
                         (primecell1 << 8) | primecell0;
    
    if (primecell != 0xB105F00D) {
        IOLog("AppleARMPL192VIC: Warning - Invalid PrimeCell ID: 0x%08x\n", primecell);
    }
    
    // Combine peripheral ID to get hardware revision
    uint32_t version = (id3 << 24) | (id2 << 16) | (id1 << 8) | id0;
    
    IOLog("AppleARMPL192VIC: Hardware version: 0x%08x (PrimeCell: 0x%08x)\n", version, primecell);
    
    return version;
}

void AppleARMPL192VIC::resetController(void)
{
    IOLog("AppleARMPL192VIC: Resetting controller\n");
    
    // Disable all interrupts
    fRegisters[VIC_REG_INT_ENABLE_CLEAR / 4] = 0xFFFFFFFF;
    
    // Clear all software interrupts
    fRegisters[VIC_REG_SOFT_INT_CLEAR / 4] = 0xFFFFFFFF;
    
    // Set all interrupts to IRQ mode (not FIQ)
    fRegisters[VIC_REG_INT_SELECT / 4] = 0;
    
    // Clear vector table
    for (int i = 0; i < PL192_MAX_VECTORS; i++) {
        fRegisters[(VIC_REG_VECT_ADDR0 / 4) + i] = 0;
        fRegisters[(VIC_REG_VECT_CTRL0 / 4) + i] = 0;
    }
    
    // Set default vector address
    fRegisters[VIC_REG_DEF_VECT_ADDR / 4] = 0;
    
    // Disable protection (for now)
    fRegisters[VIC_REG_PROTECTION / 4] = 0;
    
    // Disable preemption (for now)
    fRegisters[VIC_REG_PREEMPT / 4] = 0;
    
    IOLog("AppleARMPL192VIC: Reset complete\n");
}

bool AppleARMPL192VIC::initializeHardware(void)
{
    if (!fRegisters) {
        IOLog("AppleARMPL192VIC: No registers mapped\n");
        return false;
    }
    
    // Read hardware version
    fHardwareVersion = readHardwareVersion();
    
    // Reset controller to known state
    resetController();
    
    // Verify we're running on PL192
    if ((fHardwareVersion & 0xFFF00000) != 0x19200000) {
        IOLog("AppleARMPL192VIC: Warning - Not a PL192? Version: 0x%08x\n", fHardwareVersion);
    }
    
    // Determine number of interrupts (may be less than 32)
    // Some implementations have fewer interrupts
    uint32_t config = fRegisters[VIC_REG_PERIPH_ID2 / 4];
    fNumInterrupts = (config & 0xF0) ? 32 : ((config & 0x0F) * 4);
    
    IOLog("AppleARMPL192VIC: Supports %d interrupts, %d vector slots\n", 
          fNumInterrupts, fNumVectors);
    
    return true;
}

//==============================================================================
// Vector management
//==============================================================================

int AppleARMPL192VIC::allocateVectorSlot(void)
{
    for (int i = 0; i < fNumVectors; i++) {
        if (fVectors[i].state == kVicVectorFree) {
            fVectors[i].state = kVicVectorAllocated;
            return i;
        }
    }
    
    OSIncrementAtomic(&fVectorOverflows);
    return -1;
}

void AppleARMPL192VIC::freeVectorSlot(int slot)
{
    if (slot >= 0 && slot < fNumVectors) {
        // Clear hardware vector
        fRegisters[(VIC_REG_VECT_ADDR0 / 4) + slot] = 0;
        fRegisters[(VIC_REG_VECT_CTRL0 / 4) + slot] = 0;
        
        // Clear software state
        fVectors[slot].state = kVicVectorFree;
        fVectors[slot].source = -1;
        fVectors[slot].priority = 0;
        fVectors[slot].handler = nullptr;
        fVectors[slot].refCon = nullptr;
        fVectors[slot].nub = nullptr;
    }
}

void AppleARMPL192VIC::configureVector(int slot, uint32_t source, uint32_t priority)
{
    if (slot < 0 || slot >= fNumVectors || source >= fNumInterrupts) {
        return;
    }
    
    // Update software state
    fVectors[slot].source = source;
    fVectors[slot].priority = priority;
    
    // Configure hardware vector
    uint32_t ctrl = VIC_VECT_CTRL_ENABLE | (source & VIC_VECT_CTRL_SRC_MASK);
    fRegisters[(VIC_REG_VECT_CTRL0 / 4) + slot] = ctrl;
    
    // Update source mapping
    fSources[source].vector = slot;
}

void AppleARMPL192VIC::programVectorTable(void)
{
    // This is called when vectors are reconfigured
    // Hardware already updated via configureVector
}

//==============================================================================
// Interrupt source management
//==============================================================================

void AppleARMPL192VIC::enableInterruptSource(uint32_t source)
{
    if (source >= fNumInterrupts) {
        return;
    }
    
    fRegisters[VIC_REG_INT_ENABLE / 4] = (1 << source);
    
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("AppleARMPL192VIC: Enabled interrupt source %d\n", source);
    }
}

void AppleARMPL192VIC::disableInterruptSource(uint32_t source)
{
    if (source >= fNumInterrupts) {
        return;
    }
    
    fRegisters[VIC_REG_INT_ENABLE_CLEAR / 4] = (1 << source);
    
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("AppleARMPL192VIC: Disabled interrupt source %d\n", source);
    }
}

void AppleARMPL192VIC::clearInterrupt(uint32_t source)
{
    // For PL192, interrupts are cleared by writing to the Vector Address register
    // during interrupt handling. No explicit clear needed for most interrupts.
    
    // For software interrupts:
    if (source < fNumInterrupts) {
        fRegisters[VIC_REG_SOFT_INT_CLEAR / 4] = (1 << source);
    }
}

void AppleARMPL192VIC::setInterruptPriority(uint32_t source, uint32_t priority)
{
    // PL192 doesn't have per-source priority in hardware
    // Priority is managed through vector allocation
    if (source < fNumInterrupts && fSources[source].vector != -1) {
        int slot = fSources[source].vector;
        fVectors[slot].priority = priority & 0x0F;
        // Hardware priority is implicit in vector slot number (0 = highest)
    }
}

void AppleARMPL192VIC::setInterruptType(uint32_t source, uint32_t type)
{
    if (source >= fNumInterrupts) {
        return;
    }
    
    // Set IRQ (0) or FIQ (1) mode
    uint32_t select = fRegisters[VIC_REG_INT_SELECT / 4];
    if (type == VIC_INT_FIQ) {
        select |= (1 << source);
    } else {
        select &= ~(1 << source);
    }
    fRegisters[VIC_REG_INT_SELECT / 4] = select;
    
    fSources[source].flags = type;
}

//==============================================================================
// Interrupt handling
//==============================================================================

uint32_t AppleARMPL192VIC::getPendingInterrupt(void)
{
    // Read vector address - this gives us the highest priority pending interrupt
    uint32_t vectorAddr = fRegisters[VIC_REG_VECT_ADDR / 4];
    
    if (vectorAddr == 0) {
        // No active interrupt
        return -1;
    }
    
    // Vector address points to handler, but we need the vector number
    // In PL192, we can derive the vector number from the address
    // For simplicity, we'll scan the IRQ status register
    return fRegisters[VIC_REG_IRQ_STATUS / 4];
}

void AppleARMPL192VIC::handleInterrupt(void)
{
    uint32_t pending = fRegisters[VIC_REG_IRQ_STATUS / 4];
    uint32_t fiqPending = fRegisters[VIC_REG_FIQ_STATUS / 4];
    
    fTotalInterrupts++;
    
    KERNEL_DEBUG_CONSTANT(0x5A5A1001 | DBG_FUNC_START,
                          fVICNumber, pending, fiqPending, 0, 0);
    
    // Handle FIQ first (higher priority)
    if (fiqPending) {
        for (int i = 0; i < fNumInterrupts; i++) {
            if (fiqPending & (1 << i)) {
                dispatchInterrupt(fSources[i].vector, i);
            }
        }
    }
    
    // Handle IRQ
    if (pending) {
        // Check vectored interrupts first (higher priority)
        uint32_t vectorAddr = fRegisters[VIC_REG_VECT_ADDR / 4];
        
        if (vectorAddr != 0) {
            // Vectored interrupt - find which vector
            for (int i = 0; i < fNumVectors; i++) {
                if (fVectors[i].state == kVicVectorActive && 
                    (pending & (1 << fVectors[i].source))) {
                    dispatchInterrupt(i, fVectors[i].source);
                    pending &= ~(1 << fVectors[i].source);
                }
            }
        }
        
        // Handle remaining interrupts (non-vectored or default)
        if (pending) {
            for (int i = 0; i < fNumInterrupts; i++) {
                if (pending & (1 << i)) {
                    dispatchInterrupt(-1, i);  // Default handler
                }
            }
        }
    }
    
    // Acknowledge interrupt by writing to vector address
    fRegisters[VIC_REG_VECT_ADDR / 4] = 0;
    
    KERNEL_DEBUG_CONSTANT(0x5A5A1001 | DBG_FUNC_END, 0, 0, 0, 0, 0);
}

void AppleARMPL192VIC::dispatchInterrupt(uint32_t vector, uint32_t source)
{
    AbsoluteTime now;
    clock_get_uptime(&now);
    
    // Update statistics
    if (vector != -1 && vector < fNumVectors) {
        fVectors[vector].interruptCount++;
        fVectors[vector].lastFire = now;
    }
    
    // Call handler
    if (vector != -1 && fVectors[vector].handler) {
        // Vectored interrupt
        fVectors[vector].handler(fVectors[vector].refCon, nullptr, source);
    } else if (fSources[source].sharedHandler) {
        // Shared interrupt
        fSources[source].sharedHandler(fSources[source].sharedRefCon, nullptr, source);
    } else {
        // Spurious interrupt
        fSpuriousInterrupts++;
        fLastErrorTime = now;
        
        IOLog("AppleARMPL192VIC: Spurious interrupt on source %d\n", source);
        
        // Disable the spurious interrupt
        disableInterruptSource(source);
    }
    
    // Clear the interrupt
    clearInterrupt(source);
}

void AppleARMPL192VIC::primaryInterruptHandler(IOInterruptController* controller)
{
    AppleARMPL192VIC* vic = OSDynamicCast(AppleARMPL192VIC, controller);
    if (vic) {
        vic->handleInterrupt();
    }
}

//==============================================================================
// IOInterruptController overrides
//==============================================================================

IOReturn AppleARMPL192VIC::getInterruptType(int source, int* interruptType)
{
    if (source < 0 || source >= fNumInterrupts) {
        return kIOReturnBadArgument;
    }
    
    IOLockLock(fLock);
    
    if (fSources[source].flags == VIC_INT_FIQ) {
        *interruptType = IOInterruptTypeEdge | IOInterruptTypeLevel;  // FIQ can be either
    } else {
        *interruptType = IOInterruptTypeEdge;  // IRQ typically edge-triggered on PL192
    }
    
    IOLockUnlock(fLock);
    
    return kIOReturnSuccess;
}

IOReturn AppleARMPL192VIC::registerInterrupt(int source, OSObject* target,
                                             IOInterruptAction handler, void* refCon)
{
    if (source < 0 || source >= fNumInterrupts || !handler) {
        return kIOReturnBadArgument;
    }
    
    IOLockLock(fLock);
    
    // Check if already registered
    if (fSources[source].vector != -1) {
        // Interrupt already has vector
        if (fSources[source].shareCount > 0) {
            // This is a shared interrupt
            fSources[source].shareCount++;
            fSources[source].sharedHandler = handler;
            fSources[source].sharedRefCon = refCon;
            
            IOLockUnlock(fLock);
            return kIOReturnSuccess;
        } else {
            IOLockUnlock(fLock);
            return kIOReturnBusy;
        }
    }
    
    // Allocate vector slot
    int slot = allocateVectorSlot();
    if (slot < 0) {
        IOLockUnlock(fLock);
        return kIOReturnNoResources;
    }
    
    // Configure vector (default priority based on slot)
    configureVector(slot, source, slot & 0x0F);
    
    // Store handler information
    fVectors[slot].handler = handler;
    fVectors[slot].refCon = refCon;
    fVectors[slot].nub = OSDynamicCast(IOService, target);
    fVectors[slot].state = kVicVectorActive;
    
    fSources[source].vector = slot;
    fSources[source].shareCount = 1;
    
    IOLockUnlock(fLock);
    
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("AppleARMPL192VIC: Registered source %d to vector slot %d\n", source, slot);
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleARMPL192VIC::unregisterInterrupt(int source)
{
    if (source < 0 || source >= fNumInterrupts) {
        return kIOReturnBadArgument;
    }
    
    IOLockLock(fLock);
    
    if (fSources[source].shareCount > 1) {
        // Shared interrupt - just decrement count
        fSources[source].shareCount--;
        fSources[source].sharedHandler = nullptr;
        fSources[source].sharedRefCon = nullptr;
        
        IOLockUnlock(fLock);
        return kIOReturnSuccess;
    }
    
    int slot = fSources[source].vector;
    if (slot >= 0 && slot < fNumVectors) {
        // Free the vector slot
        freeVectorSlot(slot);
    }
    
    fSources[source].vector = -1;
    fSources[source].shareCount = 0;
    
    IOLockUnlock(fLock);
    
    if (gIOKitDebug & kIOLogDebug) {
        IOLog("AppleARMPL192VIC: Unregistered source %d\n", source);
    }
    
    return kIOReturnSuccess;
}

IOReturn AppleARMPL192VIC::enableInterrupt(int source)
{
    if (source < 0 || source >= fNumInterrupts) {
        return kIOReturnBadArgument;
    }
    
    enableInterruptSource(source);
    
    return kIOReturnSuccess;
}

IOReturn AppleARMPL192VIC::disableInterrupt(int source)
{
    if (source < 0 || source >= fNumInterrupts) {
        return kIOReturnBadArgument;
    }
    
    disableInterruptSource(source);
    
    return kIOReturnSuccess;
}

IOReturn AppleARMPL192VIC::causeInterrupt(int source)
{
    if (source < 0 || source >= fNumInterrupts) {
        return kIOReturnBadArgument;
    }
    
    // Trigger software interrupt
    fRegisters[VIC_REG_SOFT_INT / 4] = (1 << source);
    
    return kIOReturnSuccess;
}

//==============================================================================
// Power management
//==============================================================================

IOReturn AppleARMPL192VIC::setPowerState(unsigned long powerState, IOService* device)
{
    if (powerState == 0) {
        // Going to sleep
        IOLog("AppleARMPL192VIC: Preparing for sleep\n");
        
        // Save state (simplified)
        // In real implementation, we'd save all registers
        
        // Disable all interrupts
        fRegisters[VIC_REG_INT_ENABLE_CLEAR / 4] = 0xFFFFFFFF;
        
    } else {
        // Waking up
        IOLog("AppleARMPL192VIC: Waking from sleep\n");
        
        // Restore state
        resetController();
        programVectorTable();
        
        // Re-enable interrupts for active sources
        for (int i = 0; i < fNumInterrupts; i++) {
            if (fSources[i].vector != -1) {
                enableInterruptSource(i);
            }
        }
    }
    
    return kIOReturnSuccess;
}

//==============================================================================
// Debug and diagnostics
//==============================================================================

void AppleARMPL192VIC::logVICState(void)
{
    IOLog("\n===== AppleARMPL192VIC %d State =====\n", fVICNumber);
    IOLog("Hardware Version: 0x%08x\n", fHardwareVersion);
    IOLog("Base Interrupt: %d\n", fBaseInterrupt);
    IOLog("Interrupts: %d/%d\n", fNumInterrupts, PL192_MAX_INTERRUPTS);
    IOLog("Vectors: %d/%d\n", fNumVectors, PL192_MAX_VECTORS);
    IOLog("Total Interrupts: %lld\n", fTotalInterrupts);
    IOLog("Spurious: %d\n", fSpuriousInterrupts);
    IOLog("Vector Overflows: %d\n", fVectorOverflows);
    
    IOLog("\n--- Vector Table ---\n");
    for (int i = 0; i < fNumVectors; i++) {
        if (fVectors[i].state != kVicVectorFree) {
            IOLog("Vector %2d: source=%2d prio=%d count=%lld\n",
                  i, fVectors[i].source, fVectors[i].priority,
                  fVectors[i].interruptCount);
        }
    }
    
    IOLog("\n--- Interrupt Sources ---\n");
    for (int i = 0; i < fNumInterrupts; i++) {
        if (fSources[i].vector != -1) {
            IOLog("Source %2d: vector=%d flags=%s shares=%d\n",
                  i, fSources[i].vector,
                  (fSources[i].flags == VIC_INT_FIQ) ? "FIQ" : "IRQ",
                  fSources[i].shareCount);
        }
    }
    
    IOLog("===================================\n");
}

void AppleARMPL192VIC::dumpRegisters(void)
{
    if (!fRegisters) {
        return;
    }
    
    IOLog("\n===== AppleARMPL192VIC Registers =====\n");
    IOLog("IRQ_STATUS: 0x%08x\n", fRegisters[VIC_REG_IRQ_STATUS / 4]);
    IOLog("FIQ_STATUS: 0x%08x\n", fRegisters[VIC_REG_FIQ_STATUS / 4]);
    IOLog("RAW_INTR:   0x%08x\n", fRegisters[VIC_REG_RAW_INTR / 4]);
    IOLog("INT_SELECT: 0x%08x\n", fRegisters[VIC_REG_INT_SELECT / 4]);
    IOLog("INT_ENABLE: 0x%08x\n", fRegisters[VIC_REG_INT_ENABLE / 4]);
    IOLog("SOFT_INT:   0x%08x\n", fRegisters[VIC_REG_SOFT_INT / 4]);
    IOLog("PROTECTION: 0x%08x\n", fRegisters[VIC_REG_PROTECTION / 4]);
    IOLog("PREEMPT:    0x%08x\n", fRegisters[VIC_REG_PREEMPT / 4]);
    IOLog("DEF_VECT:   0x%08x\n", fRegisters[VIC_REG_DEF_VECT_ADDR / 4]);
    
    IOLog("Vector Addresses:\n");
    for (int i = 0; i < 4 && i < fNumVectors; i++) {
        IOLog("  VECT_ADDR%d: 0x%08x\n", i, 
              fRegisters[(VIC_REG_VECT_ADDR0 / 4) + i]);
    }
    
    IOLog("Vector Control:\n");
    for (int i = 0; i < 4 && i < fNumVectors; i++) {
        IOLog("  VECT_CTRL%d: 0x%08x\n", i,
              fRegisters[(VIC_REG_VECT_CTRL0 / 4) + i]);
    }
    
    IOLog("===================================\n");
}

//==============================================================================
// Start - Main entry point
//==============================================================================

bool AppleARMPL192VIC::start(IOService* provider)
{
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("AppleARMPL192VIC: Starting PL192 Vectored Interrupt Controller\n");
    
    // Map hardware registers
    if (!mapRegisters()) {
        IOLog("AppleARMPL192VIC: Failed to map registers\n");
        return false;
    }
    
    // Initialize hardware
    if (!initializeHardware()) {
        IOLog("AppleARMPL192VIC: Hardware initialization failed\n");
        unmapRegisters();
        return false;
    }
    
    // Create work loop
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) {
        IOLog("AppleARMPL192VIC: Failed to create work loop\n");
        unmapRegisters();
        return false;
    }
    
    // Set up interrupt handler
    fInterruptHandler = primaryInterruptHandler;
    
    // Register with platform
    OSSymbol* name = OSSymbol::withCString(kAppleVICInterruptController);
    getPlatform()->registerInterruptController(name, this);
    name->release();
    
    // Publish properties
    setProperty("VIC Number", fVICNumber, 32);
    setProperty("Hardware Version", fHardwareVersion, 32);
    setProperty("Interrupts", fNumInterrupts, 32);
    setProperty("Vectors", fNumVectors, 32);
    setProperty("Base Interrupt", fBaseInterrupt, 32);
    
    // Create array of supported interrupts
    OSArray* intArray = OSArray::withCapacity(fNumInterrupts);
    for (int i = 0; i < fNumInterrupts; i++) {
        intArray->setObject(OSNumber::withNumber(i, 32));
    }
    setProperty("Interrupt Sources", intArray);
    intArray->release();
    
    // Register service
    registerService();
    
    IOLog("AppleARMPL192VIC: Successfully started (VIC %d, base interrupt %d)\n",
          fVICNumber, fBaseInterrupt);
    
    // Dump state if debug enabled
    if (gIOKitDebug & kIOLogDebug) {
        logVICState();
        dumpRegisters();
    }
    
    return true;
}

void AppleARMPL192VIC::stop(IOService* provider)
{
    IOLog("AppleARMPL192VIC: Stopping\n");
    
    // Disable all interrupts
    if (fRegisters) {
        fRegisters[VIC_REG_INT_ENABLE_CLEAR / 4] = 0xFFFFFFFF;
    }
    
    // Unregister from platform
    OSSymbol* name = OSSymbol::withCString(kAppleVICInterruptController);
    getPlatform()->deregisterInterruptController(name);
    name->release();
    
    // Clean up work loop
    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = nullptr;
    }
    
    // Unmap registers
    unmapRegisters();
    
    super::stop(provider);
}
