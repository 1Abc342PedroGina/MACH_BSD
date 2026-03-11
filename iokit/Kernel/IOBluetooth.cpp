/*
 * Copyright (c) 2008-2022 Apple Inc. All rights reserved.
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
 * Please see the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

/*
 * IOBluetooth.cpp
 * Main Bluetooth Driver Entry Point for macOS
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

extern "C" {
#include "IOBluetooth.h"
}

/* Forward declarations of kext entry points */
extern "C" {
    kern_return_t IOBluetooth_start(kmod_info_t* ki, void* data);
    kern_return_t IOBluetooth_stop(kmod_info_t* ki, void* data);
}

/* Kext compatibility info */
__attribute__((used, section("__TEXT,__const")))
static const char* __kext_compatibility_version = "1.0.0";

/* Kext identification */
OSKextLogSpec gIOBluetoothDebugFlags = 0;

/*==============================================================================
 * Kext Start Entry Point
 *==============================================================================*/

kern_return_t
IOBluetooth_start(kmod_info_t* ki, void* data)
{
    kern_return_t ret = KERN_SUCCESS;
    
    printf("IOBluetooth: Loading Bluetooth stack (version 1.0.0)\n");
    
    /* Initialize core Bluetooth */
    if (IOBluetooth_initialize() != 0) {
        printf("IOBluetooth: Failed to initialize core\n");
        return KERN_FAILURE;
    }
    
    /* Register IOKit services */
    if (!IOService::serviceMatching("IOBluetoothCore")) {
        printf("IOBluetooth: Failed to register core service\n");
        IOBluetooth_terminate();
        return KERN_FAILURE;
    }
    
    if (!IOService::serviceMatching("IOBluetoothDSMOS")) {
        printf("IOBluetooth: Failed to register DSMOS integration\n");
        IOBluetooth_terminate();
        return KERN_FAILURE;
    }
    
    printf("IOBluetooth: Bluetooth stack loaded successfully\n");
    
    return ret;
}

/*==============================================================================
 * Kext Stop Entry Point
 *==============================================================================*/

kern_return_t
IOBluetooth_stop(kmod_info_t* ki, void* data)
{
    printf("IOBluetooth: Unloading Bluetooth stack\n");
    
    /* Terminate core Bluetooth */
    IOBluetooth_terminate();
    
    printf("IOBluetooth: Bluetooth stack unloaded\n");
    
    return KERN_SUCCESS;
}

/*==============================================================================
 * Kernel Module Descriptor
 *==============================================================================*/

KMOD_EXPLICIT_DECL(com.apple.iokit.IOBluetooth, "1.0.0", IOBluetooth_start, IOBluetooth_stop)

__attribute__((visibility("default"))) 
OSKextLogSpec
gIOBluetoothDebugFlags = 0;
