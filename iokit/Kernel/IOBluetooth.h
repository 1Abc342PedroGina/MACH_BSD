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
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#ifndef _IOBLUETOOTH_IOBLUETOOTH_H
#define _IOBLUETOOTH_IOBLUETOOTH_H

#include <sys/param.h>
#include <sys/types.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/errno.h>
#include <sys/conf.h>
#include <sys/ttycom.h>
#include <sys/ioccom.h>
#include <sys/socket.h>
#include <sys/kpi_mbuf.h>

#include <kern/kern_types.h>
#include <kern/locks.h>
#include <kern/zalloc.h>
#include <kern/clock.h>

#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 * Bluetooth Version Constants
 *==============================================================================*/

#define BLUETOOTH_VERSION_MAJOR        1
#define BLUETOOTH_VERSION_MINOR        0
#define BLUETOOTH_VERSION_REVISION      0
#define BLUETOOTH_VERSION_STRING        "1.0.0"

/*==============================================================================
 * Bluetooth Core Specifications Version
 *==============================================================================*/

#define BT_SPEC_VERSION_1_0             0x00
#define BT_SPEC_VERSION_1_1             0x01
#define BT_SPEC_VERSION_1_2             0x02
#define BT_SPEC_VERSION_2_0             0x03
#define BT_SPEC_VERSION_2_1             0x04
#define BT_SPEC_VERSION_3_0             0x05
#define BT_SPEC_VERSION_4_0             0x06
#define BT_SPEC_VERSION_4_1             0x07
#define BT_SPEC_VERSION_4_2             0x08
#define BT_SPEC_VERSION_5_0             0x09
#define BT_SPEC_VERSION_5_1             0x0A
#define BT_SPEC_VERSION_5_2             0x0B
#define BT_SPEC_VERSION_5_3             0x0C
#define BT_SPEC_VERSION_5_4             0x0D

#define BT_SPEC_VERSION_CURRENT         BT_SPEC_VERSION_5_3

/*==============================================================================
 * Bluetooth Device Address
 *==============================================================================*/

typedef struct {
    uint8_t     b[6];
} __attribute__((packed)) bd_addr_t;

#define BD_ADDR_LEN                     6
#define BD_ADDR_FORMAT                  "%02x:%02x:%02x:%02x:%02x:%02x"
#define BD_ADDR_ARG(addr)               (addr).b[5], (addr).b[4], (addr).b[3], \
                                        (addr).b[2], (addr).b[1], (addr).b[0]

#define BD_ADDR_ANY                     { { 0, 0, 0, 0, 0, 0 } }
#define BD_ADDR_LOCAL                    { { 0, 0, 0, 0xFF, 0xFF, 0xFF } }

static inline int
bd_addr_compare(const bd_addr_t* a, const bd_addr_t* b)
{
    return memcmp(a->b, b->b, BD_ADDR_LEN);
}

static inline void
bd_addr_copy(bd_addr_t* dst, const bd_addr_t* src)
{
    memcpy(dst->b, src->b, BD_ADDR_LEN);
}

/*==============================================================================
 * Bluetooth Device Class
 *==============================================================================*/

typedef struct {
    uint8_t     b[3];
} __attribute__((packed)) dev_class_t;

/* Major Device Classes */
#define BT_MAJOR_DEV_MISC               0x00
#define BT_MAJOR_DEV_COMPUTER            0x01
#define BT_MAJOR_DEV_PHONE               0x02
#define BT_MAJOR_DEV_LAN_AP              0x03
#define BT_MAJOR_DEV_AUDIO               0x04
#define BT_MAJOR_DEV_PERIPHERAL          0x05
#define BT_MAJOR_DEV_IMAGING             0x06
#define BT_MAJOR_DEV_WEARABLE            0x07
#define BT_MAJOR_DEV_TOY                 0x08
#define BT_MAJOR_DEV_HEALTH              0x09
#define BT_MAJOR_DEV_UNCATEGORIZED       0x1F

/* Service Classes */
#define BT_SERVICE_POSITIONING           (1 << 16)
#define BT_SERVICE_NETWORKING             (1 << 17)
#define BT_SERVICE_RENDERING              (1 << 18)
#define BT_SERVICE_CAPTURING              (1 << 19)
#define BT_SERVICE_OBJECT_XFER            (1 << 20)
#define BT_SERVICE_AUDIO                  (1 << 21)
#define BT_SERVICE_TELEPHONY              (1 << 22)
#define BT_SERVICE_INFORMATION            (1 << 23)

/*==============================================================================
 * Bluetooth Link Types
 *==============================================================================*/

#define BT_LINK_TYPE_SCO                 0x00
#define BT_LINK_TYPE_ACL                 0x01
#define BT_LINK_TYPE_ESCO                 0x02

/*==============================================================================
 * Bluetooth Packet Types
 *==============================================================================*/

#define BT_PKT_TYPE_NULL                 0x00
#define BT_PKT_TYPE_POLL                 0x01
#define BT_PKT_TYPE_FHS                  0x02
#define BT_PKT_TYPE_DM1                  0x03
#define BT_PKT_TYPE_DH1                  0x04
#define BT_PKT_TYPE_HV1                  0x05
#define BT_PKT_TYPE_HV2                  0x06
#define BT_PKT_TYPE_HV3                  0x07
#define BT_PKT_TYPE_DV                   0x08
#define BT_PKT_TYPE_AUX1                 0x09
#define BT_PKT_TYPE_DM3                  0x0A
#define BT_PKT_TYPE_DH3                  0x0B
#define BT_PKT_TYPE_DM5                  0x0C
#define BT_PKT_TYPE_DH5                  0x0D
#define BT_PKT_TYPE_2DH1                 0x0E
#define BT_PKT_TYPE_2DH3                 0x0F
#define BT_PKT_TYPE_2DH5                 0x10
#define BT_PKT_TYPE_3DH1                 0x11
#define BT_PKT_TYPE_3DH3                 0x12
#define BT_PKT_TYPE_3DH5                 0x13

/*==============================================================================
 * Bluetooth Mode
 *==============================================================================*/

#define BT_MODE_ACTIVE                   0x00
#define BT_MODE_HOLD                     0x01
#define BT_MODE_SNIFF                    0x02
#define BT_MODE_PARK                     0x03

/*==============================================================================
 * Bluetooth Role
 *==============================================================================*/

#define BT_ROLE_MASTER                   0x00
#define BT_ROLE_SLAVE                    0x01

/*==============================================================================
 * Bluetooth Feature Bits (8 bytes)
 *==============================================================================*/

/* Byte 0 - LMP Feature Bits */
#define BT_FEAT_3_SLOT_PACKETS           (1 << 0)
#define BT_FEAT_5_SLOT_PACKETS           (1 << 1)
#define BT_FEAT_ENCRYPTION                (1 << 2)
#define BT_FEAT_SLOT_OFFSET               (1 << 3)
#define BT_FEAT_TIMING_ACCURACY           (1 << 4)
#define BT_FEAT_SWITCH_ROLE               (1 << 5)
#define BT_FEAT_HOLD_MODE                 (1 << 6)
#define BT_FEAT_SNIFF_MODE                (1 << 7)

/* Byte 1 */
#define BT_FEAT_PARK_MODE                 (1 << 0)
#define BT_FEAT_RSSI                      (1 << 1)
#define BT_FEAT_CHANNEL_QUALITY           (1 << 2)
#define BT_FEAT_SCO_LINK                  (1 << 3)
#define BT_FEAT_HV2_PACKETS               (1 << 4)
#define BT_FEAT_HV3_PACKETS               (1 << 5)
#define BT_FEAT_ULAW_LOG                  (1 << 6)
#define BT_FEAT_ALAW_LOG                  (1 << 7)

/* Byte 4 - Bluetooth 4.0 Features */
#define BT_FEAT_LE_SUPPORTED              (1 << 6)

/*==============================================================================
 * Bluetooth Error Codes
 *==============================================================================*/

#define BT_ERROR_SUCCESS                  0x00
#define BT_ERROR_UNKNOWN_HCI              0x01
#define BT_ERROR_UNKNOWN_CONN              0x02
#define BT_ERROR_HARDWARE_FAIL             0x03
#define BT_ERROR_PAGE_TIMEOUT              0x04
#define BT_ERROR_AUTH_FAIL                 0x05
#define BT_ERROR_KEY_MISSING               0x06
#define BT_ERROR_MEMORY_FULL               0x07
#define BT_ERROR_CONN_TIMEOUT              0x08
#define BT_ERROR_MAX_CONN                  0x09
#define BT_ERROR_MAX_SCO                   0x0A
#define BT_ERROR_ACL_LIMIT                 0x0B
#define BT_ERROR_COMMAND_DISALLOWED         0x0C
#define BT_ERROR_REJ_LIMITED                0x0D
#define BT_ERROR_REJ_SECURITY               0x0E
#define BT_ERROR_REJ_BAD_ADDR               0x0F
#define BT_ERROR_HOST_TIMEOUT               0x10
#define BT_ERROR_UNSUPPORTED                0x11
#define BT_ERROR_INVALID_PARAM              0x12
#define BT_ERROR_REMOTE_USER                0x13
#define BT_ERROR_REMOTE_LOW_RES              0x14
#define BT_ERROR_REMOTE_POWER_OFF            0x15
#define BT_ERROR_LOCAL_HOST                  0x16
#define BT_ERROR_REPEATED_ATTEMPTS           0x17
#define BT_ERROR_PAIRING_NOT_ALLOWED         0x18
#define BT_ERROR_UNKNOWN_LMP                 0x19
#define BT_ERROR_UNSUPPORTED_LMP             0x1A
#define BT_ERROR_UNSUPPORTED_REMOTE          0x1B
#define BT_ERROR_SCO_OFFSET                  0x1C
#define BT_ERROR_SCO_INTERVAL                0x1D
#define BT_ERROR_SCO_AIR_MODE                0x1E
#define BT_ERROR_INVALID_LMP                 0x1F
#define BT_ERROR_UNSPECIFIED                 0x20
#define BT_ERROR_UNSUPPORTED_LMP_PARAM       0x21
#define BT_ERROR_ROLE_CHANGE                  0x22
#define BT_ERROR_LMP_RESPONSE                 0x23
#define BT_ERROR_LMP_ERROR                    0x24
#define BT_ERROR_LL_PROCEDURE                 0x25
#define BT_ERROR_LL_RESPONSE                  0x26
#define BT_ERROR_LL_UNKNOWN                   0x27
#define BT_ERROR_LL_INSTANT                   0x28
#define BT_ERROR_LL_UNKNOWN_PDU               0x29
#define BT_ERROR_LL_UNKNOWN_CONN               0x2A
#define BT_ERROR_LL_CONN_FAIL                  0x2B
#define BT_ERROR_LL_CONN_LIMIT                 0x2C
#define BT_ERROR_LL_CONN_PARAM                  0x2D
#define BT_ERROR_LL_CONN_UNACCEPT               0x2E
#define BT_ERROR_LL_CONN_REJECT                  0x2F
#define BT_ERROR_LL_CONN_TERMINATE               0x30
#define BT_ERROR_LL_CONN_FAILED                  0x31
#define BT_ERROR_LL_CONN_UNKNOWN                  0x32
#define BT_ERROR_LL_CONN_TIMEOUT                  0x33

/*==============================================================================
 * HCI Command and Event Structures
 *==============================================================================*/

/* HCI Command Packet */
typedef struct {
    uint16_t    opcode;                 /* Opcode (OGF|OCF) */
    uint8_t     plen;                   /* Parameter length */
    uint8_t     params[0];              /* Parameters */
} __attribute__((packed)) hci_cmd_hdr_t;

#define HCI_OPCODE(ogf, ocf)            (((ogf) << 10) | (ocf))
#define HCI_OGF(opcode)                  (((opcode) >> 10) & 0x3F)
#define HCI_OCF(opcode)                  ((opcode) & 0x3FF)

/* HCI Event Packet */
typedef struct {
    uint8_t     evt;                    /* Event code */
    uint8_t     plen;                   /* Parameter length */
    uint8_t     params[0];              /* Parameters */
} __attribute__((packed)) hci_evt_hdr_t;

/* HCI ACL Data Packet */
typedef struct {
    uint16_t    handle;                 /* Connection handle (12 bits) + flags (4 bits) */
    uint16_t    dlen;                   /* Data length */
    uint8_t     data[0];                /* Data */
} __attribute__((packed)) hci_acl_hdr_t;

#define HCI_HANDLE(handle_flags)         ((handle_flags) & 0x0FFF)
#define HCI_FLAGS(handle_flags)          (((handle_flags) >> 12) & 0x03)

/* HCI SCO Data Packet */
typedef struct {
    uint16_t    handle;                 /* Connection handle */
    uint8_t     dlen;                   /* Data length */
    uint8_t     data[0];                /* Data */
} __attribute__((packed)) hci_sco_hdr_t;

/*==============================================================================
 * HCI Command Opcodes
 *==============================================================================*/

/* Link Control Commands (OGF = 0x01) */
#define HCI_CMD_INQUIRY                 0x0001
#define HCI_CMD_INQUIRY_CANCEL          0x0002
#define HCI_CMD_PERIODIC_INQUIRY        0x0003
#define HCI_CMD_EXIT_PERIODIC_INQUIRY   0x0004
#define HCI_CMD_CREATE_CONN             0x0005
#define HCI_CMD_DISCONNECT               0x0006
#define HCI_CMD_ADD_SCO_CONN             0x0007
#define HCI_CMD_CREATE_CONN_CANCEL       0x0008
#define HCI_CMD_ACCEPT_CONN_REQ          0x0009
#define HCI_CMD_REJECT_CONN_REQ          0x000A
#define HCI_CMD_LINK_KEY_REQ_REPLY       0x000B
#define HCI_CMD_LINK_KEY_REQ_NEG         0x000C
#define HCI_CMD_PIN_CODE_REQ_REPLY       0x000D
#define HCI_CMD_PIN_CODE_REQ_NEG         0x000E
#define HCI_CMD_CHANGE_CONN_PKT_TYPE     0x000F
#define HCI_CMD_AUTH_REQUESTED           0x0011
#define HCI_CMD_SET_CONN_ENCRYPT         0x0013
#define HCI_CMD_CHANGE_LINK_KEY          0x0015
#define HCI_CMD_MASTER_LINK_KEY          0x0017
#define HCI_CMD_REMOTE_NAME_REQ          0x0019
#define HCI_CMD_REMOTE_NAME_REQ_CANCEL   0x001A
#define HCI_CMD_READ_REMOTE_FEATURES     0x001B
#define HCI_CMD_READ_REMOTE_EXT_FEATURES 0x001C
#define HCI_CMD_READ_REMOTE_VERSION      0x001D
#define HCI_CMD_READ_CLOCK_OFFSET        0x001F
#define HCI_CMD_READ_LMP_HANDLE           0x0020

/* Link Policy Commands (OGF = 0x02) */
#define HCI_CMD_HOLD_MODE                0x2001
#define HCI_CMD_SNIFF_MODE               0x2003
#define HCI_CMD_EXIT_SNIFF_MODE          0x2004
#define HCI_CMD_PARK_STATE               0x2005
#define HCI_CMD_EXIT_PARK_STATE          0x2006
#define HCI_CMD_QOS_SETUP                0x2007
#define HCI_CMD_ROLE_DISCOVERY            0x2009
#define HCI_CMD_SWITCH_ROLE               0x200B
#define HCI_CMD_READ_LINK_POLICY          0x200C
#define HCI_CMD_WRITE_LINK_POLICY         0x200D
#define HCI_CMD_READ_DEFAULT_LINK_POLICY  0x200E
#define HCI_CMD_WRITE_DEFAULT_LINK_POLICY 0x200F
#define HCI_CMD_FLOW_SPECIFICATION        0x2010
#define HCI_CMD_SNIFF_SUB_RATE             0x2011

/* Host Control Commands (OGF = 0x03) */
#define HCI_CMD_SET_EVENT_MASK            0x3001
#define HCI_CMD_RESET                     0x3003
#define HCI_CMD_SET_EVENT_FILTER          0x3005
#define HCI_CMD_FLUSH                     0x3008
#define HCI_CMD_READ_PIN_TYPE              0x3009
#define HCI_CMD_WRITE_PIN_TYPE             0x300A
#define HCI_CMD_CREATE_NEW_UNIT_KEY        0x300B
#define HCI_CMD_READ_STORED_LINK_KEY       0x300D
#define HCI_CMD_WRITE_STORED_LINK_KEY      0x3011
#define HCI_CMD_DELETE_STORED_LINK_KEY     0x3012
#define HCI_CMD_WRITE_LOCAL_NAME           0x3013
#define HCI_CMD_READ_LOCAL_NAME            0x3014
#define HCI_CMD_READ_CONN_ACCEPT_TIMEOUT   0x3015
#define HCI_CMD_WRITE_CONN_ACCEPT_TIMEOUT  0x3016
#define HCI_CMD_READ_PAGE_TIMEOUT          0x3017
#define HCI_CMD_WRITE_PAGE_TIMEOUT         0x3018
#define HCI_CMD_READ_SCAN_ENABLE           0x3019
#define HCI_CMD_WRITE_SCAN_ENABLE          0x301A
#define HCI_CMD_READ_PAGE_SCAN_ACTIVITY    0x301B
#define HCI_CMD_WRITE_PAGE_SCAN_ACTIVITY   0x301C
#define HCI_CMD_READ_INQUIRY_SCAN_ACTIVITY 0x301D
#define HCI_CMD_WRITE_INQUIRY_SCAN_ACTIVITY 0x301E
#define HCI_CMD_READ_AUTH_ENABLE           0x301F
#define HCI_CMD_WRITE_AUTH_ENABLE          0x3020
#define HCI_CMD_READ_ENCRYPT_MODE          0x3021
#define HCI_CMD_WRITE_ENCRYPT_MODE         0x3022
#define HCI_CMD_READ_CLASS_OF_DEVICE       0x3023
#define HCI_CMD_WRITE_CLASS_OF_DEVICE      0x3024
#define HCI_CMD_READ_VOICE_SETTING         0x3025
#define HCI_CMD_WRITE_VOICE_SETTING        0x3026
#define HCI_CMD_READ_AUTO_FLUSH_TIMEOUT    0x3027
#define HCI_CMD_WRITE_AUTO_FLUSH_TIMEOUT   0x3028
#define HCI_CMD_READ_NUM_BROADCAST_RETRANS 0x3029
#define HCI_CMD_WRITE_NUM_BROADCAST_RETRANS 0x302A
#define HCI_CMD_READ_HOLD_MODE_ACTIVITY    0x302B
#define HCI_CMD_WRITE_HOLD_MODE_ACTIVITY   0x302C
#define HCI_CMD_READ_TRANSMIT_POWER_LEVEL  0x302D
#define HCI_CMD_READ_SYNCHRONOUS_FLOW      0x302E
#define HCI_CMD_WRITE_SYNCHRONOUS_FLOW     0x302F
#define HCI_CMD_READ_CONTROLLER_BUFFER_SIZE 0x3040
#define HCI_CMD_READ_BD_ADDR               0x3049

/* LE Controller Commands (OGF = 0x08) */
#define HCI_CMD_LE_SET_EVENT_MASK          0x8001
#define HCI_CMD_LE_READ_BUFFER_SIZE        0x8002
#define HCI_CMD_LE_READ_LOCAL_FEATURES     0x8003
#define HCI_CMD_LE_SET_RANDOM_ADDRESS      0x8005
#define HCI_CMD_LE_SET_ADVERTISING_PARAMS  0x8006
#define HCI_CMD_LE_READ_ADVERTISING_TX_POWER 0x8007
#define HCI_CMD_LE_SET_ADVERTISING_DATA    0x8008
#define HCI_CMD_LE_SET_SCAN_RESPONSE_DATA  0x8009
#define HCI_CMD_LE_SET_ADVERTISE_ENABLE    0x800A
#define HCI_CMD_LE_SET_SCAN_PARAMS         0x800B
#define HCI_CMD_LE_SET_SCAN_ENABLE         0x800C
#define HCI_CMD_LE_CREATE_CONN             0x800D
#define HCI_CMD_LE_CREATE_CONN_CANCEL      0x800E
#define HCI_CMD_LE_READ_WHITE_LIST_SIZE    0x800F
#define HCI_CMD_LE_CLEAR_WHITE_LIST        0x8010
#define HCI_CMD_LE_ADD_DEVICE_TO_WHITE_LIST 0x8011
#define HCI_CMD_LE_REMOVE_DEVICE_FROM_WHITE_LIST 0x8012
#define HCI_CMD_LE_CONN_UPDATE             0x8013
#define HCI_CMD_LE_SET_HOST_CHANNEL_CLASS  0x8014
#define HCI_CMD_LE_READ_CHANNEL_MAP        0x8015
#define HCI_CMD_LE_READ_REMOTE_FEATURES    0x8016
#define HCI_CMD_LE_ENCRYPT                  0x8017
#define HCI_CMD_LE_RAND                    0x8018
#define HCI_CMD_LE_START_ENCRYPTION        0x8019
#define HCI_CMD_LE_LTK_REQ_REPLY           0x801A
#define HCI_CMD_LE_LTK_REQ_NEG_REPLY       0x801B
#define HCI_CMD_LE_READ_SUPPORTED_STATES   0x801C
#define HCI_CMD_LE_RECEIVER_TEST           0x801D
#define HCI_CMD_LE_TRANSMITTER_TEST        0x801E
#define HCI_CMD_LE_TEST_END                0x801F
#define HCI_CMD_LE_REMOTE_CONN_PARAM_REQ_REPLY 0x8020
#define HCI_CMD_LE_REMOTE_CONN_PARAM_REQ_NEG_REPLY 0x8021
#define HCI_CMD_LE_SET_DATA_LENGTH         0x8022
#define HCI_CMD_LE_READ_DEFAULT_DATA_LENGTH 0x8023
#define HCI_CMD_LE_WRITE_DEFAULT_DATA_LENGTH 0x8024
#define HCI_CMD_LE_READ_LOCAL_P256_PUBLIC_KEY 0x8025
#define HCI_CMD_LE_GENERATE_DHKEY          0x8026
#define HCI_CMD_LE_ADD_DEVICE_TO_RESOLV_LIST 0x8027
#define HCI_CMD_LE_REMOVE_DEVICE_FROM_RESOLV_LIST 0x8028
#define HCI_CMD_LE_CLEAR_RESOLV_LIST       0x8029
#define HCI_CMD_LE_READ_RESOLV_LIST_SIZE   0x802A
#define HCI_CMD_LE_READ_PEER_RESOLV_ADDR   0x802B
#define HCI_CMD_LE_READ_LOCAL_RESOLV_ADDR  0x802C
#define HCI_CMD_LE_SET_ADDR_RESOLUTION_ENABLE 0x802D
#define HCI_CMD_LE_SET_RESOLV_PRIVATE_ADDR_TIMEOUT 0x802E
#define HCI_CMD_LE_READ_MAX_DATA_LENGTH    0x802F
#define HCI_CMD_LE_SET_PRIVACY_MODE        0x8040

/*==============================================================================
 * HCI Event Codes
 *==============================================================================*/

#define HCI_EVT_INQUIRY_COMPLETE           0x01
#define HCI_EVT_INQUIRY_RESULT             0x02
#define HCI_EVT_CONN_COMPLETE              0x03
#define HCI_EVT_CONN_REQUEST               0x04
#define HCI_EVT_DISCONN_COMPLETE           0x05
#define HCI_EVT_AUTH_COMPLETE              0x06
#define HCI_EVT_REMOTE_NAME_REQ_COMPLETE   0x07
#define HCI_EVT_ENCRYPT_CHANGE             0x08
#define HCI_EVT_CHANGE_LINK_KEY_COMPLETE   0x09
#define HCI_EVT_MASTER_LINK_KEY_COMPLETE   0x0A
#define HCI_EVT_READ_REMOTE_FEAT_COMPLETE  0x0B
#define HCI_EVT_READ_REMOTE_VER_COMPLETE   0x0C
#define HCI_EVT_QOS_SETUP_COMPLETE         0x0D
#define HCI_EVT_COMMAND_COMPLETE           0x0E
#define HCI_EVT_COMMAND_STATUS             0x0F
#define HCI_EVT_HARDWARE_ERROR             0x10
#define HCI_EVT_FLUSH_OCCURRED             0x11
#define HCI_EVT_ROLE_CHANGE                0x12
#define HCI_EVT_NUM_COMPLETED_PKTS         0x13
#define HCI_EVT_MODE_CHANGE                0x14
#define HCI_EVT_RETURN_LINK_KEYS           0x15
#define HCI_EVT_PIN_CODE_REQ               0x16
#define HCI_EVT_LINK_KEY_REQ               0x17
#define HCI_EVT_LINK_KEY_NOTIFY            0x18
#define HCI_EVT_LOOPBACK_COMMAND           0x19
#define HCI_EVT_DATA_BUFFER_OVERFLOW       0x1A
#define HCI_EVT_MAX_SLOTS_CHANGE           0x1B
#define HCI_EVT_READ_CLOCK_OFFSET_COMPLETE 0x1C
#define HCI_EVT_CONN_PKT_TYPE_CHANGE       0x1D
#define HCI_EVT_QOS_VIOLATION              0x1E
#define HCI_EVT_PAGE_SCAN_MODE_CHANGE      0x1F
#define HCI_EVT_PAGE_SCAN_REP_MODE_CHANGE  0x20
#define HCI_EVT_FLOW_SPECIFICATION_COMPLETE 0x21
#define HCI_EVT_INQUIRY_RESULT_RSSI        0x22
#define HCI_EVT_READ_REMOTE_EXT_FEAT_COMPLETE 0x23
#define HCI_EVT_SYNC_CONN_COMPLETE         0x2C
#define HCI_EVT_SYNC_CONN_CHANGED          0x2D
#define HCI_EVT_SNIFF_SUB_RATING           0x2E
#define HCI_EVT_EXTENDED_INQUIRY_RESULT    0x2F
#define HCI_EVT_ENCRYPTION_KEY_REFRESH     0x30
#define HCI_EVT_IO_CAPABILITY_REQ          0x31
#define HCI_EVT_IO_CAPABILITY_RSP          0x32
#define HCI_EVT_USER_CONFIRMATION_REQ      0x33
#define HCI_EVT_USER_PASSKEY_REQ           0x34
#define HCI_EVT_REMOTE_OOB_DATA_REQ        0x35
#define HCI_EVT_SIMPLE_PAIRING_COMPLETE    0x36
#define HCI_EVT_LINK_SUPERVISION_TO_CHANGE 0x38
#define HCI_EVT_ENHANCED_FLUSH_COMPLETE    0x39
#define HCI_EVT_USER_PASSKEY_NOTIFY        0x3B
#define HCI_EVT_KEYPRESS_NOTIFY            0x3C
#define HCI_EVT_REMOTE_HOST_FEATURES_NOTIFY 0x3D
#define HCI_EVT_LE_META                    0x3E
#define HCI_EVT_NUM_COMPLETED_BLOCKS       0x48
#define HCI_EVT_TRIGGERED_CLOCK_CAPTURE    0x4E
#define HCI_EVT_SYNCHRONIZATION_TRAIN_COMPLETE 0x4F
#define HCI_EVT_SYNCHRONIZATION_TRAIN_RECEIVED 0x50
#define HCI_EVT_CONN_BROADCAST_RECV        0x51
#define HCI_EVT_CONN_BROADCAST_TIMEOUT     0x52
#define HCI_EVT_TRUNCATED_PAGE_COMPLETE    0x53
#define HCI_EVT_SALVE_PAGE_RESP_TIMEOUT    0x54
#define HCI_EVT_CONN_BROADCAST_CHANNEL_MAP_CHANGE 0x55
#define HCI_EVT_INQUIRY_RESPONSE_NOTIFY    0x56
#define HCI_EVT_AUTHENTICATED_PAYLOAD_TIMEOUT 0x57
#define HCI_EVT_SAM_STATUS_CHANGE          0x58

/* LE Meta Sub-events */
#define HCI_LEVT_CONN_COMPLETE              0x01
#define HCI_LEVT_ADVERTISING_REPORT         0x02
#define HCI_LEVT_CONN_UPDATE_COMPLETE       0x03
#define HCI_LEVT_READ_REMOTE_FEAT_COMPLETE  0x04
#define HCI_LEVT_LTK_REQUEST                0x05
#define HCI_LEVT_REMOTE_CONN_PARAM_REQ      0x06
#define HCI_LEVT_DATA_LENGTH_CHANGE         0x07
#define HCI_LEVT_READ_LOCAL_P256_COMPLETE   0x08
#define HCI_LEVT_GENERATE_DHKEY_COMPLETE    0x09
#define HCI_LEVT_ENHANCED_CONN_COMPLETE     0x0A
#define HCI_LEVT_DIRECTED_ADV_REPORT        0x0B
#define HCI_LEVT_PHY_UPDATE_COMPLETE        0x0C
#define HCI_LEVT_EXTENDED_ADVERTISING_REPORT 0x0D
#define HCI_LEVT_PERIODIC_ADV_SYNC_ESTABLISHED 0x0E
#define HCI_LEVT_PERIODIC_ADV_REPORT        0x0F
#define HCI_LEVT_PERIODIC_ADV_SYNC_LOST     0x10
#define HCI_LEVT_SCAN_TIMEOUT               0x11
#define HCI_LEVT_ADVERTISING_SET_TERMINATED 0x12
#define HCI_LEVT_SCAN_REQUEST_RECEIVED      0x13
#define HCI_LEVT_CHANNEL_SELECTION_ALGORITHM 0x14
#define HCI_LEVT_CONNLESS_IQ_REPORT         0x15
#define HCI_LEVT_CONN_IQ_REPORT              0x16
#define HCI_LEVT_CTE_REQ_FAILED              0x17
#define HCI_LEVT_PERIODIC_ADV_SYNC_TRANSFER_RECEIVED 0x18
#define HCI_LEVT_CIS_ESTABLISHED             0x19
#define HCI_LEVT_CIS_REQUEST                  0x1A
#define HCI_LEVT_CREATE_BIG_COMPLETE          0x1B
#define HCI_LEVT_TERMINATE_BIG_COMPLETE       0x1C
#define HCI_LEVT_BIG_SYNC_ESTABLISHED         0x1D
#define HCI_LEVT_BIG_SYNC_LOST                0x1E
#define HCI_LEVT_REQ_PEER_SCA_COMPLETE        0x1F
#define HCI_LEVT_PATH_LOSS_THRESHOLD           0x20
#define HCI_LEVT_TRANSMIT_POWER_REPORTING     0x21
#define HCI_LEVT_BIG_INFO_ADV_REPORT          0x22
#define HCI_LEVT_SUBRATE_CHANGE                0x23
#define HCI_LEVT_PRIVACY_MODE_CHANGE           0x24

/*==============================================================================
 * L2CAP Constants
 *==============================================================================*/

/* L2CAP Signaling Commands */
#define L2CAP_CMD_COMMAND_REJECT         0x01
#define L2CAP_CMD_CONNECTION_REQ         0x02
#define L2CAP_CMD_CONNECTION_RSP         0x03
#define L2CAP_CMD_CONFIGURATION_REQ      0x04
#define L2CAP_CMD_CONFIGURATION_RSP      0x05
#define L2CAP_CMD_DISCONNECTION_REQ      0x06
#define L2CAP_CMD_DISCONNECTION_RSP      0x07
#define L2CAP_CMD_ECHO_REQ               0x08
#define L2CAP_CMD_ECHO_RSP               0x09
#define L2CAP_CMD_INFORMATION_REQ        0x0A
#define L2CAP_CMD_INFORMATION_RSP        0x0B
#define L2CAP_CMD_CREATE_CHANNEL_REQ     0x0C
#define L2CAP_CMD_CREATE_CHANNEL_RSP     0x0D
#define L2CAP_CMD_MOVE_CHANNEL_REQ       0x0E
#define L2CAP_CMD_MOVE_CHANNEL_RSP       0x0F
#define L2CAP_CMD_MOVE_CHANNEL_CONFIRM   0x10
#define L2CAP_CMD_MOVE_CHANNEL_CONFIRM_RSP 0x11
#define L2CAP_CMD_CONNECTION_PARAM_UPDATE_REQ 0x12
#define L2CAP_CMD_CONNECTION_PARAM_UPDATE_RSP 0x13
#define L2CAP_CMD_LE_CREDIT_BASED_CONN_REQ 0x14
#define L2CAP_CMD_LE_CREDIT_BASED_CONN_RSP 0x15
#define L2CAP_CMD_FLOW_CONTROL_CREDIT    0x16

/* L2CAP Fixed Channel IDs */
#define L2CAP_CID_NULL                   0x00
#define L2CAP_CID_SIGNALING              0x01
#define L2CAP_CID_CONNECTIONLESS         0x02
#define L2CAP_CID_AMP_MANAGER            0x03
#define L2CAP_CID_ATTRIBUTE              0x04
#define L2CAP_CID_LE_SIGNALING           0x05
#define L2CAP_CID_SECURITY_MANAGER       0x06
#define L2CAP_CID_BREDR_SECURITY_MANAGER 0x07
#define L2CAP_CID_DYNAMIC_MIN            0x40
#define L2CAP_CID_DYNAMIC_MAX            0xFFFF

/* L2CAP Protocol/Service Multiplexers (PSM) */
#define L2CAP_PSM_SDP                    0x0001
#define L2CAP_PSM_RFCOMM                 0x0003
#define L2CAP_PSM_TCS_BIN                0x0005
#define L2CAP_PSM_TCS_BIN_CORDLESS       0x0007
#define L2CAP_PSM_BNEP                   0x000F
#define L2CAP_PSM_HID_CONTROL             0x0011
#define L2CAP_PSM_HID_INTERRUPT           0x0013
#define L2CAP_PSM_AVCTP                   0x0017
#define L2CAP_PSM_AVDTP                   0x0019
#define L2CAP_PSM_AVCTP_BROWSE            0x001B
#define L2CAP_PSM_ATT                     0x001F
#define L2CAP_PSM_3DSP                    0x0021
#define L2CAP_PSM_IPSEC                   0x0023
#define L2CAP_PSM_AMP                     0x0025
#define L2CAP_PSM_GATT                    0x0027
#define L2CAP_PSM_ANQP                    0x0029

/* L2CAP Configuration Options */
#define L2CAP_CONF_OPT_MTU                0x01
#define L2CAP_CONF_OPT_FLUSH_TO           0x02
#define L2CAP_CONF_OPT_QOS                 0x03
#define L2CAP_CONF_OPT_RETRANS             0x04
#define L2CAP_CONF_OPT_FCS                 0x05
#define L2CAP_CONF_OPT_EXT_WINDOW          0x06
#define L2CAP_CONF_OPT_CHAN_MASK           0x07

/* L2CAP Configuration Result Codes */
#define L2CAP_CONF_SUCCESS                 0x00
#define L2CAP_CONF_UNACCEPTABLE_PARAMS     0x01
#define L2CAP_CONF_REJECTED                 0x02
#define L2CAP_CONF_UNKNOWN_OPTIONS         0x03
#define L2CAP_CONF_PENDING                 0x04
#define L2CAP_CONF_FLOW_SPEC_REJECTED      0x05

/* L2CAP Information Types */
#define L2CAP_INFO_CONNLESS_MTU            0x01
#define L2CAP_INFO_EXTENDED_FEATURES       0x02
#define L2CAP_INFO_FIXED_CHANNELS          0x03

/* L2CAP Extended Features */
#define L2CAP_EXT_FEAT_FLOW_CONTROL        (1 << 0)
#define L2CAP_EXT_FEAT_RETRANSMISSION      (1 << 1)
#define L2CAP_EXT_FEAT_BIDIR_QOS           (1 << 2)
#define L2CAP_EXT_FEAT_ENHANCED_RETRANS    (1 << 3)
#define L2CAP_EXT_FEAT_STREAMING           (1 << 4)
#define L2CAP_EXT_FEAT_FCS_OPTION          (1 << 5)
#define L2CAP_EXT_FEAT_EXTENDED_FLOW_SPEC  (1 << 6)
#define L2CAP_EXT_FEAT_FIXED_CHANNELS      (1 << 7)
#define L2CAP_EXT_FEAT_EXT_WINDOW          (1 << 8)
#define L2CAP_EXT_FEAT_UNICAST_CONNLESS    (1 << 9)
#define L2CAP_EXT_FEAT_ECRED_TRANSPORT     (1 << 10)

/* L2CAP Fixed Channels Supported */
#define L2CAP_FIXED_CHAN_SIGNALING         (1 << 0)
#define L2CAP_FIXED_CHAN_CONNLESS          (1 << 1)
#define L2CAP_FIXED_CHAN_AMP_MANAGER       (1 << 2)
#define L2CAP_FIXED_CHAN_ATTRIBUTE         (1 << 3)
#define L2CAP_FIXED_CHAN_LE_SIGNALING      (1 << 4)
#define L2CAP_FIXED_CHAN_SECURITY_MANAGER  (1 << 5)
#define L2CAP_FIXED_CHAN_BREDR_SECURITY    (1 << 6)

/*==============================================================================
 * L2CAP Structures
 *==============================================================================*/

/* L2CAP Command Header */
typedef struct {
    uint8_t     code;                   /* Command code */
    uint8_t     ident;                  /* Command identifier */
    uint16_t    len;                    /* Command length */
    uint8_t     data[0];                /* Command data */
} __attribute__((packed)) l2cap_cmd_hdr_t;

/* L2CAP Connection Request */
typedef struct {
    uint16_t    psm;                    /* Protocol/Service Multiplexer */
    uint16_t    scid;                   /* Source channel ID */
} __attribute__((packed)) l2cap_conn_req_t;

/* L2CAP Connection Response */
typedef struct {
    uint16_t    dcid;                   /* Destination channel ID */
    uint16_t    scid;                   /* Source channel ID */
    uint16_t    result;                 /* Connection result */
    uint16_t    status;                 /* Connection status */
} __attribute__((packed)) l2cap_conn_rsp_t;

/* L2CAP Configuration Request */
typedef struct {
    uint16_t    dcid;                   /* Destination channel ID */
    uint16_t    flags;                  /* Configuration flags */
    uint8_t     options[0];             /* Configuration options */
} __attribute__((packed)) l2cap_config_req_t;

/* L2CAP Configuration Response */
typedef struct {
    uint16_t    scid;                   /* Source channel ID */
    uint16_t    flags;                  /* Configuration flags */
    uint16_t    result;                 /* Configuration result */
    uint8_t     options[0];             /* Configuration options */
} __attribute__((packed)) l2cap_config_rsp_t;

/* L2CAP Disconnection Request */
typedef struct {
    uint16_t    dcid;                   /* Destination channel ID */
    uint16_t    scid;                   /* Source channel ID */
} __attribute__((packed)) l2cap_disconn_req_t;

/* L2CAP Disconnection Response */
typedef struct {
    uint16_t    dcid;                   /* Destination channel ID */
    uint16_t    scid;                   /* Source channel ID */
} __attribute__((packed)) l2cap_disconn_rsp_t;

/* L2CAP Information Request */
typedef struct {
    uint16_t    type;                   /* Information type */
} __attribute__((packed)) l2cap_info_req_t;

/* L2CAP Information Response */
typedef struct {
    uint16_t    type;                   /* Information type */
    uint16_t    result;                 /* Information result */
    uint8_t     data[0];                /* Information data */
} __attribute__((packed)) l2cap_info_rsp_t;

/* L2CAP Connection Parameter Update Request (LE) */
typedef struct {
    uint16_t    min_interval;           /* Minimum connection interval */
    uint16_t    max_interval;           /* Maximum connection interval */
    uint16_t    latency;                /* Connection latency */
    uint16_t    timeout;                /* Supervision timeout */
} __attribute__((packed)) l2cap_conn_param_req_t;

/* L2CAP Connection Parameter Update Response */
typedef struct {
    uint16_t    result;                 /* Update result */
} __attribute__((packed)) l2cap_conn_param_rsp_t;

/* L2CAP LE Credit Based Connection Request */
typedef struct {
    uint16_t    le_psm;                 /* LE Protocol/Service Multiplexer */
    uint16_t    mtu;                    /* Maximum Transmission Unit */
    uint16_t    mps;                    /* Maximum PDU Size */
    uint16_t    credits;                /* Initial credits */
} __attribute__((packed)) l2cap_le_conn_req_t;

/* L2CAP LE Credit Based Connection Response */
typedef struct {
    uint16_t    dcid;                   /* Destination channel ID */
    uint16_t    mtu;                    /* Maximum Transmission Unit */
    uint16_t    mps;                    /* Maximum PDU Size */
    uint16_t    credits;                /* Initial credits */
    uint16_t    result;                 /* Connection result */
} __attribute__((packed)) l2cap_le_conn_rsp_t;

/*==============================================================================
 * RFCOMM Constants
 *==============================================================================*/

/* RFCOMM Frame Types */
#define RFCOMM_FRAME_SABM                0x2F
#define RFCOMM_FRAME_UA                  0x63
#define RFCOMM_FRAME_DM                  0x0F
#define RFCOMM_FRAME_DISC                0x43
#define RFCOMM_FRAME_UIH                 0xEF
#define RFCOMM_FRAME_UI                  0x03

/* RFCOMM Control Commands */
#define RFCOMM_CMD_PN                    0x80    /* Parameter Negotiation */
#define RFCOMM_CMD_PN_RSP                0x81
#define RFCOMM_CMD_MSC                   0xE0    /* Modem Status Command */
#define RFCOMM_CMD_MSC_RSP               0xE1
#define RFCOMM_CMD_RPN                   0x90    /* Remote Port Negotiation */
#define RFCOMM_CMD_RPN_RSP               0x91
#define RFCOMM_CMD_RLS                   0xA0    /* Remote Line Status */
#define RFCOMM_CMD_RLS_RSP               0xA1
#define RFCOMM_CMD_TEST                  0x20    /* Test Command */
#define RFCOMM_CMD_TEST_RSP              0x21
#define RFCOMM_CMD_FCON                  0xA0    /* Flow Control On */
#define RFCOMM_CMD_FCOFF                 0x60    /* Flow Control Off */

/* RFCOMM DLCI (Data Link Connection Identifier) */
#define RFCOMM_DLCI(dlci)                 ((dlci) >> 2)
#define RFCOMM_CHANNEL(dlci)               ((dlci) >> 3)
#define RFCOMM_DIRECTION(dlci)             (((dlci) >> 1) & 0x01)

/* RFCOMM Maximum MTU */
#define RFCOMM_MAX_MTU                   65535
#define RFCOMM_DEFAULT_MTU               127

/* RFCOMM Port States */
#define RFCOMM_PORT_CLOSED               0
#define RFCOMM_PORT_OPENING              1
#define RFCOMM_PORT_OPEN                 2
#define RFCOMM_PORT_CLOSING               3

/*==============================================================================
 * RFCOMM Structures
 *==============================================================================*/

/* RFCOMM Parameter Negotiation Command */
typedef struct {
    uint8_t     dlci;                   /* DLCI */
    uint8_t     i_bits;                 /* I bits */
    uint8_t     cl;                     /* Convergence layer */
    uint8_t     priority;               /* Priority */
    uint16_t    mtu;                    /* Maximum frame size */
    uint8_t     n1;                     /* Maximum number of retransmissions */
    uint8_t     n2;                     /* Maximum number of acknowledgements */
    uint8_t     k;                      /* Window size */
} __attribute__((packed)) rfcomm_pn_cmd_t;

/* RFCOMM Modem Status Command */
typedef struct {
    uint8_t     dlci;                   /* DLCI */
    uint8_t     signals;                /* Modem signals */
    uint8_t     break_signal;           /* Break signal */
} __attribute__((packed)) rfcomm_msc_cmd_t;

/* Modem signal bits */
#define RFCOMM_SIGNAL_DTR                (1 << 0)
#define RFCOMM_SIGNAL_RTS                (1 << 1)
#define RFCOMM_SIGNAL_CTS                (1 << 2)
#define RFCOMM_SIGNAL_DSR                (1 << 3)
#define RFCOMM_SIGNAL_DCD                (1 << 4)
#define RFCOMM_SIGNAL_RI                 (1 << 5)

/*==============================================================================
 * SDP (Service Discovery Protocol) Constants
 *==============================================================================*/

/* SDP PDU IDs */
#define SDP_PDU_ERROR_RESPONSE           0x01
#define SDP_PDU_SERVICE_SEARCH_REQUEST   0x02
#define SDP_PDU_SERVICE_SEARCH_RESPONSE  0x03
#define SDP_PDU_SERVICE_ATTRIBUTE_REQUEST 0x04
#define SDP_PDU_SERVICE_ATTRIBUTE_RESPONSE 0x05
#define SDP_PDU_SERVICE_SEARCH_ATTRIBUTE_REQUEST 0x06
#define SDP_PDU_SERVICE_SEARCH_ATTRIBUTE_RESPONSE 0x07

/* SDP Attribute IDs */
#define SDP_ATTR_SERVICE_RECORD_HANDLE   0x0000
#define SDP_ATTR_SERVICE_CLASS_ID_LIST   0x0001
#define SDP_ATTR_SERVICE_RECORD_STATE    0x0002
#define SDP_ATTR_SERVICE_ID              0x0003
#define SDP_ATTR_PROTOCOL_DESC_LIST      0x0004
#define SDP_ATTR_BROWSE_GROUP_LIST       0x0005
#define SDP_ATTR_LANGUAGE_BASE_ATTR_ID   0x0006
#define SDP_ATTR_SERVICE_INFO_TIME_TO_LIVE 0x0007
#define SDP_ATTR_SERVICE_AVAILABILITY    0x0008
#define SDP_ATTR_BLUETOOTH_PROFILE_DESC_LIST 0x0009
#define SDP_ATTR_DOCUMENTATION_URL       0x000A
#define SDP_ATTR_CLIENT_EXECUTABLE_URL   0x000B
#define SDP_ATTR_ICON_URL                0x000C
#define SDP_ATTR_ADDITIONAL_PROTOCOL_DESC_LISTS 0x000D

/* SDP Universal Attribute Definitions */
#define SDP_ATTR_ID_PRIMARY_LANGUAGE_BASE 0x0100
#define SDP_ATTR_ID_SERVICE_NAME          (SDP_ATTR_ID_PRIMARY_LANGUAGE_BASE + 0x0000)
#define SDP_ATTR_ID_SERVICE_DESCRIPTION   (SDP_ATTR_ID_PRIMARY_LANGUAGE_BASE + 0x0001)
#define SDP_ATTR_ID_PROVIDER_NAME         (SDP_ATTR_ID_PRIMARY_LANGUAGE_BASE + 0x0002)

/* SDP Data Element Type Descriptors */
#define SDP_DATA_TYPE_NIL                0x00
#define SDP_DATA_TYPE_UINT8               0x08
#define SDP_DATA_TYPE_UINT16              0x09
#define SDP_DATA_TYPE_UINT32              0x0A
#define SDP_DATA_TYPE_UINT64              0x0B
#define SDP_DATA_TYPE_UINT128             0x0C
#define SDP_DATA_TYPE_INT8                0x10
#define SDP_DATA_TYPE_INT16               0x11
#define SDP_DATA_TYPE_INT32               0x12
#define SDP_DATA_TYPE_INT64               0x13
#define SDP_DATA_TYPE_INT128              0x14
#define SDP_DATA_TYPE_UUID16              0x19
#define SDP_DATA_TYPE_UUID32              0x1A
#define SDP_DATA_TYPE_UUID128             0x1C
#define SDP_DATA_TYPE_STRING              0x20
#define SDP_DATA_TYPE_BOOLEAN             0x28
#define SDP_DATA_TYPE_SEQUENCE            0x30
#define SDP_DATA_TYPE_ALTERNATIVE         0x38
#define SDP_DATA_TYPE_URL                 0x40

/*==============================================================================
 * GATT (Generic Attribute Profile) Constants
 *==============================================================================*/

/* GATT Attribute Types (16-bit UUIDs) */
#define GATT_PRIMARY_SERVICE              0x2800
#define GATT_SECONDARY_SERVICE            0x2801
#define GATT_INCLUDE_SERVICE              0x2802
#define GATT_CHARACTERISTIC               0x2803

/* GATT Characteristic Properties */
#define GATT_CHAR_PROP_BROADCAST          (1 << 0)
#define GATT_CHAR_PROP_READ               (1 << 1)
#define GATT_CHAR_PROP_WRITE_NO_RESPONSE  (1 << 2)
#define GATT_CHAR_PROP_WRITE              (1 << 3)
#define GATT_CHAR_PROP_NOTIFY              (1 << 4)
#define GATT_CHAR_PROP_INDICATE            (1 << 5)
#define GATT_CHAR_PROP_AUTHENTICATED_SIGNED_WRITES (1 << 6)
#define GATT_CHAR_PROP_EXTENDED_PROPERTIES (1 << 7)

/* GATT Client Requests */
#define GATT_REQ_ERROR_RESPONSE           0x01
#define GATT_REQ_EXCHANGE_MTU             0x02
#define GATT_REQ_FIND_INFORMATION         0x04
#define GATT_REQ_FIND_BY_TYPE_VALUE       0x06
#define GATT_REQ_READ_BY_TYPE              0x08
#define GATT_REQ_READ                     0x0A
#define GATT_REQ_READ_BLOB                 0x0C
#define GATT_REQ_READ_MULTIPLE             0x0E
#define GATT_REQ_READ_BY_GROUP_TYPE        0x10
#define GATT_REQ_WRITE                     0x12
#define GATT_REQ_WRITE_COMMAND             0x52
#define GATT_REQ_SIGNED_WRITE_COMMAND      0xD2
#define GATT_REQ_PREPARE_WRITE             0x16
#define GATT_REQ_EXECUTE_WRITE             0x18
#define GATT_REQ_HANDLE_VALUE_NOTIFICATION 0x1B
#define GATT_REQ_HANDLE_VALUE_INDICATION   0x1D
#define GATT_REQ_HANDLE_VALUE_CONFIRMATION 0x1E

/* GATT Error Codes */
#define GATT_ERR_INVALID_HANDLE            0x01
#define GATT_ERR_READ_NOT_PERMITTED        0x02
#define GATT_ERR_WRITE_NOT_PERMITTED       0x03
#define GATT_ERR_INVALID_PDU               0x04
#define GATT_ERR_INSUFFICIENT_AUTHENTICATION 0x05
#define GATT_ERR_REQUEST_NOT_SUPPORTED     0x06
#define GATT_ERR_INVALID_OFFSET            0x07
#define GATT_ERR_INSUFFICIENT_AUTHORIZATION 0x08
#define GATT_ERR_PREPARE_QUEUE_FULL        0x09
#define GATT_ERR_ATTRIBUTE_NOT_FOUND       0x0A
#define GATT_ERR_ATTRIBUTE_NOT_LONG        0x0B
#define GATT_ERR_INSUFFICIENT_ENCRYPTION_KEY_SIZE 0x0C
#define GATT_ERR_INVALID_ATTRIBUTE_VALUE_LENGTH 0x0D
#define GATT_ERR_UNLIKELY_ERROR            0x0E
#define GATT_ERR_INSUFFICIENT_ENCRYPTION   0x0F
#define GATT_ERR_UNSUPPORTED_GROUP_TYPE    0x10
#define GATT_ERR_INSUFFICIENT_RESOURCES    0x11
#define GATT_ERR_DATABASE_OUT_OF_SYNC      0x12
#define GATT_ERR_VALUE_NOT_ALLOWED          0x13

/*==============================================================================
 * GATT Structures
 *==============================================================================*/

/* GATT Attribute Handle */
typedef uint16_t gatt_handle_t;

/* GATT UUID (16/32/128-bit) */
typedef struct {
    uint8_t     type;                   /* UUID type (16/32/128) */
    union {
        uint16_t    uuid16;
        uint32_t    uuid32;
        uint8_t     uuid128[16];
    } u;
} gatt_uuid_t;

/* GATT Attribute */
typedef struct {
    gatt_handle_t   handle;             /* Attribute handle */
    gatt_uuid_t     type;                /* Attribute type UUID */
    uint8_t         permissions;          /* Attribute permissions */
    uint16_t        value_len;            /* Current value length */
    uint8_t*        value;                 /* Attribute value */
} gatt_attribute_t;

/* GATT Service */
typedef struct {
    gatt_handle_t   start_handle;        /* First handle of service */
    gatt_handle_t   end_handle;          /* Last handle of service */
    gatt_uuid_t     uuid;                 /* Service UUID */
} gatt_service_t;

/* GATT Characteristic */
typedef struct {
    gatt_handle_t   handle;               /* Characteristic declaration handle */
    gatt_handle_t   value_handle;         /* Characteristic value handle */
    uint8_t         properties;           /* Characteristic properties */
    gatt_uuid_t     uuid;                  /* Characteristic UUID */
} gatt_characteristic_t;

/* GATT Descriptor */
typedef struct {
    gatt_handle_t   handle;               /* Descriptor handle */
    gatt_uuid_t     uuid;                  /* Descriptor UUID */
} gatt_descriptor_t;

/*==============================================================================
 * SMP (Security Manager Protocol) Constants
 *==============================================================================*/

/* SMP Command Codes */
#define SMP_CMD_PAIRING_REQUEST          0x01
#define SMP_CMD_PAIRING_RESPONSE         0x02
#define SMP_CMD_PAIRING_CONFIRM          0x03
#define SMP_CMD_PAIRING_RANDOM           0x04
#define SMP_CMD_PAIRING_FAILED           0x05
#define SMP_CMD_ENCRYPTION_INFORMATION   0x06
#define SMP_CMD_MASTER_IDENTIFICATION    0x07
#define SMP_CMD_IDENTITY_INFORMATION     0x08
#define SMP_CMD_IDENTITY_ADDRESS_INFORMATION 0x09
#define SMP_CMD_SIGNING_INFORMATION      0x0A
#define SMP_CMD_SECURITY_REQUEST          0x0B
#define SMP_CMD_PAIRING_PUBLIC_KEY       0x0C
#define SMP_CMD_PAIRING_DHKEY_CHECK      0x0D
#define SMP_CMD_PAIRING_KEYPRESS_NOTIFICATION 0x0E

/* SMP IO Capabilities */
#define SMP_IO_DISPLAY_ONLY              0x00
#define SMP_IO_DISPLAY_YES_NO            0x01
#define SMP_IO_KEYBOARD_ONLY              0x02
#define SMP_IO_NO_INPUT_NO_OUTPUT        0x03
#define SMP_IO_KEYBOARD_DISPLAY           0x04

/* SMP OOB Data Present */
#define SMP_OOB_NOT_PRESENT               0x00
#define SMP_OOB_PRESENT                   0x01

/* SMP Authentication Requirements */
#define SMP_AUTH_BONDING                  (1 << 0)
#define SMP_AUTH_MITM                     (1 << 1)
#define SMP_AUTH_SC                       (1 << 2)
#define SMP_AUTH_KEYPRESS                  (1 << 3)
#define SMP_AUTH_CT2                      (1 << 4)

/* SMP Key Distribution */
#define SMP_KEY_DIST_ENC                  (1 << 0)
#define SMP_KEY_DIST_ID                   (1 << 1)
#define SMP_KEY_DIST_SIGN                 (1 << 2)
#define SMP_KEY_DIST_LINK                 (1 << 3)

/* SMP Pairing Failed Reasons */
#define SMP_ERR_PASSKEY_ENTRY_FAILED      0x01
#define SMP_ERR_OOB_NOT_AVAILABLE         0x02
#define SMP_ERR_AUTHENTICATION_REQUIREMENTS 0x03
#define SMP_ERR_CONFIRM_VALUE_FAILED      0x04
#define SMP_ERR_PAIRING_NOT_SUPPORTED     0x05
#define SMP_ERR_ENCRYPTION_KEY_SIZE       0x06
#define SMP_ERR_COMMAND_NOT_SUPPORTED     0x07
#define SMP_ERR_UNSPECIFIED_REASON        0x08
#define SMP_ERR_REPEATED_ATTEMPTS         0x09
#define SMP_ERR_INVALID_PARAMETERS        0x0A
#define SMP_ERR_DHKEY_CHECK_FAILED        0x0B
#define SMP_ERR_NUMERIC_COMPARISON_FAILED 0x0C
#define SMP_ERR_BREDR_PAIRING_IN_PROGRESS 0x0D
#define SMP_ERR_CROSS_TRANSPORT_KEY_DERIVATION_NOT_ALLOWED 0x0E

/* SMP Keypress Notification Types */
#define SMP_KEYPRESS_PASSKEY_STARTED      0x00
#define SMP_KEYPRESS_PASSKEY_DIGIT_ENTERED 0x01
#define SMP_KEYPRESS_PASSKEY_DIGIT_ERASED 0x02
#define SMP_KEYPRESS_PASSKEY_CLEARED      0x03
#define SMP_KEYPRESS_PASSKEY_COMPLETED    0x04

/*==============================================================================
 * SMP Structures
 *==============================================================================*/

/* SMP Pairing Request/Response */
typedef struct {
    uint8_t     io_capability;           /* IO Capability */
    uint8_t     oob_data_flag;            /* OOB Data Present */
    uint8_t     auth_req;                 /* Authentication Requirements */
    uint8_t     max_encryption_key_size;  /* Maximum Encryption Key Size (7-16) */
    uint8_t     initiator_key_dist;       /* Initiator Key Distribution */
    uint8_t     responder_key_dist;       /* Responder Key Distribution */
} __attribute__((packed)) smp_pairing_t;

/* SMP Pairing Confirm */
typedef struct {
    uint8_t     confirm_value[16];        /* Confirm Value */
} __attribute__((packed)) smp_pairing_confirm_t;

/* SMP Pairing Random */
typedef struct {
    uint8_t     random_value[16];         /* Random Value */
} __attribute__((packed)) smp_pairing_random_t;

/* SMP Pairing Failed */
typedef struct {
    uint8_t     reason;                   /* Failure Reason */
} __attribute__((packed)) smp_pairing_failed_t;

/* SMP Encryption Information */
typedef struct {
    uint8_t     ltk[16];                  /* Long Term Key */
} __attribute__((packed)) smp_encryption_info_t;

/* SMP Master Identification */
typedef struct {
    uint16_t    ediv;                     /* Encrypted Diversifier */
    uint64_t    rand;                     /* Random Number */
} __attribute__((packed)) smp_master_ident_t;

/* SMP Identity Information */
typedef struct {
    uint8_t     irk[16];                  /* Identity Resolving Key */
} __attribute__((packed)) smp_identity_info_t;

/* SMP Identity Address Information */
typedef struct {
    uint8_t     addr_type;                 /* Address Type (0=Public, 1=Random) */
    bd_addr_t   bd_addr;                   /* Bluetooth Device Address */
} __attribute__((packed)) smp_identity_addr_info_t;

/* SMP Signing Information */
typedef struct {
    uint8_t     csrk[16];                  /* Connection Signature Resolving Key */
} __attribute__((packed)) smp_signing_info_t;

/* SMP Security Request */
typedef struct {
    uint8_t     auth_req;                  /* Authentication Requirements */
} __attribute__((packed)) smp_security_request_t;

/* SMP Pairing Public Key */
typedef struct {
    uint8_t     public_key_x[32];          /* Public Key X coordinate */
    uint8_t     public_key_y[32];          /* Public Key Y coordinate */
} __attribute__((packed)) smp_public_key_t;

/* SMP Pairing DHKey Check */
typedef struct {
    uint8_t     dhkey_check[16];           /* DHKey Check Value */
} __attribute__((packed)) smp_dhkey_check_t;

/* SMP Keypress Notification */
typedef struct {
    uint8_t     notification_type;         /* Keypress Notification Type */
} __attribute__((packed)) smp_keypress_notification_t;

/*==============================================================================
 * Bluetooth Device Structure (Forward Declaration)
 *==============================================================================*/

struct bt_device;
struct bt_controller;
struct l2cap_channel;
struct rfcomm_dlc;
struct gatt_service;
struct gatt_characteristic;
struct gatt_descriptor;

/*==============================================================================
 * Callback Types
 *==============================================================================*/

typedef void (*bt_device_callback_t)(struct bt_device* dev, void* context);
typedef void (*bt_data_callback_t)(struct bt_device* dev, void* data, uint16_t len, void* context);
typedef void (*bt_event_callback_t)(struct bt_device* dev, uint8_t event, void* data, uint8_t len, void* context);
typedef void (*bt_connection_callback_t)(struct bt_device* dev, uint8_t status, void* context);

/*==============================================================================
 * Bluetooth Controller API
 *==============================================================================*/

/* Controller initialization */
int bt_controller_initialize(void);
void bt_controller_terminate(void);

struct bt_controller* bt_controller_create(void);
void bt_controller_destroy(struct bt_controller* ctrl);
int bt_controller_start(struct bt_controller* ctrl);
int bt_controller_stop(struct bt_controller* ctrl);
int bt_controller_reset(struct bt_controller* ctrl);

/* Controller configuration */
int bt_controller_set_scan_mode(struct bt_controller* ctrl, uint32_t mode);
int bt_controller_set_connectable(struct bt_controller* ctrl, uint32_t enable);
int bt_controller_set_discoverable(struct bt_controller* ctrl, uint32_t enable);
int bt_controller_set_pairable(struct bt_controller* ctrl, uint32_t enable);
int bt_controller_set_local_name(struct bt_controller* ctrl, const char* name);
int bt_controller_set_device_class(struct bt_controller* ctrl, dev_class_t dev_class);

/* Controller information */
const bd_addr_t* bt_controller_get_address(struct bt_controller* ctrl);
const char* bt_controller_get_name(struct bt_controller* ctrl);
uint8_t bt_controller_get_version(struct bt_controller* ctrl);
uint16_t bt_controller_get_manufacturer(struct bt_controller* ctrl);
const uint8_t* bt_controller_get_features(struct bt_controller* ctrl);
const uint8_t* bt_controller_get_le_features(struct bt_controller* ctrl);
uint32_t bt_controller_get_state(struct bt_controller* ctrl);

/* Device management */
struct bt_device* bt_device_create(struct bt_controller* ctrl, const bd_addr_t* addr);
void bt_device_free(struct bt_controller* ctrl, struct bt_device* dev);
struct bt_device* bt_device_find_by_addr(struct bt_controller* ctrl, const bd_addr_t* addr);
struct bt_device* bt_device_find_by_handle(struct bt_controller* ctrl, uint16_t handle);

/*==============================================================================
 * HCI Command API
 *==============================================================================*/

int bt_hci_send_command(struct bt_controller* ctrl, uint16_t opcode, void* params, uint8_t plen);
int bt_hci_send_acl_data(struct bt_controller* ctrl, uint16_t handle, void* data, uint16_t len);
int bt_hci_send_sco_data(struct bt_controller* ctrl, uint16_t handle, void* data, uint16_t len);

/* HCI commands */
int bt_hci_inquiry(struct bt_controller* ctrl, uint8_t lap[3], uint8_t length, uint8_t num_responses);
int bt_hci_inquiry_cancel(struct bt_controller* ctrl);
int bt_hci_create_connection(struct bt_controller* ctrl, const bd_addr_t* addr, uint16_t packet_type,
                              uint8_t page_scan_rep_mode, uint8_t page_scan_mode,
                              uint16_t clock_offset, uint8_t allow_role_switch);
int bt_hci_disconnect(struct bt_controller* ctrl, uint16_t handle, uint8_t reason);
int bt_hci_accept_connection_request(struct bt_controller* ctrl, const bd_addr_t* addr, uint8_t role);
int bt_hci_reject_connection_request(struct bt_controller* ctrl, const bd_addr_t* addr, uint8_t reason);
int bt_hci_authentication_requested(struct bt_controller* ctrl, uint16_t handle);
int bt_hci_set_connection_encryption(struct bt_controller* ctrl, uint16_t handle, uint8_t enable);
int bt_hci_change_connection_packet_type(struct bt_controller* ctrl, uint16_t handle, uint16_t packet_type);
int bt_hci_remote_name_request(struct bt_controller* ctrl, const bd_addr_t* addr,
                                uint8_t page_scan_rep_mode, uint8_t page_scan_mode,
                                uint16_t clock_offset);
int bt_hci_read_remote_features(struct bt_controller* ctrl, uint16_t handle);
int bt_hci_read_remote_version(struct bt_controller* ctrl, uint16_t handle);
int bt_hci_read_clock_offset(struct bt_controller* ctrl, uint16_t handle);
int bt_hci_hold_mode(struct bt_controller* ctrl, uint16_t handle, uint16_t max_interval, uint16_t min_interval);
int bt_hci_sniff_mode(struct bt_controller* ctrl, uint16_t handle, uint16_t max_interval,
                       uint16_t min_interval, uint16_t attempt, uint16_t timeout);
int bt_hci_exit_sniff_mode(struct bt_controller* ctrl, uint16_t handle);
int bt_hci_park_state(struct bt_controller* ctrl, uint16_t handle, uint16_t beacon_max_interval,
                       uint16_t beacon_min_interval);
int bt_hci_exit_park_state(struct bt_controller* ctrl, uint16_t handle);
int bt_hci_qos_setup(struct bt_controller* ctrl, uint16_t handle, uint8_t flags,
                      uint8_t service_type, uint32_t token_rate, uint32_t peak_bandwidth,
                      uint32_t latency, uint32_t delay_variation);
int bt_hci_role_discovery(struct bt_controller* ctrl, uint16_t handle);
int bt_hci_switch_role(struct bt_controller* ctrl, const bd_addr_t* addr, uint8_t role);
int bt_hci_set_event_mask(struct bt_controller* ctrl, uint8_t mask[8]);
int bt_hci_reset(struct bt_controller* ctrl);
int bt_hci_set_event_filter(struct bt_controller* ctrl, uint8_t filter_type,
                             uint8_t filter_condition_type, void* condition);
int bt_hci_flush(struct bt_controller* ctrl, uint16_t handle);
int bt_hci_write_pin_type(struct bt_controller* ctrl, uint8_t pin_type);
int bt_hci_write_stored_link_key(struct bt_controller* ctrl, bd_addr_t* addr, uint8_t* key);
int bt_hci_delete_stored_link_key(struct bt_controller* ctrl, bd_addr_t* addr, uint8_t delete_all);
int bt_hci_write_local_name(struct bt_controller* ctrl, const char* name);
int bt_hci_read_local_name(struct bt_controller* ctrl, char* name, uint8_t* len);
int bt_hci_write_scan_enable(struct bt_controller* ctrl, uint8_t scan_enable);
int bt_hci_write_class_of_device(struct bt_controller* ctrl, dev_class_t dev_class);
int bt_hci_write_voice_setting(struct bt_controller* ctrl, uint16_t voice_setting);
int bt_hci_read_buffer_size(struct bt_controller* ctrl);
int bt_hci_read_bd_addr(struct bt_controller* ctrl);
int bt_hci_read_local_features(struct bt_controller* ctrl);

/*==============================================================================
 * LE HCI Commands
 *==============================================================================*/

int bt_hci_le_set_event_mask(struct bt_controller* ctrl, uint8_t mask[8]);
int bt_hci_le_read_buffer_size(struct bt_controller* ctrl);
int bt_hci_le_read_local_features(struct bt_controller* ctrl);
int bt_hci_le_set_random_address(struct bt_controller* ctrl, bd_addr_t* addr);
int bt_hci_le_set_advertising_parameters(struct bt_controller* ctrl, uint16_t min_interval,
                                          uint16_t max_interval, uint8_t advertising_type,
                                          uint8_t own_address_type, uint8_t peer_address_type,
                                          bd_addr_t* peer_address, uint8_t channel_map,
                                          uint8_t filter_policy);
int bt_hci_le_set_advertising_data(struct bt_controller* ctrl, uint8_t length, uint8_t* data);
int bt_hci_le_set_scan_response_data(struct bt_controller* ctrl, uint8_t length, uint8_t* data);
int bt_hci_le_set_advertise_enable(struct bt_controller* ctrl, uint8_t enable);
int bt_hci_le_set_scan_parameters(struct bt_controller* ctrl, uint8_t scan_type,
                                    uint16_t interval, uint16_t window,
                                    uint8_t own_address_type, uint8_t filter_policy);
int bt_hci_le_set_scan_enable(struct bt_controller* ctrl, uint8_t enable, uint8_t filter_duplicates);
int bt_hci_le_create_connection(struct bt_controller* ctrl, uint16_t scan_interval,
                                  uint16_t scan_window, uint8_t initiator_filter_policy,
                                  uint8_t peer_address_type, bd_addr_t* peer_address,
                                  uint8_t own_address_type, uint16_t conn_interval_min,
                                  uint16_t conn_interval_max, uint16_t conn_latency,
                                  uint16_t supervision_timeout, uint16_t min_ce_length,
                                  uint16_t max_ce_length);
int bt_hci_le_create_connection_cancel(struct bt_controller* ctrl);
int bt_hci_le_connection_update(struct bt_controller* ctrl, uint16_t handle,
                                 uint16_t conn_interval_min, uint16_t conn_interval_max,
                                 uint16_t conn_latency, uint16_t supervision_timeout,
                                 uint16_t min_ce_length, uint16_t max_ce_length);
int bt_hci_le_read_remote_features(struct bt_controller* ctrl, uint16_t handle);
int bt_hci_le_start_encryption(struct bt_controller* ctrl, uint16_t handle,
                                uint64_t rand, uint16_t ediv, uint8_t* ltk);
int bt_hci_le_ltk_request_reply(struct bt_controller* ctrl, uint16_t handle, uint8_t* ltk);
int bt_hci_le_ltk_request_neg_reply(struct bt_controller* ctrl, uint16_t handle);
int bt_hci_le_rand(struct bt_controller* ctrl, uint8_t* random);
int bt_hci_le_encrypt(struct bt_controller* ctrl, uint8_t* key, uint8_t* plaintext, uint8_t* ciphertext);
int bt_hci_le_read_supported_states(struct bt_controller* ctrl);
int bt_hci_le_receiver_test(struct bt_controller* ctrl, uint8_t rx_channel);
int bt_hci_le_transmitter_test(struct bt_controller* ctrl, uint8_t tx_channel, uint8_t length, uint8_t payload);
int bt_hci_le_test_end(struct bt_controller* ctrl);
int bt_hci_le_set_data_length(struct bt_controller* ctrl, uint16_t handle, uint16_t tx_octets, uint16_t tx_time);
int bt_hci_le_read_default_data_length(struct bt_controller* ctrl);
int bt_hci_le_write_default_data_length(struct bt_controller* ctrl, uint16_t tx_octets, uint16_t tx_time);
int bt_hci_le_read_local_p256_public_key(struct bt_controller* ctrl);
int bt_hci_le_generate_dhkey(struct bt_controller* ctrl, uint8_t* remote_pkey);

/*==============================================================================
 * L2CAP API
 *==============================================================================*/

struct l2cap_channel* l2cap_channel_create(struct bt_device* dev, uint16_t psm);
void l2cap_channel_free(struct l2cap_channel* chan);
int l2cap_channel_connect(struct l2cap_channel* chan);
int l2cap_channel_disconnect(struct l2cap_channel* chan);
int l2cap_channel_send(struct l2cap_channel* chan, void* data, uint16_t len);
int l2cap_channel_set_rx_callback(struct l2cap_channel* chan, 
                                   int (*callback)(struct l2cap_channel*, void*, uint16_t));

/* L2CAP signaling */
int l2cap_send_connection_request(struct bt_device* dev, uint16_t psm, uint16_t scid);
int l2cap_send_connection_response(struct bt_device* dev, uint8_t ident, 
                                     uint16_t dcid, uint16_t scid, uint16_t result);
int l2cap_send_configuration_request(struct l2cap_channel* chan, uint16_t flags, void* options, uint16_t opt_len);
int l2cap_send_configuration_response(struct l2cap_channel* chan, uint8_t ident,
                                        uint16_t flags, uint16_t result, void* options, uint16_t opt_len);
int l2cap_send_disconnection_request(struct l2cap_channel* chan);
int l2cap_send_disconnection_response(struct l2cap_channel* chan, uint8_t ident);
int l2cap_send_echo_request(struct bt_device* dev, void* data, uint16_t len);
int l2cap_send_echo_response(struct bt_device* dev, uint8_t ident, void* data, uint16_t len);
int l2cap_send_information_request(struct bt_device* dev, uint16_t info_type);
int l2cap_send_information_response(struct bt_device* dev, uint8_t ident, uint16_t info_type,
                                      uint16_t result, void* data, uint16_t len);

/* LE L2CAP */
int l2cap_le_send_connection_parameter_update_request(struct bt_device* dev,
                                                        uint16_t min_interval, uint16_t max_interval,
                                                        uint16_t latency, uint16_t timeout);
int l2cap_le_send_connection_parameter_update_response(struct bt_device* dev, uint8_t ident, uint16_t result);
int l2cap_le_credit_based_connection_request(struct bt_device* dev, uint16_t psm,
                                               uint16_t mtu, uint16_t mps, uint16_t credits);
int l2cap_le_credit_based_connection_response(struct l2cap_channel* chan, uint8_t ident,
                                                uint16_t mtu, uint16_t mps, uint16_t credits, uint16_t result);
int l2cap_le_send_flow_control_credit(struct l2cap_channel* chan, uint16_t credits);

/*==============================================================================
 * RFCOMM API
 *==============================================================================*/

struct rfcomm_dlc* rfcomm_dlc_create(struct l2cap_channel* l2cap, uint8_t dlci);
void rfcomm_dlc_free(struct rfcomm_dlc* dlc);
int rfcomm_dlc_open(struct rfcomm_dlc* dlc);
int rfcomm_dlc_close(struct rfcomm_dlc* dlc);
int rfcomm_dlc_send(struct rfcomm_dlc* dlc, void* data, uint16_t len);
int rfcomm_dlc_set_rx_callback(struct rfcomm_dlc* dlc,
                                int (*callback)(struct rfcomm_dlc*, void*, uint16_t));

/* RFCOMM control */
int rfcomm_send_parameter_negotiation(struct rfcomm_dlc* dlc, uint16_t mtu);
int rfcomm_send_modem_status(struct rfcomm_dlc* dlc, uint8_t signals);
int rfcomm_send_remote_port_negotiation(struct rfcomm_dlc* dlc, void* params, uint8_t len);
int rfcomm_send_remote_line_status(struct rfcomm_dlc* dlc, uint8_t status);
int rfcomm_send_test(struct rfcomm_dlc* dlc, void* data, uint16_t len);
int rfcomm_send_flow_control_on(struct rfcomm_dlc* dlc);
int rfcomm_send_flow_control_off(struct rfcomm_dlc* dlc);

/*==============================================================================
 * SDP API
 *==============================================================================*/

int sdp_register_service(struct bt_device* dev, void* record, uint16_t len, uint32_t* handle);
int sdp_unregister_service(struct bt_device* dev, uint32_t handle);
int sdp_service_search(struct bt_device* dev, uint8_t* uuid, uint16_t uuid_len, void* response, uint16_t* resp_len);
int sdp_service_attribute_search(struct bt_device* dev, uint32_t handle, uint16_t* attr_list,
                                   uint16_t attr_count, void* response, uint16_t* resp_len);
int sdp_service_search_attribute(struct bt_device* dev, uint8_t* uuid, uint16_t uuid_len,
                                   uint16_t* attr_list, uint16_t attr_count,
                                   void* response, uint16_t* resp_len);

/* SDP record builders */
void* sdp_build_service_record(uint32_t handle, uint8_t* uuid, uint16_t uuid_len,
                                 uint8_t** record, uint16_t* record_len);
void* sdp_add_protocol_descriptor_list(void* record, uint16_t* len, uint16_t psm, uint16_t version);
void* sdp_add_language_base_attribute_id(void* record, uint16_t* len);
void* sdp_add_service_name(void* record, uint16_t* len, const char* name);
void* sdp_add_service_description(void* record, uint16_t* len, const char* desc);
void* sdp_add_provider_name(void* record, uint16_t* len, const char* provider);

/*==============================================================================
 * GATT API
 *==============================================================================*/

/* GATT Server */
int gatt_server_register_service(struct bt_controller* ctrl, gatt_service_t* service,
                                   gatt_characteristic_t* chars, uint16_t num_chars,
                                   gatt_descriptor_t* descs, uint16_t num_descs);
int gatt_server_unregister_service(struct bt_controller* ctrl, gatt_handle_t start_handle);
int gatt_server_notify_characteristic(struct bt_device* dev, gatt_handle_t handle, void* value, uint16_t len);
int gatt_server_indicate_characteristic(struct bt_device* dev, gatt_handle_t handle, void* value, uint16_t len);
int gatt_server_send_response(struct bt_device* dev, uint8_t opcode, uint16_t handle, void* data, uint16_t len);

/* GATT Client */
int gatt_client_exchange_mtu(struct bt_device* dev, uint16_t mtu);
int gatt_client_discover_all_services(struct bt_device* dev);
int gatt_client_discover_services_by_uuid(struct bt_device* dev, gatt_uuid_t* uuid);
int gatt_client_discover_included_services(struct bt_device* dev, gatt_handle_t start, gatt_handle_t end);
int gatt_client_discover_characteristics(struct bt_device* dev, gatt_handle_t start, gatt_handle_t end);
int gatt_client_discover_characteristics_by_uuid(struct bt_device* dev, gatt_handle_t start,
                                                   gatt_handle_t end, gatt_uuid_t* uuid);
int gatt_client_discover_descriptors(struct bt_device* dev, gatt_handle_t start, gatt_handle_t end);
int gatt_client_read_characteristic(struct bt_device* dev, gatt_handle_t handle);
int gatt_client_read_long_characteristic(struct bt_device* dev, gatt_handle_t handle, uint16_t offset);
int gatt_client_read_multiple_characteristics(struct bt_device* dev, gatt_handle_t* handles, uint16_t num_handles);
int gatt_client_write_characteristic(struct bt_device* dev, gatt_handle_t handle, void* value, uint16_t len);
int gatt_client_write_long_characteristic(struct bt_device* dev, gatt_handle_t handle,
                                            void* value, uint16_t len, uint8_t reliable);
int gatt_client_write_characteristic_without_response(struct bt_device* dev, gatt_handle_t handle,
                                                        void* value, uint16_t len);
int gatt_client_signed_write_without_response(struct bt_device* dev, gatt_handle_t handle,
                                                void* value, uint16_t len);
int gatt_client_read_descriptor(struct bt_device* dev, gatt_handle_t handle);
int gatt_client_write_descriptor(struct bt_device* dev, gatt_handle_t handle, void* value, uint16_t len);
int gatt_client_set_notification(struct bt_device* dev, gatt_handle_t handle, uint8_t enable);
int gatt_client_set_indication(struct bt_device* dev, gatt_handle_t handle, uint8_t enable);

/*==============================================================================
 * SMP API
 *==============================================================================*/

int smp_pair(struct bt_device* dev, uint8_t io_capability, uint8_t auth_req);
int smp_send_security_request(struct bt_device* dev);
int smp_encrypt_connection(struct bt_device* dev, uint64_t rand, uint16_t ediv, uint8_t* ltk);
int smp_generate_ltk(struct bt_device* dev, uint8_t* ltk);
int smp_generate_irk(struct bt_device* dev, uint8_t* irk);
int smp_generate_csrk(struct bt_device* dev, uint8_t* csrk);
int smp_generate_dhkey(struct bt_device* dev, uint8_t* public_key, uint8_t* private_key, uint8_t* dhkey);
int smp_sign_data(struct bt_device* dev, uint8_t* data, uint16_t len, uint8_t* signature);
int smp_verify_signature(struct bt_device* dev, uint8_t* data, uint16_t len, uint8_t* signature);
int smp_resolve_private_address(bd_addr_t* address, uint8_t* irk, bd_addr_t* resolved);

/*==============================================================================
 * Device API
 *==============================================================================*/

/* Device information */
const bd_addr_t* bt_device_get_address(struct bt_device* dev);
const char* bt_device_get_name(struct bt_device* dev);
uint32_t bt_device_get_class(struct bt_device* dev);
uint16_t bt_device_get_handle(struct bt_device* dev);
uint8_t bt_device_get_role(struct bt_device* dev);
uint8_t bt_device_get_mode(struct bt_device* dev);
uint32_t bt_device_get_flags(struct bt_device* dev);
const uint8_t* bt_device_get_features(struct bt_device* dev);

/* Device connection */
int bt_device_connect(struct bt_device* dev);
int bt_device_disconnect(struct bt_device* dev, uint8_t reason);
int bt_device_authenticate(struct bt_device* dev);
int bt_device_encrypt(struct bt_device* dev, uint8_t enable);
int bt_device_pair(struct bt_device* dev);
int bt_device_unpair(struct bt_device* dev);

/* Device callbacks */
void bt_device_set_connection_callback(struct bt_device* dev, bt_connection_callback_t callback, void* context);
void bt_device_set_data_callback(struct bt_device* dev, bt_data_callback_t callback, void* context);
void bt_device_set_event_callback(struct bt_device* dev, bt_event_callback_t callback, void* context);

/* Device RSSI and link quality */
int bt_device_read_rssi(struct bt_device* dev, int8_t* rssi);
int bt_device_read_link_quality(struct bt_device* dev, uint8_t* quality);
int bt_device_read_tx_power(struct bt_device* dev, int8_t* power);

/*==============================================================================
 * Bluetooth Security Integration (DSMOS)
 *==============================================================================*/

/* DSMOS integration for secure key derivation */
int bt_security_initialize(void);
void bt_security_terminate(void);

/* Key derivation from DSMOS */
const uint8_t* bt_security_get_link_key(void);
const uint8_t* bt_security_get_ltk(void);
const uint8_t* bt_security_get_irk(void);
const uint8_t* bt_security_get_csrk(void);
const uint8_t* bt_security_get_dhkey(void);
int bt_security_derive_session_key(const uint8_t* random, uint8_t random_len,
                                     uint8_t* session_key, uint8_t* key_len);
int bt_security_sign_data(const uint8_t* data, uint32_t data_len,
                           uint8_t* signature, uint32_t* sig_len);
int bt_security_verify_signature(const uint8_t* data, uint32_t data_len,
                                   const uint8_t* signature, uint32_t sig_len);
int bt_security_is_hardware_authorized(void);

/*==============================================================================
 * IOBluetooth Core Initialization
 *==============================================================================*/

/* Global initialization */
int IOBluetooth_initialize(void);
void IOBluetooth_terminate(void);

/* Debug and logging */
void IOBluetooth_set_debug_level(uint32_t level);
uint32_t IOBluetooth_get_debug_level(void);

/* Statistics */
typedef struct {
    uint64_t    tx_packets;
    uint64_t    rx_packets;
    uint64_t    tx_bytes;
    uint64_t    rx_bytes;
    uint64_t    tx_errors;
    uint64_t    rx_errors;
    uint64_t    timeouts;
    uint64_t    disconnections;
    uint64_t    reconnections;
    uint64_t    pairing_attempts;
    uint64_t    pairing_success;
    uint64_t    pairing_failures;
    uint64_t    le_connections;
    uint64_t    bredr_connections;
} bt_statistics_t;

int bt_get_statistics(bt_statistics_t* stats);
void bt_reset_statistics(void);

/*==============================================================================
 * Error Code Translation
 *==============================================================================*/

const char* bt_strerror(int error);
const char* bt_hci_strerror(uint8_t error);
const char* bt_l2cap_strerror(uint16_t error);
const char* bt_smp_strerror(uint8_t error);
const char* bt_gatt_strerror(uint8_t error);

/*==============================================================================
 * Utility Functions
 *==============================================================================*/

/* UUID utilities */
void bt_uuid16_to_uuid128(uint16_t uuid16, uint8_t* uuid128);
int bt_uuid_compare(const uint8_t* uuid1, const uint8_t* uuid2, uint8_t len);
void bt_uuid_to_string(const uint8_t* uuid, uint8_t len, char* str, size_t str_len);

/* Address utilities */
int bt_addr_from_string(const char* str, bd_addr_t* addr);
void bt_addr_to_string(const bd_addr_t* addr, char* str, size_t str_len);
int bt_addr_is_valid(const bd_addr_t* addr);
int bt_addr_is_public(const bd_addr_t* addr);
int bt_addr_is_random(const bd_addr_t* addr);

/* Time utilities */
uint64_t bt_get_uptime(void);
void bt_msleep(uint32_t ms);
void bt_usleep(uint32_t us);

/* Buffer utilities */
void* bt_alloc_buffer(size_t size);
void bt_free_buffer(void* buf, size_t size);

/* Crypto utilities */
void bt_crypto_random_bytes(uint8_t* buf, size_t len);
void bt_crypto_aes_encrypt(const uint8_t* key, const uint8_t* plaintext, uint8_t* ciphertext);
void bt_crypto_aes_decrypt(const uint8_t* key, const uint8_t* ciphertext, uint8_t* plaintext);
void bt_crypto_sha1(const uint8_t* data, size_t len, uint8_t* hash);
void bt_crypto_sha256(const uint8_t* data, size_t len, uint8_t* hash);
void bt_crypto_hmac_sha256(const uint8_t* key, size_t key_len,
                            const uint8_t* data, size_t data_len, uint8_t* hmac);

#ifdef __cplusplus
}
#endif

#endif /* _IOBLUETOOTH_IOBLUETOOTH_H */
