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

/*
 * AppleSMBIOS.cpp
 * SMBIOS (System Management BIOS) Interface Driver for macOS
 *
 * This driver reads and interprets SMBIOS tables from firmware
 * to provide hardware identification and configuration information
 * to the macOS operating system.
 *
 * SMBIOS Specification Version: 3.5.0 (DMTF)
 * Supported on: x86_64 platforms only
 */

#ifdef __x86_64_
_
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pwr_mgt/IOPM.h>

#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSDebug.h>
#include <libkern/libkern.h>

#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <sys/reboot.h>

#include <kern/clock.h>
#include <kern/locks.h>

#include <machine/machine_routines.h>
#include <machine/cpu_capabilities.h>

#include <uuid/uuid.h>

/*==============================================================================
 * SMBIOS Specification Constants and Structures
 * Based on DMTF SMBIOS Reference Specification v3.5.0
 *==============================================================================*/

/* SMBIOS Entry Point Structure (32-bit) */
#define SMBIOS_ANCHOR_32              "_SM_"
#define SMBIOS_ANCHOR_32_LEN          4
#define SMBIOS_INTERMEDIATE_ANCHOR     "_DMI_"
#define SMBIOS_INTERMEDIATE_LEN        5

struct smbios_entry_32 {
    char        anchor[4];              /* "_SM_" */
    uint8_t     checksum;               /* Entry point checksum */
    uint8_t     length;                  /* Entry point length (0x1F) */
    uint8_t     major_version;           /* Major version */
    uint8_t     minor_version;           /* Minor version */
    uint16_t    max_structure_size;      /* Maximum structure size */
    uint8_t     entry_revision;          /* Entry point revision */
    uint8_t     formatted_area[5];       /* Formatted area */
    char        intermediate_anchor[5];  /* "_DMI_" */
    uint8_t     intermediate_checksum;   /* Intermediate checksum */
    uint16_t    structure_table_length;  /* Total length of structure table */
    uint32_t    structure_table_address; /* Physical address of structure table */
    uint16_t    number_of_structures;    /* Number of structures */
    uint8_t     smbios_bcd_revision;     /* BCD revision */
} __attribute__((packed));

/* SMBIOS Entry Point Structure (64-bit) */
#define SMBIOS_ANCHOR_64              "_SM3_"
#define SMBIOS_ANCHOR_64_LEN          5

struct smbios_entry_64 {
    char        anchor[5];              /* "_SM3_" */
    uint8_t     checksum;               /* Entry point checksum */
    uint8_t     length;                  /* Entry point length */
    uint8_t     major_version;           /* Major version */
    uint8_t     minor_version;           /* Minor version */
    uint8_t     docrev;                   /* Documentation revision */
    uint8_t     entry_revision;          /* Entry point revision */
    uint8_t     reserved;                /* Reserved */
    uint32_t    structure_table_max_size; /* Maximum size of structure table */
    uint64_t    structure_table_address; /* Physical address of structure table */
} __attribute__((packed));

/* SMBIOS Structure Header */
struct smbios_header {
    uint8_t     type;                    /* Structure type */
    uint8_t     length;                  /* Structure length */
    uint16_t    handle;                  /* Structure handle */
} __attribute__((packed));

/*==============================================================================
 * SMBIOS Structure Types (Common)
 *==============================================================================*/

#define SMBIOS_TYPE_BIOS                0       /* BIOS Information */
#define SMBIOS_TYPE_SYSTEM              1       /* System Information */
#define SMBIOS_TYPE_BASEBOARD           2       /* Baseboard Information */
#define SMBIOS_TYPE_CHASSIS             3       /* Chassis Information */
#define SMBIOS_TYPE_PROCESSOR           4       /* Processor Information */
#define SMBIOS_TYPE_MEMORY_CONTROLLER   5       /* Memory Controller (obsolete) */
#define SMBIOS_TYPE_MEMORY_MODULE       6       /* Memory Module (obsolete) */
#define SMBIOS_TYPE_CACHE               7       /* Cache Information */
#define SMBIOS_TYPE_PORT_CONNECTOR      8       /* Port Connector Information */
#define SMBIOS_TYPE_SYSTEM_SLOTS        9       /* System Slots */
#define SMBIOS_TYPE_ONBOARD_DEVICES     10      /* On Board Devices */
#define SMBIOS_TYPE_OEM_STRINGS          11      /* OEM Strings */
#define SMBIOS_TYPE_SYSTEM_CONFIG        12      /* System Configuration Options */
#define SMBIOS_TYPE_BIOS_LANGUAGE        13      /* BIOS Language Information */
#define SMBIOS_TYPE_GROUP_ASSOC          14      /* Group Associations */
#define SMBIOS_TYPE_SYSTEM_EVENT_LOG     15      /* System Event Log */
#define SMBIOS_TYPE_PHYSICAL_MEMORY      16      /* Physical Memory Array */
#define SMBIOS_TYPE_MEMORY_DEVICE        17      /* Memory Device */
#define SMBIOS_TYPE_32BIT_MEMORY_ERROR   18      /* 32-bit Memory Error Information */
#define SMBIOS_TYPE_MEMORY_ARRAY_MAPPED  19      /* Memory Array Mapped Address */
#define SMBIOS_TYPE_MEMORY_DEVICE_MAPPED 20      /* Memory Device Mapped Address */
#define SMBIOS_TYPE_BUILTIN_POINTING     21      /* Built-in Pointing Device */
#define SMBIOS_TYPE_PORTABLE_BATTERY     22      /* Portable Battery */
#define SMBIOS_TYPE_SYSTEM_RESET         23      /* System Reset */
#define SMBIOS_TYPE_HARDWARE_SECURITY    24      /* Hardware Security */
#define SMBIOS_TYPE_SYSTEM_POWER_CONTROL 25      /* System Power Controls */
#define SMBIOS_TYPE_VOLTAGE_PROBE        26      /* Voltage Probe */
#define SMBIOS_TYPE_COOLING_DEVICE       27      /* Cooling Device */
#define SMBIOS_TYPE_TEMPERATURE_PROBE    28      /* Temperature Probe */
#define SMBIOS_TYPE_ELECTRICAL_CURRENT   29      /* Electrical Current Probe */
#define SMBIOS_TYPE_OOB_REMOTE_ACCESS    30      /* Out-of-band Remote Access */
#define SMBIOS_TYPE_BOOT_INTEGRITY       31      /* Boot Integrity Services */
#define SMBIOS_TYPE_SYSTEM_BOOT          32      /* System Boot Information */
#define SMBIOS_TYPE_64BIT_MEMORY_ERROR   33      /* 64-bit Memory Error Information */
#define SMBIOS_TYPE_MANAGEMENT_DEVICE    34      /* Management Device */
#define SMBIOS_TYPE_MANAGEMENT_DEVICE_COMPONENT 35 /* Management Device Component */
#define SMBIOS_TYPE_MANAGEMENT_DEVICE_THRESHOLD 36 /* Management Device Threshold Data */
#define SMBIOS_TYPE_MEMORY_CHANNEL       37      /* Memory Channel */
#define SMBIOS_TYPE_IPMI_DEVICE          38      /* IPMI Device Information */
#define SMBIOS_TYPE_POWER_SUPPLY         39      /* Power Supply */
#define SMBIOS_TYPE_ADDITIONAL_INFO      40      /* Additional Information */
#define SMBIOS_TYPE_ONBOARD_DEVICES_EXT  41      /* Onboard Devices Extended */
#define SMBIOS_TYPE_MGMT_CONTROLLER_HOST 42      /* Management Controller Host Interface */
#define SMBIOS_TYPE_TPM_DEVICE           43      /* TPM Device */
#define SMBIOS_TYPE_PROCESSOR_ADDITIONAL 44      /* Processor Additional Information */
#define SMBIOS_TYPE_FIRMWARE_INVENTORY   45      /* Firmware Inventory Information */
#define SMBIOS_TYPE_STRING_PROPERTY      46      /* String Property */
#define SMBIOS_TYPE_INACTIVE             126     /* Inactive */
#define SMBIOS_TYPE_END_OF_TABLE         127     /* End-of-Table */

/*==============================================================================
 * SMBIOS Structure Type 0 - BIOS Information
 *==============================================================================*/

struct smbios_type_0 {
    struct smbios_header header;
    uint8_t     vendor;                  /* String number */
    uint8_t     version;                 /* String number */
    uint16_t    starting_address_segment; /* 0xF000 */
    uint8_t     release_date;            /* String number */
    uint8_t     rom_size;                /* ROM size in 64K blocks */
    uint64_t    characteristics;         /* BIOS characteristics */
    uint8_t     characteristics_ext1;    /* Extended characteristics */
    uint8_t     characteristics_ext2;    /* Extended characteristics */
    uint8_t     system_bios_major;       /* System BIOS major release */
    uint8_t     system_bios_minor;       /* System BIOS minor release */
    uint8_t     embedded_fw_major;        /* Embedded firmware major release */
    uint8_t     embedded_fw_minor;        /* Embedded firmware minor release */
    uint16_t    extended_rom_size;        /* Extended ROM size */
} __attribute__((packed));

/*==============================================================================
 * SMBIOS Structure Type 1 - System Information
 *==============================================================================*/

struct smbios_type_1 {
    struct smbios_header header;
    uint8_t     manufacturer;            /* String number */
    uint8_t     product_name;            /* String number */
    uint8_t     version;                 /* String number */
    uint8_t     serial_number;           /* String number */
    uint8_t     uuid[16];                /* UUID */
    uint8_t     wakeup_type;             /* Wake-up type */
    uint8_t     sku_number;              /* String number (SKU) */
    uint8_t     family;                  /* String number (family) */
} __attribute__((packed));

/*==============================================================================
 * SMBIOS Structure Type 2 - Baseboard Information
 *==============================================================================*/

struct smbios_type_2 {
    struct smbios_header header;
    uint8_t     manufacturer;            /* String number */
    uint8_t     product;                 /* String number */
    uint8_t     version;                 /* String number */
    uint8_t     serial_number;           /* String number */
    uint8_t     asset_tag;               /* String number */
    uint8_t     feature_flags;           /* Feature flags */
    uint8_t     location;                /* String number */
    uint16_t    chassis_handle;          /* Chassis handle */
    uint8_t     board_type;              /* Board type */
    uint8_t     num_children;            /* Number of contained objects */
    uint16_t    children[0];             /* Contained object handles */
} __attribute__((packed));

/*==============================================================================
 * SMBIOS Structure Type 4 - Processor Information
 *==============================================================================*/

struct smbios_type_4 {
    struct smbios_header header;
    uint8_t     socket_designation;      /* String number */
    uint8_t     processor_type;          /* Processor type */
    uint8_t     processor_family;        /* Processor family */
    uint8_t     manufacturer;            /* String number */
    uint64_t    processor_id;            /* Processor ID */
    uint8_t     processor_version;       /* String number */
    uint8_t     voltage;                  /* Voltage */
    uint16_t    external_clock;          /* External clock in MHz */
    uint16_t    max_speed;                /* Maximum speed in MHz */
    uint16_t    current_speed;            /* Current speed in MHz */
    uint8_t     status;                   /* Status */
    uint8_t     processor_upgrade;        /* Processor upgrade */
    uint16_t    l1_cache_handle;          /* L1 cache handle */
    uint16_t    l2_cache_handle;          /* L2 cache handle */
    uint16_t    l3_cache_handle;          /* L3 cache handle */
    uint8_t     serial_number;            /* String number */
    uint8_t     asset_tag;                 /* String number */
    uint8_t     part_number;               /* String number */
    uint8_t     core_count;                /* Core count */
    uint8_t     core_enabled;              /* Core enabled */
    uint8_t     thread_count;              /* Thread count */
    uint16_t    processor_characteristics; /* Processor characteristics */
    uint16_t    processor_family2;         /* Processor family 2 */
    uint16_t    core_count2;               /* Core count 2 */
    uint16_t    core_enabled2;             /* Core enabled 2 */
    uint16_t    thread_count2;             /* Thread count 2 */
} __attribute__((packed));

/*==============================================================================
 * SMBIOS Structure Type 16 - Physical Memory Array
 *==============================================================================*/

struct smbios_type_16 {
    struct smbios_header header;
    uint8_t     location;                  /* Array location */
    uint8_t     use;                       /* Array use */
    uint8_t     error_correction;          /* Error correction type */
    uint32_t    max_capacity;              /* Maximum capacity in KB (or use extended) */
    uint16_t    error_handle;              /* Error information handle */
    uint16_t    num_devices;                /* Number of devices */
    uint64_t    extended_max_capacity;      /* Extended maximum capacity */
} __attribute__((packed));

/*==============================================================================
 * SMBIOS Structure Type 17 - Memory Device
 *==============================================================================*/

struct smbios_type_17 {
    struct smbios_header header;
    uint16_t    array_handle;              /* Physical memory array handle */
    uint16_t    error_handle;              /* Memory error handle */
    uint16_t    total_width;                /* Total width in bits */
    uint16_t    data_width;                 /* Data width in bits */
    uint16_t    size;                       /* Size in KB (0x7FFF = extended) */
    uint8_t     form_factor;                /* Form factor */
    uint8_t     device_set;                  /* Device set */
    uint8_t     device_locator;              /* String number */
    uint8_t     bank_locator;                /* String number */
    uint8_t     memory_type;                 /* Memory type */
    uint16_t    type_detail;                 /* Type detail */
    uint16_t    speed;                       /* Speed in MHz */
    uint8_t     manufacturer;                /* String number */
    uint8_t     serial_number;                /* String number */
    uint8_t     asset_tag;                    /* String number */
    uint8_t     part_number;                  /* String number */
    uint8_t     attributes;                   /* Attributes */
    uint32_t    extended_size;                /* Extended size in MB */
    uint16_t    configured_clock_speed;       /* Configured clock speed */
    uint16_t    min_voltage;                  /* Minimum voltage in mV */
    uint16_t    max_voltage;                  /* Maximum voltage in mV */
    uint16_t    configured_voltage;            /* Configured voltage in mV */
    uint8_t     memory_technology;             /* Memory technology */
    uint16_t    memory_operating_mode_capability; /* Operating mode capability */
    uint8_t     firmware_version;              /* String number */
    uint16_t    module_manufacturer_id;        /* Module manufacturer ID */
    uint16_t    module_product_id;             /* Module product ID */
    uint16_t    memory_subsystem_controller_manufacturer_id; /* Subsystem controller */
    uint16_t    memory_subsystem_controller_product_id; /* Subsystem product */
    uint64_t    non_volatile_size;              /* Non-volatile size in bytes */
    uint64_t    volatile_size;                  /* Volatile size in bytes */
    uint64_t    cache_size;                     /* Cache size in bytes */
    uint64_t    logical_size;                    /* Logical size in bytes */
    uint32_t    extended_speed;                  /* Extended speed in MHz */
    uint32_t    extended_configured_clock_speed; /* Extended configured speed */
} __attribute__((packed));

/*==============================================================================
 * SMBIOS Structure Type 19 - Memory Array Mapped Address
 *==============================================================================*/

struct smbios_type_19 {
    struct smbios_header header;
    uint32_t    starting_address;           /* Starting address in KB */
    uint32_t    ending_address;             /* Ending address in KB */
    uint16_t    array_handle;               /* Physical memory array handle */
    uint8_t     partition_width;             /* Partition width */
    uint64_t    extended_starting_address;   /* Extended starting address */
    uint64_t    extended_ending_address;     /* Extended ending address */
} __attribute__((packed));

/*==============================================================================
 * SMBIOS Structure Type 20 - Memory Device Mapped Address
 *==============================================================================*/

struct smbios_type_20 {
    struct smbios_header header;
    uint32_t    starting_address;           /* Starting address in KB */
    uint32_t    ending_address;             /* Ending address in KB */
    uint16_t    device_handle;              /* Memory device handle */
    uint16_t    array_handle;                /* Memory array handle */
    uint8_t     partition_width;             /* Partition width */
    uint64_t    extended_starting_address;   /* Extended starting address */
    uint64_t    extended_ending_address;     /* Extended ending address */
} __attribute__((packed));

/*==============================================================================
 * Apple-Specific SMBIOS Extensions
 *==============================================================================*/

/* Apple custom SMBIOS types (in OEM range 128-255) */
#define SMBIOS_APPLE_TYPE_FIRMWARE      128     /* Apple Firmware Information */
#define SMBIOS_APPLE_TYPE_BOOTER        129     /* Apple Booter Information */
#define SMBIOS_APPLE_TYPE_PLATFORM      130     /* Apple Platform Information */
#define SMBIOS_APPLE_TYPE_SECURITY      131     /* Apple Security Information */

struct smbios_apple_platform {
    struct smbios_header header;
    uint8_t     board_id[16];              /* Board identifier */
    uint8_t     chip_id[16];                /* Chip identifier */
    uint8_t     security_flags;             /* Security flags */
    uint32_t    boot_rom_version;           /* Boot ROM version */
    uint32_t    firmware_features;          /* Firmware features */
    uint8_t     platform_uuid[16];          /* Platform UUID */
    uint8_t     mlb_serial[32];              /* MLB serial number */
    uint8_t     rom_serial[12];              /* ROM serial */
} __attribute__((packed));

/*==============================================================================
 * AppleSMBIOS Driver Class
 *==============================================================================*/

class AppleSMBIOS : public IOService
{
    OSDeclareDefaultStructors(AppleSMBIOS)
    
private:
    /* SMBIOS table data */
    uint8_t*                    fSMBIOSTable;
    uint32_t                    fSMBIOSTableLength;
    uint16_t                    fSMBIOSNumStructures;
    uint8_t                     fSMBIOSMajorVersion;
    uint8_t                     fSMBIOSMinorVersion;
    bool                        fSMBIOS64Bit;
    
    /* Entry point */
    union {
        struct smbios_entry_32* entry32;
        struct smbios_entry_64* entry64;
        void*                   entry;
    } fEntry;
    uint32_t                    fEntryLength;
    uint64_t                    fEntryPhysAddr;
    
    /* IOKit resources */
    IOWorkLoop*                 fWorkLoop;
    IOCommandGate*              fCommandGate;
    
    /* Locking */
    lck_mtx_t*                  fLock;
    lck_grp_t*                  fLockGroup;
    lck_attr_t*                 fLockAttr;
    
    /* Cached system information */
    char                        fManufacturer[64];
    char                        fProductName[64];
    char                        fProductVersion[64];
    char                        fSerialNumber[64];
    char                        fSystemUUID[37];
    char                        fFamily[64];
    char                        fSKU[64];
    
    /* Cached baseboard information */
    char                        fBoardManufacturer[64];
    char                        fBoardProduct[64];
    char                        fBoardVersion[64];
    char                        fBoardSerial[64];
    char                        fBoardAssetTag[64];
    char                        fBoardLocation[64];
    uint8_t                     fBoardType;
    
    /* Cached BIOS information */
    char                        fBiosVendor[64];
    char                        fBiosVersion[64];
    char                        fBiosDate[64];
    uint8_t                     fBiosMajor;
    uint8_t                     fBiosMinor;
    uint64_t                    fBiosCharacteristics;
    
    /* Cached processor information */
    uint32_t                    fProcessorCount;
    uint32_t                    fCoreCount;
    uint32_t                    fThreadCount;
    uint16_t                    fMaxSpeed;
    uint16_t                    fCurrentSpeed;
    uint64_t                    fProcessorID;
    char                        fProcessorManufacturer[64];
    char                        fProcessorVersion[64];
    
    /* Cached memory information */
    uint32_t                    fMemoryDevices;
    uint64_t                    fTotalMemory;
    uint32_t                    fMemorySpeed;
    uint8_t                     fMemoryType;
    uint8_t                     fMemoryFormFactor;
    
    /* Apple-specific */
    uint8_t                     fBoardID[16];
    uint8_t                     fChipID[16];
    uint8_t                     fPlatformUUID[16];
    uint8_t                     fMLBSerial[32];
    uint8_t                     fROMSerial[12];
    uint32_t                    fBootROMVersion;
    uint32_t                    fFirmwareFeatures;
    uint8_t                     fSecurityFlags;
    
    /* State */
    bool                        fValidTables;
    bool                        fInitialized;
    uint32_t                    fErrorCount;
    
    /* Private methods */
    bool                        findSMBIOSEntryPoint(void);
    bool                        validateEntryPoint(void);
    bool                        mapSMBIOSTables(void);
    void                        unmapSMBIOSTables(void);
    
    uint8_t*                    findStructure(uint8_t type, uint16_t* handle, int instance);
    const char*                 getStringAtIndex(uint8_t* structure, uint8_t index);
    
    bool                        parseSystemInfo(void);
    bool                        parseBaseboardInfo(void);
    bool                        parseBIOSInfo(void);
    bool                        parseProcessorInfo(void);
    bool                        parseMemoryInfo(void);
    bool                        parseAppleInfo(void);
    
    void                        publishProperties(void);
    void                        publishMemoryProperties(void);
    void                        publishProcessorProperties(void);
    
    bool                        verifyChecksum(void* data, size_t length);
    
protected:
    bool                        init(OSDictionary* dictionary) APPLE_KEXT_OVERRIDE;
    void                        free(void) APPLE_KEXT_OVERRIDE;
    
public:
    bool                        start(IOService* provider) APPLE_KEXT_OVERRIDE;
    void                        stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    
    /* Platform Expert integration */
    virtual bool                serializeProperties(OSSerialize* s) const APPLE_KEXT_OVERRIDE;
    
    /* Accessor methods for other drivers */
    const char*                 getProductName(void) { return fProductName; }
    const char*                 getSerialNumber(void) { return fSerialNumber; }
    const char*                 getSystemUUID(void) { return fSystemUUID; }
    const char*                 getBoardSerial(void) { return fBoardSerial; }
    uint64_t                    getTotalMemory(void) { return fTotalMemory; }
    
    /* Debug */
    void                        dumpSMBIOSTables(void);
};

OSDefineMetaClassAndStructors(AppleSMBIOS, IOService)

/*==============================================================================
 * AppleSMBIOS Initialization
 *==============================================================================*/

bool AppleSMBIOS::init(OSDictionary* dictionary)
{
    if (!super::init(dictionary)) {
        return false;
    }
    
    /* Initialize state */
    fSMBIOSTable = NULL;
    fSMBIOSTableLength = 0;
    fSMBIOSNumStructures = 0;
    fSMBIOSMajorVersion = 0;
    fSMBIOSMinorVersion = 0;
    fSMBIOS64Bit = false;
    fEntry.entry = NULL;
    fEntryLength = 0;
    fEntryPhysAddr = 0;
    fWorkLoop = NULL;
    fCommandGate = NULL;
    fLock = NULL;
    fLockGroup = NULL;
    fLockAttr = NULL;
    fValidTables = false;
    fInitialized = false;
    fErrorCount = 0;
    
    /* Clear cached data */
    bzero(fManufacturer, sizeof(fManufacturer));
    bzero(fProductName, sizeof(fProductName));
    bzero(fProductVersion, sizeof(fProductVersion));
    bzero(fSerialNumber, sizeof(fSerialNumber));
    bzero(fSystemUUID, sizeof(fSystemUUID));
    bzero(fFamily, sizeof(fFamily));
    bzero(fSKU, sizeof(fSKU));
    bzero(fBoardManufacturer, sizeof(fBoardManufacturer));
    bzero(fBoardProduct, sizeof(fBoardProduct));
    bzero(fBoardVersion, sizeof(fBoardVersion));
    bzero(fBoardSerial, sizeof(fBoardSerial));
    bzero(fBoardAssetTag, sizeof(fBoardAssetTag));
    bzero(fBoardLocation, sizeof(fBoardLocation));
    bzero(fBiosVendor, sizeof(fBiosVendor));
    bzero(fBiosVersion, sizeof(fBiosVersion));
    bzero(fBiosDate, sizeof(fBiosDate));
    bzero(fProcessorManufacturer, sizeof(fProcessorManufacturer));
    bzero(fProcessorVersion, sizeof(fProcessorVersion));
    bzero(fBoardID, sizeof(fBoardID));
    bzero(fChipID, sizeof(fChipID));
    bzero(fPlatformUUID, sizeof(fPlatformUUID));
    bzero(fMLBSerial, sizeof(fMLBSerial));
    bzero(fROMSerial, sizeof(fROMSerial));
    
    fProcessorCount = 0;
    fCoreCount = 0;
    fThreadCount = 0;
    fMaxSpeed = 0;
    fCurrentSpeed = 0;
    fProcessorID = 0;
    fMemoryDevices = 0;
    fTotalMemory = 0;
    fMemorySpeed = 0;
    fMemoryType = 0;
    fMemoryFormFactor = 0;
    fBiosMajor = 0;
    fBiosMinor = 0;
    fBiosCharacteristics = 0;
    fBootROMVersion = 0;
    fFirmwareFeatures = 0;
    fSecurityFlags = 0;
    fBoardType = 0;
    
    /* Create lock group */
    fLockGroup = lck_grp_alloc_init("AppleSMBIOS", LCK_GRP_ATTR_NULL);
    if (!fLockGroup) {
        return false;
    }
    
    fLockAttr = lck_attr_alloc_init();
    if (!fLockAttr) {
        lck_grp_free(fLockGroup);
        return false;
    }
    
    fLock = lck_mtx_alloc_init(fLockGroup, fLockAttr);
    if (!fLock) {
        lck_attr_free(fLockAttr);
        lck_grp_free(fLockGroup);
        return false;
    }
    
    return true;
}

void AppleSMBIOS::free(void)
{
    /* Unmap SMBIOS tables if mapped */
    unmapSMBIOSTables();
    
    /* Free lock */
    if (fLock) {
        lck_mtx_free(fLock, fLockGroup);
        fLock = NULL;
    }
    
    if (fLockAttr) {
        lck_attr_free(fLockAttr);
        fLockAttr = NULL;
    }
    
    if (fLockGroup) {
        lck_grp_free(fLockGroup);
        fLockGroup = NULL;
    }
    
    super::free();
}

/*==============================================================================
 * SMBIOS Entry Point Detection
 *==============================================================================*/

bool AppleSMBIOS::findSMBIOSEntryPoint(void)
{
    /* Search for SMBIOS entry point in physical memory range 0xF0000-0xFFFFF */
    uint8_t* search_base;
    uint32_t search_length = 0x10000;  /* 64KB */
    uint32_t offset;
    
    /* Map the BIOS area */
    IOMemoryDescriptor* memDesc = IOMemoryDescriptor::withPhysicalAddress(
        0xF0000, search_length, kIODirectionIn);
    
    if (!memDesc) {
        IOLog("AppleSMBIOS: Failed to create memory descriptor for BIOS area\n");
        return false;
    }
    
    IOMemoryMap* map = memDesc->map();
    if (!map) {
        IOLog("AppleSMBIOS: Failed to map BIOS area\n");
        memDesc->release();
        return false;
    }
    
    search_base = (uint8_t*)map->getVirtualAddress();
    
    /* Search for 32-bit SMBIOS entry point */
    for (offset = 0; offset <= search_length - sizeof(struct smbios_entry_32); offset += 16) {
        struct smbios_entry_32* entry = (struct smbios_entry_32*)(search_base + offset);
        
        if (memcmp(entry->anchor, SMBIOS_ANCHOR_32, SMBIOS_ANCHOR_32_LEN) == 0 &&
            memcmp(entry->intermediate_anchor, SMBIOS_INTERMEDIATE_ANCHOR, SMBIOS_INTERMEDIATE_LEN) == 0) {
            
            /* Found 32-bit entry point */
            fEntry.entry32 = (struct smbios_entry_32*)IOMalloc(sizeof(struct smbios_entry_32));
            if (!fEntry.entry32) {
                map->release();
                memDesc->release();
                return false;
            }
            
            memcpy(fEntry.entry32, entry, sizeof(struct smbios_entry_32));
            fEntryPhysAddr = 0xF0000 + offset;
            fEntryLength = sizeof(struct smbios_entry_32);
            fSMBIOS64Bit = false;
            
            IOLog("AppleSMBIOS: Found 32-bit SMBIOS entry point at 0x%x\n", (uint32_t)(0xF0000 + offset));
            
            map->release();
            memDesc->release();
            return true;
        }
    }
    
    /* Search for 64-bit SMBIOS entry point */
    for (offset = 0; offset <= search_length - sizeof(struct smbios_entry_64); offset += 16) {
        struct smbios_entry_64* entry = (struct smbios_entry_64*)(search_base + offset);
        
        if (memcmp(entry->anchor, SMBIOS_ANCHOR_64, SMBIOS_ANCHOR_64_LEN) == 0) {
            
            /* Found 64-bit entry point */
            fEntry.entry64 = (struct smbios_entry_64*)IOMalloc(sizeof(struct smbios_entry_64));
            if (!fEntry.entry64) {
                map->release();
                memDesc->release();
                return false;
            }
            
            memcpy(fEntry.entry64, entry, sizeof(struct smbios_entry_64));
            fEntryPhysAddr = 0xF0000 + offset;
            fEntryLength = sizeof(struct smbios_entry_64);
            fSMBIOS64Bit = true;
            
            IOLog("AppleSMBIOS: Found 64-bit SMBIOS entry point at 0x%x\n", (uint32_t)(0xF0000 + offset));
            
            map->release();
            memDesc->release();
            return true;
        }
    }
    
    IOLog("AppleSMBIOS: No SMBIOS entry point found\n");
    
    map->release();
    memDesc->release();
    return false;
}

bool AppleSMBIOS::validateEntryPoint(void)
{
    if (!fEntry.entry) {
        return false;
    }
    
    /* Verify checksums */
    if (!fSMBIOS64Bit) {
        struct smbios_entry_32* entry = fEntry.entry32;
        
        /* Verify entry point checksum */
        if (!verifyChecksum(entry, entry->length)) {
            IOLog("AppleSMBIOS: Entry point checksum failed\n");
            return false;
        }
        
        /* Verify intermediate checksum */
        uint8_t* intermediate = (uint8_t*)&entry->intermediate_anchor;
        if (!verifyChecksum(intermediate, 0x0F)) {  /* 15 bytes from intermediate anchor */
            IOLog("AppleSMBIOS: Intermediate checksum failed\n");
            return false;
        }
        
        fSMBIOSMajorVersion = entry->major_version;
        fSMBIOSMinorVersion = entry->minor_version;
        fSMBIOSNumStructures = entry->number_of_structures;
        fSMBIOSTableLength = entry->structure_table_length;
        
        IOLog("AppleSMBIOS: SMBIOS %d.%d found with %d structures, table length %d\n",
              fSMBIOSMajorVersion, fSMBIOSMinorVersion,
              fSMBIOSNumStructures, fSMBIOSTableLength);
        
    } else {
        struct smbios_entry_64* entry = fEntry.entry64;
        
        /* Verify checksum */
        if (!verifyChecksum(entry, entry->length)) {
            IOLog("AppleSMBIOS: Entry point checksum failed\n");
            return false;
        }
        
        fSMBIOSMajorVersion = entry->major_version;
        fSMBIOSMinorVersion = entry->minor_version;
        fSMBIOSNumStructures = 0;  /* Not provided in 64-bit entry */
        fSMBIOSTableLength = entry->structure_table_max_size;
        
        IOLog("AppleSMBIOS: SMBIOS 3.%d.%d found, table max size %d\n",
              fSMBIOSMajorVersion, fSMBIOSMinorVersion, fSMBIOSTableLength);
    }
    
    return true;
}

bool AppleSMBIOS::verifyChecksum(void* data, size_t length)
{
    uint8_t* bytes = (uint8_t*)data;
    uint8_t sum = 0;
    
    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    
    return (sum == 0);
}

/*==============================================================================
 * SMBIOS Table Mapping
 *==============================================================================*/

bool AppleSMBIOS::mapSMBIOSTables(void)
{
    uint64_t tableAddress;
    uint32_t tableLength = fSMBIOSTableLength;
    
    if (!fEntry.entry) {
        return false;
    }
    
    /* Get table address from entry point */
    if (!fSMBIOS64Bit) {
        tableAddress = fEntry.entry32->structure_table_address;
    } else {
        tableAddress = fEntry.entry64->structure_table_address;
    }
    
    IOLog("AppleSMBIOS: Mapping SMBIOS table at physical 0x%llx, length %u\n",
          tableAddress, tableLength);
    
    /* Map the SMBIOS table */
    IOMemoryDescriptor* memDesc = IOMemoryDescriptor::withPhysicalAddress(
        tableAddress, tableLength, kIODirectionIn);
    
    if (!memDesc) {
        IOLog("AppleSMBIOS: Failed to create memory descriptor for SMBIOS table\n");
        return false;
    }
    
    IOMemoryMap* map = memDesc->map();
    if (!map) {
        IOLog("AppleSMBIOS: Failed to map SMBIOS table\n");
        memDesc->release();
        return false;
    }
    
    fSMBIOSTable = (uint8_t*)IOMalloc(tableLength);
    if (!fSMBIOSTable) {
        IOLog("AppleSMBIOS: Failed to allocate memory for SMBIOS table\n");
        map->release();
        memDesc->release();
        return false;
    }
    
    /* Copy the table */
    memcpy(fSMBIOSTable, (void*)map->getVirtualAddress(), tableLength);
    
    map->release();
    memDesc->release();
    
    fValidTables = true;
    
    return true;
}

void AppleSMBIOS::unmapSMBIOSTables(void)
{
    if (fSMBIOSTable) {
        IOFree(fSMBIOSTable, fSMBIOSTableLength);
        fSMBIOSTable = NULL;
    }
    
    if (fEntry.entry) {
        IOFree(fEntry.entry, fEntryLength);
        fEntry.entry = NULL;
    }
    
    fValidTables = false;
}

/*==============================================================================
 * SMBIOS Structure Access
 *==============================================================================*/

uint8_t* AppleSMBIOS::findStructure(uint8_t type, uint16_t* handle, int instance)
{
    uint8_t* current = fSMBIOSTable;
    uint8_t* end = fSMBIOSTable + fSMBIOSTableLength;
    int found = 0;
    
    if (!fValidTables || !current) {
        return NULL;
    }
    
    while (current < end) {
        struct smbios_header* header = (struct smbios_header*)current;
        
        if (header->type == SMBIOS_TYPE_END_OF_TABLE) {
            break;
        }
        
        if (header->type == type) {
            if (found == instance) {
                if (handle) {
                    *handle = header->handle;
                }
                return current;
            }
            found++;
        }
        
        /* Skip to next structure */
        current += header->length;
        
        /* Skip strings */
        while (*current != 0 || *(current + 1) != 0) {
            current++;
        }
        current += 2;  /* Skip double null terminator */
    }
    
    return NULL;
}

const char* AppleSMBIOS::getStringAtIndex(uint8_t* structure, uint8_t index)
{
    uint8_t* current = structure + ((struct smbios_header*)structure)->length;
    int i = 1;
    
    if (index == 0) {
        return NULL;
    }
    
    while (i < index) {
        while (*current != 0) {
            current++;
        }
        current++;
        i++;
    }
    
    return (const char*)current;
}

/*==============================================================================
 * SMBIOS Structure Parsing
 *==============================================================================*/

bool AppleSMBIOS::parseSystemInfo(void)
{
    uint8_t* structure;
    struct smbios_type_1* sysinfo;
    
    structure = findStructure(SMBIOS_TYPE_SYSTEM, NULL, 0);
    if (!structure) {
        IOLog("AppleSMBIOS: System Information structure not found\n");
        return false;
    }
    
    sysinfo = (struct smbios_type_1*)structure;
    
    /* Extract strings */
    if (sysinfo->manufacturer) {
        const char* str = getStringAtIndex(structure, sysinfo->manufacturer);
        if (str) strlcpy(fManufacturer, str, sizeof(fManufacturer));
    }
    
    if (sysinfo->product_name) {
        const char* str = getStringAtIndex(structure, sysinfo->product_name);
        if (str) strlcpy(fProductName, str, sizeof(fProductName));
    }
    
    if (sysinfo->version) {
        const char* str = getStringAtIndex(structure, sysinfo->version);
        if (str) strlcpy(fProductVersion, str, sizeof(fProductVersion));
    }
    
    if (sysinfo->serial_number) {
        const char* str = getStringAtIndex(structure, sysinfo->serial_number);
        if (str) strlcpy(fSerialNumber, str, sizeof(fSerialNumber));
    }
    
    if (sysinfo->sku_number) {
        const char* str = getStringAtIndex(structure, sysinfo->sku_number);
        if (str) strlcpy(fSKU, str, sizeof(fSKU));
    }
    
    if (sysinfo->family) {
        const char* str = getStringAtIndex(structure, sysinfo->family);
        if (str) strlcpy(fFamily, str, sizeof(fFamily));
    }
    
    /* Format UUID */
    snprintf(fSystemUUID, sizeof(fSystemUUID),
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             sysinfo->uuid[0], sysinfo->uuid[1], sysinfo->uuid[2], sysinfo->uuid[3],
             sysinfo->uuid[4], sysinfo->uuid[5], sysinfo->uuid[6], sysinfo->uuid[7],
             sysinfo->uuid[8], sysinfo->uuid[9], sysinfo->uuid[10], sysinfo->uuid[11],
             sysinfo->uuid[12], sysinfo->uuid[13], sysinfo->uuid[14], sysinfo->uuid[15]);
    
    IOLog("AppleSMBIOS: System: %s %s (S/N %s)\n",
          fManufacturer, fProductName, fSerialNumber);
    
    return true;
}

bool AppleSMBIOS::parseBaseboardInfo(void)
{
    uint8_t* structure;
    struct smbios_type_2* board;
    
    structure = findStructure(SMBIOS_TYPE_BASEBOARD, NULL, 0);
    if (!structure) {
        IOLog("AppleSMBIOS: Baseboard Information structure not found\n");
        return false;
    }
    
    board = (struct smbios_type_2*)structure;
    
    /* Extract strings */
    if (board->manufacturer) {
        const char* str = getStringAtIndex(structure, board->manufacturer);
        if (str) strlcpy(fBoardManufacturer, str, sizeof(fBoardManufacturer));
    }
    
    if (board->product) {
        const char* str = getStringAtIndex(structure, board->product);
        if (str) strlcpy(fBoardProduct, str, sizeof(fBoardProduct));
    }
    
    if (board->version) {
        const char* str = getStringAtIndex(structure, board->version);
        if (str) strlcpy(fBoardVersion, str, sizeof(fBoardVersion));
    }
    
    if (board->serial_number) {
        const char* str = getStringAtIndex(structure, board->serial_number);
        if (str) strlcpy(fBoardSerial, str, sizeof(fBoardSerial));
    }
    
    if (board->asset_tag) {
        const char* str = getStringAtIndex(structure, board->asset_tag);
        if (str) strlcpy(fBoardAssetTag, str, sizeof(fBoardAssetTag));
    }
    
    if (board->location) {
        const char* str = getStringAtIndex(structure, board->location);
        if (str) strlcpy(fBoardLocation, str, sizeof(fBoardLocation));
    }
    
    fBoardType = board->board_type;
    
    IOLog("AppleSMBIOS: Baseboard: %s %s (S/N %s)\n",
          fBoardManufacturer, fBoardProduct, fBoardSerial);
    
    return true;
}

bool AppleSMBIOS::parseBIOSInfo(void)
{
    uint8_t* structure;
    struct smbios_type_0* bios;
    
    structure = findStructure(SMBIOS_TYPE_BIOS, NULL, 0);
    if (!structure) {
        IOLog("AppleSMBIOS: BIOS Information structure not found\n");
        return false;
    }
    
    bios = (struct smbios_type_0*)structure;
    
    /* Extract strings */
    if (bios->vendor) {
        const char* str = getStringAtIndex(structure, bios->vendor);
        if (str) strlcpy(fBiosVendor, str, sizeof(fBiosVendor));
    }
    
    if (bios->version) {
        const char* str = getStringAtIndex(structure, bios->version);
        if (str) strlcpy(fBiosVersion, str, sizeof(fBiosVersion));
    }
    
    if (bios->release_date) {
        const char* str = getStringAtIndex(structure, bios->release_date);
        if (str) strlcpy(fBiosDate, str, sizeof(fBiosDate));
    }
    
    fBiosMajor = bios->system_bios_major;
    fBiosMinor = bios->system_bios_minor;
    fBiosCharacteristics = bios->characteristics;
    
    IOLog("AppleSMBIOS: BIOS: %s %s (%d.%d)\n",
          fBiosVendor, fBiosVersion, fBiosMajor, fBiosMinor);
    
    return true;
}

bool AppleSMBIOS::parseProcessorInfo(void)
{
    uint8_t* structure;
    struct smbios_type_4* cpu;
    int instance = 0;
    
    fProcessorCount = 0;
    fCoreCount = 0;
    fThreadCount = 0;
    
    while ((structure = findStructure(SMBIOS_TYPE_PROCESSOR, NULL, instance)) != NULL) {
        cpu = (struct smbios_type_4*)structure;
        
        fProcessorCount++;
        
        /* Accumulate core/thread counts */
        if (cpu->core_count2 != 0) {
            fCoreCount += cpu->core_count2;
            fThreadCount += cpu->thread_count2;
        } else {
            fCoreCount += cpu->core_count;
            fThreadCount += cpu->thread_count;
        }
        
        /* Use first processor for speed info */
        if (instance == 0) {
            fMaxSpeed = cpu->max_speed;
            fCurrentSpeed = cpu->current_speed;
            fProcessorID = cpu->processor_id;
            
            if (cpu->manufacturer) {
                const char* str = getStringAtIndex(structure, cpu->manufacturer);
                if (str) strlcpy(fProcessorManufacturer, str, sizeof(fProcessorManufacturer));
            }
            
            if (cpu->processor_version) {
                const char* str = getStringAtIndex(structure, cpu->processor_version);
                if (str) strlcpy(fProcessorVersion, str, sizeof(fProcessorVersion));
            }
        }
        
        instance++;
    }
    
    IOLog("AppleSMBIOS: Processors: %d, Cores: %d, Threads: %d, Speed: %d MHz\n",
          fProcessorCount, fCoreCount, fThreadCount, fCurrentSpeed);
    
    return (fProcessorCount > 0);
}

bool AppleSMBIOS::parseMemoryInfo(void)
{
    uint8_t* structure;
    struct smbios_type_16* array;
    struct smbios_type_17* device;
    int instance = 0;
    
    /* Find memory array */
    structure = findStructure(SMBIOS_TYPE_PHYSICAL_MEMORY, NULL, 0);
    if (!structure) {
        IOLog("AppleSMBIOS: Physical Memory Array structure not found\n");
        return false;
    }
    
    array = (struct smbios_type_16*)structure;
    
    if (array->extended_max_capacity != 0) {
        fTotalMemory = array->extended_max_capacity * 1024; /* Convert KB to bytes */
    } else if (array->max_capacity != 0x80000000) {
        fTotalMemory = (uint64_t)array->max_capacity * 1024;
    }
    
    /* Count memory devices and get details */
    fMemoryDevices = 0;
    fMemorySpeed = 0;
    
    while ((structure = findStructure(SMBIOS_TYPE_MEMORY_DEVICE, NULL, instance)) != NULL) {
        device = (struct smbios_type_17*)structure;
        
        /* Check if device is present (size > 0) */
        if (device->size != 0 || device->extended_size != 0) {
            fMemoryDevices++;
            
            /* Use first device for type/speed info */
            if (instance == 0) {
                fMemoryType = device->memory_type;
                fMemoryFormFactor = device->form_factor;
                
                if (device->extended_configured_clock_speed != 0) {
                    fMemorySpeed = device->extended_configured_clock_speed;
                } else if (device->configured_clock_speed != 0) {
                    fMemorySpeed = device->configured_clock_speed;
                } else {
                    fMemorySpeed = device->speed;
                }
            }
        }
        
        instance++;
    }
    
    IOLog("AppleSMBIOS: Memory: %llu MB total, %d devices, %d MHz\n",
          fTotalMemory / (1024 * 1024), fMemoryDevices, fMemorySpeed);
    
    return true;
}

bool AppleSMBIOS::parseAppleInfo(void)
{
    uint8_t* structure;
    struct smbios_apple_platform* apple;
    
    /* Look for Apple-specific SMBIOS structures */
    structure = findStructure(SMBIOS_APPLE_TYPE_PLATFORM, NULL, 0);
    if (structure) {
        apple = (struct smbios_apple_platform*)structure;
        
        memcpy(fBoardID, apple->board_id, sizeof(fBoardID));
        memcpy(fChipID, apple->chip_id, sizeof(fChipID));
        memcpy(fPlatformUUID, apple->platform_uuid, sizeof(fPlatformUUID));
        memcpy(fMLBSerial, apple->mlb_serial, sizeof(fMLBSerial));
        memcpy(fROMSerial, apple->rom_serial, sizeof(fROMSerial));
        
        fBootROMVersion = apple->boot_rom_version;
        fFirmwareFeatures = apple->firmware_features;
        fSecurityFlags = apple->security_flags;
        
        IOLog("AppleSMBIOS: Apple platform information found\n");
        return true;
    }
    
    IOLog("AppleSMBIOS: Apple-specific platform information not found\n");
    return false;
}

/*==============================================================================
 * Property Publishing
 *==============================================================================*/

void AppleSMBIOS::publishProperties(void)
{
    OSObject* obj;
    
    /* System information */
    if (fManufacturer[0]) setProperty("SMBIOS-Manufacturer", fManufacturer);
    if (fProductName[0]) setProperty("SMBIOS-ProductName", fProductName);
    if (fProductVersion[0]) setProperty("SMBIOS-ProductVersion", fProductVersion);
    if (fSerialNumber[0]) setProperty("SMBIOS-SerialNumber", fSerialNumber);
    if (fSystemUUID[0]) setProperty("SMBIOS-SystemUUID", fSystemUUID);
    if (fFamily[0]) setProperty("SMBIOS-Family", fFamily);
    if (fSKU[0]) setProperty("SMBIOS-SKU", fSKU);
    
    /* Baseboard information */
    if (fBoardManufacturer[0]) setProperty("SMBIOS-BoardManufacturer", fBoardManufacturer);
    if (fBoardProduct[0]) setProperty("SMBIOS-BoardProduct", fBoardProduct);
    if (fBoardVersion[0]) setProperty("SMBIOS-BoardVersion", fBoardVersion);
    if (fBoardSerial[0]) setProperty("SMBIOS-BoardSerial", fBoardSerial);
    if (fBoardAssetTag[0]) setProperty("SMBIOS-BoardAssetTag", fBoardAssetTag);
    if (fBoardLocation[0]) setProperty("SMBIOS-BoardLocation", fBoardLocation);
    setProperty("SMBIOS-BoardType", fBoardType, 8);
    
    /* BIOS information */
    if (fBiosVendor[0]) setProperty("SMBIOS-BIOSVendor", fBiosVendor);
    if (fBiosVersion[0]) setProperty("SMBIOS-BIOSVersion", fBiosVersion);
    if (fBiosDate[0]) setProperty("SMBIOS-BIOSDate", fBiosDate);
    setProperty("SMBIOS-BIOSMajor", fBiosMajor, 8);
    setProperty("SMBIOS-BIOSMinor", fBiosMinor, 8);
    
    /* Processor information */
    setProperty("SMBIOS-ProcessorCount", fProcessorCount, 32);
    setProperty("SMBIOS-CoreCount", fCoreCount, 32);
    setProperty("SMBIOS-ThreadCount", fThreadCount, 32);
    setProperty("SMBIOS-ProcessorMaxSpeed", fMaxSpeed, 16);
    setProperty("SMBIOS-ProcessorCurrentSpeed", fCurrentSpeed, 16);
    if (fProcessorManufacturer[0]) setProperty("SMBIOS-ProcessorManufacturer", fProcessorManufacturer);
    if (fProcessorVersion[0]) setProperty("SMBIOS-ProcessorVersion", fProcessorVersion);
    
    /* Memory information */
    setProperty("SMBIOS-MemoryDevices", fMemoryDevices, 32);
    setProperty("SMBIOS-MemorySpeed", fMemorySpeed, 32);
    setProperty("SMBIOS-MemoryType", fMemoryType, 8);
    setProperty("SMBIOS-MemoryFormFactor", fMemoryFormFactor, 8);
    
    char memStr[32];
    snprintf(memStr, sizeof(memStr), "%llu MB", fTotalMemory / (1024 * 1024));
    setProperty("SMBIOS-MemorySize", memStr);
    
    /* Apple-specific */
    if (fBoardID[0]) setProperty("Apple-BoardID", OSData::withBytes(fBoardID, sizeof(fBoardID)));
    if (fChipID[0]) setProperty("Apple-ChipID", OSData::withBytes(fChipID, sizeof(fChipID)));
    if (fPlatformUUID[0]) setProperty("Apple-PlatformUUID", OSData::withBytes(fPlatformUUID, sizeof(fPlatformUUID)));
    setProperty("Apple-BootROMVersion", fBootROMVersion, 32);
    setProperty("Apple-FirmwareFeatures", fFirmwareFeatures, 32);
    setProperty("Apple-SecurityFlags", fSecurityFlags, 8);
    
    /* Version information */
    char smbiosVer[16];
    snprintf(smbiosVer, sizeof(smbiosVer), "%d.%d", fSMBIOSMajorVersion, fSMBIOSMinorVersion);
    setProperty("SMBIOS-Version", smbiosVer);
    setProperty("SMBIOS-64Bit", fSMBIOS64Bit ? kOSBooleanTrue : kOSBooleanFalse);
    
    /* Platform Expert keys (for compatibility) */
    if (fProductName[0]) getPlatform()->setProperty(kIOPlatformModelKey, fProductName);
    if (fSerialNumber[0]) getPlatform()->setProperty(kIOPlatformSerialNumberKey, fSerialNumber);
    if (fSystemUUID[0]) getPlatform()->setProperty(kIOPlatformUUIDKey, fSystemUUID);
}

void AppleSMBIOS::publishMemoryProperties(void)
{
    /* Publish memory configuration for X86PlatformPlugin */
    OSArray* memArray = OSArray::withCapacity(fMemoryDevices);
    
    /* In real implementation, would iterate through all memory devices
       and publish detailed information for each slot */
    
    if (memArray) {
        setProperty("Memory Device Map", memArray);
        memArray->release();
    }
}

void AppleSMBIOS::publishProcessorProperties(void)
{
    /* Publish processor configuration for X86PlatformPlugin */
    OSArray* cpuArray = OSArray::withCapacity(fProcessorCount);
    
    /* In real implementation, would publish per-processor details */
    
    if (cpuArray) {
        setProperty("Processor Map", cpuArray);
        cpuArray->release();
    }
}

/*==============================================================================
 * Debug
 *==============================================================================*/

void AppleSMBIOS::dumpSMBIOSTables(void)
{
    uint8_t* current = fSMBIOSTable;
    uint8_t* end = fSMBIOSTable + fSMBIOSTableLength;
    int structCount = 0;
    
    IOLog("AppleSMBIOS: Dumping SMBIOS tables (version %d.%d)\n",
          fSMBIOSMajorVersion, fSMBIOSMinorVersion);
    
    while (current < end) {
        struct smbios_header* header = (struct smbios_header*)current;
        
        if (header->type == SMBIOS_TYPE_END_OF_TABLE) {
            IOLog("AppleSMBIOS: End of table marker\n");
            break;
        }
        
        IOLog("AppleSMBIOS: Structure %d: Type %d, Length %d, Handle 0x%04x\n",
              structCount++, header->type, header->length, header->handle);
        
        /* Skip to next structure */
        current += header->length;
        
        /* Skip strings */
        while (*current != 0 || *(current + 1) != 0) {
            current++;
        }
        current += 2;  /* Skip double null terminator */
        
        if (structCount >= fSMBIOSNumStructures && !fSMBIOS64Bit) {
            break;
        }
    }
    
    IOLog("AppleSMBIOS: Dump complete, %d structures found\n", structCount);
}

/*==============================================================================
 * IOService Overrides
 *==============================================================================*/

bool AppleSMBIOS::start(IOService* provider)
{
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("AppleSMBIOS: Starting SMBIOS interface driver\n");
    
#if !defined(__x86_64__)
    IOLog("AppleSMBIOS: Not supported on non-x86 platform\n");
    return false;
#endif
    
    /* Find and validate SMBIOS entry point */
    if (!findSMBIOSEntryPoint()) {
        IOLog("AppleSMBIOS: Failed to find SMBIOS entry point\n");
        return false;
    }
    
    if (!validateEntryPoint()) {
        IOLog("AppleSMBIOS: Invalid SMBIOS entry point\n");
        return false;
    }
    
    /* Map SMBIOS tables */
    if (!mapSMBIOSTables()) {
        IOLog("AppleSMBIOS: Failed to map SMBIOS tables\n");
        return false;
    }
    
    /* Parse SMBIOS structures */
    if (!parseSystemInfo()) {
        IOLog("AppleSMBIOS: Failed to parse system information\n");
        fErrorCount++;
    }
    
    if (!parseBaseboardInfo()) {
        IOLog("AppleSMBIOS: Failed to parse baseboard information\n");
        fErrorCount++;
    }
    
    if (!parseBIOSInfo()) {
        IOLog("AppleSMBIOS: Failed to parse BIOS information\n");
        fErrorCount++;
    }
    
    if (!parseProcessorInfo()) {
        IOLog("AppleSMBIOS: Failed to parse processor information\n");
        fErrorCount++;
    }
    
    if (!parseMemoryInfo()) {
        IOLog("AppleSMBIOS: Failed to parse memory information\n");
        fErrorCount++;
    }
    
    /* Parse Apple-specific information (optional) */
    parseAppleInfo();
    
    /* Publish properties to IORegistry */
    publishProperties();
    publishMemoryProperties();
    publishProcessorProperties();
    
    /* Dump tables if debug enabled */
    if (gIOKitDebug & kIOLogDebug) {
        dumpSMBIOSTables();
    }
    
    /* Validate against expected Apple values */
    if (strstr(fProductName, "Mac") == NULL && strstr(fProductName, "iMac") == NULL &&
        strstr(fProductName, "MacBook") == NULL && strstr(fProductName, "Macmini") == NULL &&
        strstr(fProductName, "MacPro") == NULL && strstr(fProductName, "Xserve") == NULL) {
        
        IOLog("AppleSMBIOS: WARNING - Non-Apple product name detected: %s\n", fProductName);
        
        /* In secure boot, this would cause issues */
#if SECURE_KERNEL
        IOLog("AppleSMBIOS: Unauthorized hardware detected - halting\n");
        return false;
#endif
    }
    
    /* Register service */
    registerService();
    
    fInitialized = true;
    
    IOLog("AppleSMBIOS: SMBIOS interface driver started successfully\n");
    
    return true;
}

void AppleSMBIOS::stop(IOService* provider)
{
    IOLog("AppleSMBIOS: Stopping SMBIOS interface driver\n");
    
    /* Unmap tables */
    unmapSMBIOSTables();
    
    fInitialized = false;
    
    super::stop(provider);
}

bool AppleSMBIOS::serializeProperties(OSSerialize* s) const
{
    return super::serializeProperties(s);
}

/*==============================================================================
 * Kernel Module Entry Points
 *==============================================================================*/

__attribute__((constructor))
static void apple_smbios_constructor(void)
{
    printf("AppleSMBIOS: Constructor\n");
}

#endif
__attribute__((destructor))
static void apple_smbios_destructor(void)
{
    printf("AppleSMBIOS: Destructor\n");
}
