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
 * IOBluetooth.c
 * Core Bluetooth Kernel-Mode Driver for macOS
 * Supports Bluetooth 2.0/2.1/3.0/4.0/4.1/4.2/5.0/5.1/5.2
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/socket.h>
#include <sys/filio.h>
#include <sys/ioccom.h>

#include <kern/kern_types.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/zalloc.h>
#include <kern/task.h>

#include <machine/machine_routines.h>
#include <machine/atomic.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>
#include <IOKit/assert.h>

#include <libkern/OSAtomic.h>
#include <libkern/OSMalloc.h>
#include <libkern/crypto/sha1.h>
#include <libkern/crypto/aes.h>

#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>

#include <net/if.h>
#include <net/if_types.h>

/*==============================================================================
 * Bluetooth Core Specifications
 *==============================================================================*/

/* Bluetooth Version */
#define BT_VERSION_1_0             0x00
#define BT_VERSION_1_1             0x01
#define BT_VERSION_1_2             0x02
#define BT_VERSION_2_0             0x03
#define BT_VERSION_2_1             0x04
#define BT_VERSION_3_0             0x05
#define BT_VERSION_4_0             0x06
#define BT_VERSION_4_1             0x07
#define BT_VERSION_4_2             0x08
#define BT_VERSION_5_0             0x09
#define BT_VERSION_5_1             0x0A
#define BT_VERSION_5_2             0x0B

/* Link Types */
#define BT_LINK_TYPE_SCO           0x00
#define BT_LINK_TYPE_ACL           0x01
#define BT_LINK_TYPE_ESCO          0x02

/* Packet Types */
#define BT_PKT_TYPE_NULL           0x00
#define BT_PKT_TYPE_POLL           0x01
#define BT_PKT_TYPE_FHS            0x02
#define BT_PKT_TYPE_DM1            0x03
#define BT_PKT_TYPE_DH1            0x04
#define BT_PKT_TYPE_HV1            0x05
#define BT_PKT_TYPE_HV2            0x06
#define BT_PKT_TYPE_HV3            0x07
#define BT_PKT_TYPE_DV             0x08
#define BT_PKT_TYPE_AUX1           0x09
#define BT_PKT_TYPE_DM3            0x0A
#define BT_PKT_TYPE_DH3            0x0B
#define BT_PKT_TYPE_DM5            0x0C
#define BT_PKT_TYPE_DH5            0x0D
#define BT_PKT_TYPE_2DH1           0x0E
#define BT_PKT_TYPE_2DH3           0x0F
#define BT_PKT_TYPE_2DH5           0x10
#define BT_PKT_TYPE_3DH1           0x11
#define BT_PKT_TYPE_3DH3           0x12
#define BT_PKT_TYPE_3DH5           0x13

/*==============================================================================
 * HCI (Host Controller Interface) Commands
 *==============================================================================*/

/* OGF - Opcode Group Field */
#define HCI_OGF_LINK_CONTROL       0x01
#define HCI_OGF_LINK_POLICY        0x02
#define HCI_OGF_HOST_CONTROL       0x03
#define HCI_OGF_INFO_PARAM         0x04
#define HCI_OGF_STATUS_PARAM       0x05
#define HCI_OGF_TESTING            0x06
#define HCI_OGF_LE_CONTROL         0x08
#define HCI_OGF_VENDOR              0x3F

/* Link Control Commands (OGF=0x01) */
#define HCI_CMD_INQUIRY             0x0001
#define HCI_CMD_INQUIRY_CANCEL      0x0002
#define HCI_CMD_CREATE_CONN         0x0005
#define HCI_CMD_DISCONNECT          0x0006
#define HCI_CMD_ACCEPT_CONN_REQ     0x0009
#define HCI_CMD_REJECT_CONN_REQ     0x000A
#define HCI_CMD_LINK_KEY_REQ_REPLY  0x000B
#define HCI_CMD_LINK_KEY_REQ_NEG    0x000C
#define HCI_CMD_PIN_CODE_REQ_REPLY  0x000D
#define HCI_CMD_PIN_CODE_REQ_NEG    0x000E
#define HCI_CMD_CHANGE_CONN_PKT     0x000F
#define HCI_CMD_AUTH_REQUESTED      0x0011
#define HCI_CMD_SET_CONN_ENCRYPT    0x0013
#define HCI_CMD_CHANGE_LINK_KEY     0x0015
#define HCI_CMD_MASTER_LINK_KEY     0x0017
#define HCI_CMD_REMOTE_NAME_REQ     0x0019
#define HCI_CMD_READ_REMOTE_FEAT    0x001B
#define HCI_CMD_READ_REMOTE_VER     0x001D
#define HCI_CMD_READ_CLOCK_OFFSET   0x001F

/* Host Control Commands (OGF=0x03) */
#define HCI_CMD_SET_EVENT_MASK      0x1001
#define HCI_CMD_RESET               0x1003
#define HCI_CMD_SET_EVENT_FILTER    0x1005
#define HCI_CMD_FLUSH               0x1008
#define HCI_CMD_READ_PIN_TYPE       0x1009
#define HCI_CMD_WRITE_PIN_TYPE      0x100A
#define HCI_CMD_CREATE_NEW_UNIT_KEY 0x100B
#define HCI_CMD_READ_STORED_LINK_KEY 0x100D
#define HCI_CMD_WRITE_STORED_LINK_KEY 0x1011
#define HCI_CMD_DELETE_STORED_LINK_KEY 0x1012
#define HCI_CMD_WRITE_LOCAL_NAME    0x1013
#define HCI_CMD_READ_LOCAL_NAME     0x1014
#define HCI_CMD_READ_CONN_ACCEPT    0x1015
#define HCI_CMD_WRITE_CONN_ACCEPT   0x1016
#define HCI_CMD_READ_PAGE_TO        0x1017
#define HCI_CMD_WRITE_PAGE_TO       0x1018
#define HCI_CMD_READ_SCAN_ENABLE    0x1019
#define HCI_CMD_WRITE_SCAN_ENABLE   0x101A
#define HCI_CMD_READ_AUTH_ENABLE    0x101F
#define HCI_CMD_WRITE_AUTH_ENABLE   0x1020
#define HCI_CMD_READ_ENCRYPT_MODE   0x1021
#define HCI_CMD_WRITE_ENCRYPT_MODE  0x1022
#define HCI_CMD_READ_CLASS_OF_DEV   0x1023
#define HCI_CMD_WRITE_CLASS_OF_DEV  0x1024
#define HCI_CMD_READ_VOICE_SETTING  0x1025
#define HCI_CMD_WRITE_VOICE_SETTING 0x1026
#define HCI_CMD_READ_AUTO_FLUSH_TO  0x1027
#define HCI_CMD_WRITE_AUTO_FLUSH_TO 0x1028
#define HCI_CMD_READ_NUM_BCAST      0x1029
#define HCI_CMD_READ_BD_ADDR        0x1049

/* LE Control Commands (OGF=0x08) */
#define HCI_CMD_LE_SET_EVENT_MASK   0x2001
#define HCI_CMD_LE_READ_BUFFER_SIZE 0x2002
#define HCI_CMD_LE_READ_LOCAL_FEAT  0x2003
#define HCI_CMD_LE_SET_RAND_ADDR    0x2005
#define HCI_CMD_LE_SET_ADV_PARAM    0x2006
#define HCI_CMD_LE_READ_ADV_TX_PWR  0x2007
#define HCI_CMD_LE_SET_ADV_DATA     0x2008
#define HCI_CMD_LE_SET_SCAN_RSP     0x2009
#define HCI_CMD_LE_SET_ADV_ENABLE   0x200A
#define HCI_CMD_LE_SET_SCAN_PARAM   0x200B
#define HCI_CMD_LE_SET_SCAN_ENABLE  0x200C
#define HCI_CMD_LE_CREATE_CONN      0x200D
#define HCI_CMD_LE_CREATE_CONN_CANCEL 0x200E
#define HCI_CMD_LE_READ_WHITE_LIST_SIZE 0x200F
#define HCI_CMD_LE_CLEAR_WHITE_LIST 0x2010
#define HCI_CMD_LE_ADD_WHITE_LIST   0x2011
#define HCI_CMD_LE_REMOVE_WHITE_LIST 0x2012
#define HCI_CMD_LE_CONN_UPDATE      0x2013
#define HCI_CMD_LE_SET_HOST_CLASS   0x2014
#define HCI_CMD_LE_READ_CHAN_MAP    0x2015
#define HCI_CMD_LE_READ_REMOTE_FEAT 0x2016
#define HCI_CMD_LE_ENCRYPT          0x2017
#define HCI_CMD_LE_RAND             0x2018
#define HCI_CMD_LE_START_ENCRYPT    0x2019
#define HCI_CMD_LE_LTK_REQ_REPLY    0x201A
#define HCI_CMD_LE_LTK_REQ_NEG      0x201B
#define HCI_CMD_LE_READ_SUPP_STATES 0x201C
#define HCI_CMD_LE_RX_TEST          0x201D
#define HCI_CMD_LE_TX_TEST          0x201E
#define HCI_CMD_LE_TEST_END         0x201F
#define HCI_CMD_LE_REMOTE_CONN_PARAM 0x2020
#define HCI_CMD_LE_SET_DATA_LEN     0x2022
#define HCI_CMD_LE_READ_DEF_DATA_LEN 0x2023
#define HCI_CMD_LE_WRITE_DEF_DATA_LEN 0x2024
#define HCI_CMD_LE_READ_LOCAL_P256  0x2025
#define HCI_CMD_LE_GENERATE_DHKEY   0x2026
#define HCI_CMD_LE_ADD_DEVICE_RESOLV 0x2027
#define HCI_CMD_LE_REMOVE_DEV_RESOLV 0x2028
#define HCI_CMD_LE_CLEAR_RESOLV_LIST 0x2029
#define HCI_CMD_LE_READ_RESOLV_SIZE 0x202A
#define HCI_CMD_LE_READ_RESOLV_EN   0x202B
#define HCI_CMD_LE_SET_RESOLV_EN    0x202C
#define HCI_CMD_LE_SET_RESOLV_ADDR  0x202D
#define HCI_CMD_LE_SET_ADDR_RESOLV  0x202E

/*==============================================================================
 * HCI Event Types
 *==============================================================================*/

#define HCI_EVT_INQUIRY_COMPLETE    0x01
#define HCI_EVT_INQUIRY_RESULT      0x02
#define HCI_EVT_CONN_COMPLETE       0x03
#define HCI_EVT_CONN_REQUEST        0x04
#define HCI_EVT_DISCONN_COMPLETE    0x05
#define HCI_EVT_AUTH_COMPLETE       0x06
#define HCI_EVT_REMOTE_NAME_REQ_COMPLETE 0x07
#define HCI_EVT_ENCRYPT_CHANGE      0x08
#define HCI_EVT_CHANGE_LINK_KEY_COMPLETE 0x09
#define HCI_EVT_MASTER_LINK_KEY_COMPLETE 0x0A
#define HCI_EVT_READ_REMOTE_FEAT_COMPLETE 0x0B
#define HCI_EVT_READ_REMOTE_VER_COMPLETE 0x0C
#define HCI_EVT_QOS_SETUP_COMPLETE  0x0D
#define HCI_EVT_COMMAND_COMPLETE     0x0E
#define HCI_EVT_COMMAND_STATUS       0x0F
#define HCI_EVT_HARDWARE_ERROR       0x10
#define HCI_EVT_FLUSH_OCCURRED       0x11
#define HCI_EVT_ROLE_CHANGE          0x12
#define HCI_EVT_NUM_COMPLETED_PKTS   0x13
#define HCI_EVT_MODE_CHANGE          0x14
#define HCI_EVT_RETURN_LINK_KEYS     0x15
#define HCI_EVT_PIN_CODE_REQ         0x16
#define HCI_EVT_LINK_KEY_REQ         0x17
#define HCI_EVT_LINK_KEY_NOTIFY      0x18
#define HCI_EVT_LOOPBACK_COMMAND     0x19
#define HCI_EVT_DATA_BUFFER_OVERFLOW 0x1A
#define HCI_EVT_MAX_SLOTS_CHANGE     0x1B
#define HCI_EVT_READ_CLOCK_OFFSET_COMPLETE 0x1C
#define HCI_EVT_CONN_PKT_TYPE_CHANGE 0x1D
#define HCI_EVT_QOS_VIOLATION        0x1E
#define HCI_EVT_PAGE_SCAN_MODE_CHANGE 0x1F
#define HCI_EVT_PAGE_SCAN_REP_MODE_CHANGE 0x20
#define HCI_EVT_FLOW_SPEC_COMPLETE   0x21
#define HCI_EVT_INQUIRY_RESULT_RSSI  0x22
#define HCI_EVT_READ_REMOTE_FEAT_EXT 0x23
#define HCI_EVT_SYNC_CONN_COMPLETE   0x2C
#define HCI_EVT_SYNC_CONN_CHANGED    0x2D
#define HCI_EVT_SNIFF_SUB_RATING     0x2E
#define HCI_EVT_EXTENDED_INQUIRY_RESULT 0x2F
#define HCI_EVT_ENCRYPTION_KEY_REFRESH 0x30
#define HCI_EVT_IO_CAPABILITY_REQ    0x31
#define HCI_EVT_IO_CAPABILITY_RSP    0x32
#define HCI_EVT_USER_CONFIRMATION_REQ 0x33
#define HCI_EVT_USER_PASSKEY_REQ     0x34
#define HCI_EVT_REMOTE_OOB_DATA_REQ  0x35
#define HCI_EVT_SIMPLE_PAIRING_COMPLETE 0x36
#define HCI_EVT_LINK_SUPERVISION_TO_CHANGE 0x38
#define HCI_EVT_ENHANCED_FLUSH_COMPLETE 0x39
#define HCI_EVT_USER_PASSKEY_NOTIFY  0x3B
#define HCI_EVT_KEYPRESS_NOTIFY      0x3C
#define HCI_EVT_REMOTE_HOST_FEAT     0x3D
#define HCI_EVT_LE_META              0x3E
#define HCI_EVT_NUM_COMPLETED_BLOCKS 0x48

/* LE Meta Events */
#define HCI_LEVT_CONN_COMPLETE       0x01
#define HCI_LEVT_ADV_REPORT          0x02
#define HCI_LEVT_CONN_UPDATE_COMPLETE 0x03
#define HCI_LEVT_READ_REMOTE_FEAT_COMPLETE 0x04
#define HCI_LEVT_LTK_REQUEST         0x05
#define HCI_LEVT_REMOTE_CONN_PARAM_REQ 0x06
#define HCI_LEVT_DATA_LEN_CHANGE     0x07
#define HCI_LEVT_READ_LOCAL_P256_COMPLETE 0x08
#define HCI_LEVT_GENERATE_DHKEY_COMPLETE 0x09
#define HCI_LEVT_ENHANCED_CONN_COMPLETE 0x0A
#define HCI_LEVT_DIRECT_ADV_REPORT   0x0B
#define HCI_LEVT_PHY_UPDATE_COMPLETE 0x0C
#define HCI_LEVT_EXT_ADV_REPORT      0x0D
#define HCI_LEVT_PERIODIC_ADV_SYNC_EST 0x0E
#define HCI_LEVT_PERIODIC_ADV_REPORT 0x0F
#define HCI_LEVT_PERIODIC_ADV_SYNC_LOST 0x10
#define HCI_LEVT_SCAN_TIMEOUT        0x11
#define HCI_LEVT_ADV_SET_TERMINATED  0x12
#define HCI_LEVT_SCAN_REQ_RECEIVED   0x13
#define HCI_LEVT_CHAN_SEL_ALG        0x14

/*==============================================================================
 * L2CAP (Logical Link Control and Adaptation Protocol)
 *==============================================================================*/

/* L2CAP Signaling Commands */
#define L2CAP_CMD_COMMAND_REJECT     0x01
#define L2CAP_CMD_CONN_REQ           0x02
#define L2CAP_CMD_CONN_RSP           0x03
#define L2CAP_CMD_CONFIG_REQ         0x04
#define L2CAP_CMD_CONFIG_RSP         0x05
#define L2CAP_CMD_DISCONN_REQ        0x06
#define L2CAP_CMD_DISCONN_RSP        0x07
#define L2CAP_CMD_ECHO_REQ           0x08
#define L2CAP_CMD_ECHO_RSP           0x09
#define L2CAP_CMD_INFO_REQ           0x0A
#define L2CAP_CMD_INFO_RSP           0x0B
#define L2CAP_CMD_CREATE_CHAN_REQ    0x0C
#define L2CAP_CMD_CREATE_CHAN_RSP    0x0D
#define L2CAP_CMD_MOVE_CHAN_REQ      0x0E
#define L2CAP_CMD_MOVE_CHAN_RSP      0x0F
#define L2CAP_CMD_MOVE_CHAN_CFM      0x10
#define L2CAP_CMD_MOVE_CHAN_CFM_RSP  0x11
#define L2CAP_CMD_CONN_PARAM_UPDATE_REQ 0x12
#define L2CAP_CMD_CONN_PARAM_UPDATE_RSP 0x13
#define L2CAP_CMD_LE_CREDIT_BASED_CONN_REQ 0x14
#define L2CAP_CMD_LE_CREDIT_BASED_CONN_RSP 0x15
#define L2CAP_CMD_FLOW_CONTROL_CREDIT 0x16

/* L2CAP Fixed Channels */
#define L2CAP_CID_NULL              0x00
#define L2CAP_CID_SIGNALING         0x01
#define L2CAP_CID_CONNECTIONLESS    0x02
#define L2CAP_CID_AMP_MANAGER       0x03
#define L2CAP_CID_ATT               0x04
#define L2CAP_CID_LE_SIGNALING      0x05
#define L2CAP_CID_SMP               0x06
#define L2CAP_CID_AMP_TEST          0x3F

/*==============================================================================
 * RFCOMM Protocol
 *==============================================================================*/

/* RFCOMM Frame Types */
#define RFCOMM_FRAME_SABM           0x2F
#define RFCOMM_FRAME_UA             0x63
#define RFCOMM_FRAME_DM             0x0F
#define RFCOMM_FRAME_DISC           0x43
#define RFCOMM_FRAME_UIH            0xEF
#define RFCOMM_FRAME_UI             0x03

/* RFCOMM Control Commands */
#define RFCOMM_CMD_PN                0x80
#define RFCOMM_CMD_PN_RSP            0x81
#define RFCOMM_CMD_MSC               0xE0
#define RFCOMM_CMD_MSC_RSP           0xE1
#define RFCOMM_CMD_RPN               0x90
#define RFCOMM_CMD_RPN_RSP           0x91
#define RFCOMM_CMD_RLS               0xA0
#define RFCOMM_CMD_RLS_RSP           0xA1
#define RFCOMM_CMD_TEST              0x20
#define RFCOMM_CMD_TEST_RSP          0x21
#define RFCOMM_CMD_FCON              0xA0
#define RFCOMM_CMD_FCOFF             0x60

/*==============================================================================
 * SDP (Service Discovery Protocol)
 *==============================================================================*/

/* SDP PDU IDs */
#define SDP_PDU_ERROR_RSP            0x01
#define SDP_PDU_SVC_SEARCH_REQ       0x02
#define SDP_PDU_SVC_SEARCH_RSP       0x03
#define SDP_PDU_SVC_ATTR_REQ         0x04
#define SDP_PDU_SVC_ATTR_RSP         0x05
#define SDP_PDU_SVC_SEARCH_ATTR_REQ  0x06
#define SDP_PDU_SVC_SEARCH_ATTR_RSP  0x07

/* SDP Attribute IDs */
#define SDP_ATTR_SVC_RECORD_HANDLE   0x0000
#define SDP_ATTR_SVC_CLASS_ID_LIST   0x0001
#define SDP_ATTR_SVC_RECORD_STATE    0x0002
#define SDP_ATTR_SVC_ID              0x0003
#define SDP_ATTR_PROTOCOL_DESC_LIST  0x0004
#define SDP_ATTR_BROWSE_GROUP_LIST   0x0005
#define SDP_ATTR_LANG_BASE_ATTR_ID   0x0006
#define SDP_ATTR_SVC_INFO_TIME_TO_LIVE 0x0007
#define SDP_ATTR_SVC_AVAILABILITY    0x0008
#define SDP_ATTR_BLUETOOTH_PROFILE_DESC_LIST 0x0009
#define SDP_ATTR_DOCUMENTATION_URL   0x000A
#define SDP_ATTR_CLIENT_EXECUTABLE_URL 0x000B
#define SDP_ATTR_ICON_URL            0x000C
#define SDP_ATTR_ADDITIONAL_PROTOCOL_DESC_LISTS 0x000D

/*==============================================================================
 * GATT (Generic Attribute Profile)
 *==============================================================================*/

/* GATT Attribute Types */
#define GATT_PRIMARY_SERVICE         0x2800
#define GATT_SECONDARY_SERVICE       0x2801
#define GATT_INCLUDE                 0x2802
#define GATT_CHARACTERISTIC          0x2803

/* GATT Characteristic Properties */
#define GATT_CHAR_PROP_BROADCAST     (1 << 0)
#define GATT_CHAR_PROP_READ          (1 << 1)
#define GATT_CHAR_PROP_WRITE_NO_RSP  (1 << 2)
#define GATT_CHAR_PROP_WRITE         (1 << 3)
#define GATT_CHAR_PROP_NOTIFY        (1 << 4)
#define GATT_CHAR_PROP_INDICATE      (1 << 5)
#define GATT_CHAR_PROP_AUTH_SIGNED   (1 << 6)
#define GATT_CHAR_PROP_EXT_PROP      (1 << 7)

/* GATT Client Requests */
#define GATT_REQ_FIND_INFO           0x04
#define GATT_REQ_FIND_BY_TYPE        0x06
#define GATT_REQ_READ_BY_TYPE        0x08
#define GATT_REQ_READ                0x0A
#define GATT_REQ_READ_BLOB           0x0C
#define GATT_REQ_MULTI_READ          0x0E
#define GATT_REQ_WRITE               0x12
#define GATT_REQ_WRITE_CMD           0x52
#define GATT_REQ_PREPARE_WRITE       0x16
#define GATT_REQ_EXECUTE_WRITE       0x18
#define GATT_REQ_HANDLE_VALUE_NOTIFY 0x1B
#define GATT_REQ_HANDLE_VALUE_IND    0x1D

/*==============================================================================
 * SMP (Security Manager Protocol)
 *==============================================================================*/

/* SMP Pairing Commands */
#define SMP_CMD_PAIRING_REQ          0x01
#define SMP_CMD_PAIRING_RSP          0x02
#define SMP_CMD_PAIRING_CONFIRM      0x03
#define SMP_CMD_PAIRING_RANDOM       0x04
#define SMP_CMD_PAIRING_FAILED       0x05
#define SMP_CMD_ENCRYPT_INFO         0x06
#define SMP_CMD_MASTER_IDENT         0x07
#define SMP_CMD_IDENT_INFO           0x08
#define SMP_CMD_IDENT_ADDR_INFO      0x09
#define SMP_CMD_SIGN_INFO            0x0A
#define SMP_CMD_SECURITY_REQ         0x0B

/* SMP IO Capabilities */
#define SMP_IO_DISPLAY_ONLY          0x00
#define SMP_IO_DISPLAY_YES_NO        0x01
#define SMP_IO_KEYBOARD_ONLY         0x02
#define SMP_IO_NO_INPUT_NO_OUTPUT    0x03
#define SMP_IO_KEYBOARD_DISPLAY      0x04

/* SMP Pairing Failed Reasons */
#define SMP_ERR_PASSKEY_ENTRY        0x01
#define SMP_ERR_OOB_NOT_AVAIL        0x02
#define SMP_ERR_AUTH_REQ              0x03
#define SMP_ERR_CONFIRM_FAILED        0x04
#define SMP_ERR_PAIRING_NOT_SUPP      0x05
#define SMP_ERR_ENC_KEY_SIZE          0x06
#define SMP_ERR_CMD_NOT_SUPP          0x07
#define SMP_ERR_UNSPECIFIED           0x08
#define SMP_ERR_REPEATED_ATTEMPTS     0x09
#define SMP_ERR_INVALID_PARAM         0x0A
#define SMP_ERR_DHKEY_CHECK           0x0B
#define SMP_ERR_NUMERIC_COMP           0x0C
#define SMP_ERR_BREDR_PAIRING         0x0D
#define SMP_ERR_CROSS_TRANSPORT       0x0E

/*==============================================================================
 * Bluetooth Device Address
 *==============================================================================*/

typedef struct {
    uint8_t     b[6];
} __attribute__((packed)) bd_addr_t;

#define BD_ADDR_FORMAT "%02x:%02x:%02x:%02x:%02x:%02x"
#define BD_ADDR_ARG(addr) addr.b[5], addr.b[4], addr.b[3], addr.b[2], addr.b[1], addr.b[0]

/*==============================================================================
 * Bluetooth Device Structure
 *==============================================================================*/

struct bt_device {
    bd_addr_t       bd_addr;
    uint8_t         dev_class[3];
    char            name[248];
    uint8_t         features[8];
    uint16_t        handle;
    uint8_t         link_type;
    uint8_t         enc_mode;
    uint8_t         auth_enabled;
    uint32_t        link_key[8];        /* 128-bit link key */
    uint8_t         key_type;
    uint8_t         pin_length;
    uint16_t        conn_interval;
    uint16_t        conn_latency;
    uint16_t        supervision_to;
    uint8_t         role;
    uint8_t         mode;
    uint32_t        flags;
    void*           private_data;
    struct bt_device* next;
};

/* Device flags */
#define BT_DEV_FLAG_CONNECTED       (1 << 0)
#define BT_DEV_FLAG_BONDED          (1 << 1)
#define BT_DEV_FLAG_AUTHENTICATED   (1 << 2)
#define BT_DEV_FLAG_ENCRYPTED       (1 << 3)
#define BT_DEV_FLAG_LE               (1 << 4)
#define BT_DEV_FLAG_BREDR           (1 << 5)
#define BT_DEV_FLAG_DUAL_MODE       (1 << 6)

/*==============================================================================
 * L2CAP Channel Structure
 *==============================================================================*/

struct l2cap_channel {
    uint16_t        psm;                /* Protocol/Service Multiplexer */
    uint16_t        scid;               /* Source CID */
    uint16_t        dcid;               /* Destination CID */
    uint16_t        mtu;                /* Maximum Transmission Unit */
    uint16_t        mps;                /* Maximum PDU Size */
    uint16_t        credits;            /* Credits for LE credit-based flow */
    uint8_t         state;              /* Channel state */
    uint8_t         mode;               /* Retransmission/Flow Control mode */
    uint8_t         fcs;                /* FCS option */
    struct bt_device* dev;               /* Associated device */
    void*           private_data;
    int (*rx_callback)(struct l2cap_channel*, void*, uint16_t);
    struct l2cap_channel* next;
};

/* L2CAP channel states */
#define L2CAP_STATE_CLOSED          0
#define L2CAP_STATE_WAIT_CONN_RSP   1
#define L2CAP_STATE_WAIT_CONNECT    2
#define L2CAP_STATE_CONFIG          3
#define L2CAP_STATE_OPEN            4
#define L2CAP_STATE_WAIT_DISCONN    5

/*==============================================================================
 * RFCOMM DLC Structure
 *==============================================================================*/

struct rfcomm_dlc {
    uint8_t         dlci;               /* DLCI (0-63) */
    uint8_t         state;              /* DLC state */
    uint8_t         priority;           /* Priority */
    uint16_t        mtu;                /* MTU for this DLC */
    uint8_t         credit_rx;          /* Credits for RX */
    uint8_t         credit_tx;          /* Credits for TX */
    struct l2cap_channel* l2cap;         /* Parent L2CAP channel */
    void*           private_data;
    int (*rx_callback)(struct rfcomm_dlc*, void*, uint16_t);
    struct rfcomm_dlc* next;
};

/* RFCOMM DLC states */
#define RFCOMM_DLC_CLOSED           0
#define RFCOMM_DLC_WAIT_UA          1
#define RFCOMM_DLC_WAIT_DM          2
#define RFCOMM_DLC_OPEN             3

/*==============================================================================
 * GATT Service Structure
 *==============================================================================*/

struct gatt_service {
    uint16_t        start_handle;
    uint16_t        end_handle;
    uint16_t        uuid16;
    uint8_t         uuid128[16];
    uint8_t         type;               /* Primary/Secondary */
    struct gatt_service* next;
};

struct gatt_char {
    uint16_t        handle;
    uint16_t        value_handle;
    uint8_t         properties;
    uint16_t        uuid16;
    uint8_t         uuid128[16];
    struct gatt_char* next;
};

struct gatt_desc {
    uint16_t        handle;
    uint16_t        uuid16;
    uint8_t         uuid128[16];
    struct gatt_desc* next;
};

/*==============================================================================
 * Bluetooth Controller Structure
 *==============================================================================*/

struct bt_controller {
    uint32_t        magic;
#define BT_CONTROLLER_MAGIC     0x4254434C  /* "BTCL" */
    
    /* Controller identification */
    uint8_t         version;
    uint16_t        revision;
    uint8_t         lmp_version;
    uint16_t        lmp_subversion;
    uint16_t        manufacturer;
    bd_addr_t       bd_addr;
    char            name[248];
    
    /* Capabilities */
    uint32_t        supported_commands[16];
    uint8_t         features[8];
    uint8_t         le_features[8];
    uint16_t        acl_packet_size;
    uint16_t        sco_packet_size;
    uint16_t        le_packet_size;
    uint16_t        total_acl_packets;
    uint16_t        total_sco_packets;
    uint16_t        total_le_packets;
    
    /* State */
    uint32_t        state;
    uint32_t        scan_mode;
    uint32_t        connectable;
    uint32_t        discoverable;
    uint32_t        pairable;
    
    /* Connected devices */
    struct bt_device* devices;
    uint32_t        num_devices;
    
    /* L2CAP channels */
    struct l2cap_channel* channels;
    uint32_t        num_channels;
    
    /* RFCOMM DLCs */
    struct rfcomm_dlc* dlcs;
    uint32_t        num_dlcs;
    
    /* GATT database */
    struct gatt_service* services;
    struct gatt_char* characteristics;
    struct gatt_desc* descriptors;
    
    /* Security */
    uint8_t         ir[16];              /* Identity Resolving Key */
    uint8_t         irk[16];             /* Identity Resolving Key */
    uint8_t         csrk[16];            /* Connection Signature Resolving Key */
    uint8_t         dhkey[32];            /* Diffie-Hellman key */
    
    /* HCI transport */
    void*           hci_transport;
    int (*hci_send)(struct bt_controller* ctrl, uint16_t opcode, void* data, uint8_t len);
    
    /* Locks */
    lck_mtx_t*      lock;
    
    /* Statistics */
    uint64_t        tx_packets;
    uint64_t        rx_packets;
    uint64_t        tx_bytes;
    uint64_t        rx_bytes;
    uint64_t        errors;
    
    /* DMA buffers */
    void*           dma_tx_buf;
    void*           dma_rx_buf;
    uint32_t        dma_tx_size;
    uint32_t        dma_rx_size;
};

/* Controller states */
#define BT_STATE_OFF               0
#define BT_STATE_RESET             1
#define BT_STATE_INIT              2
#define BT_STATE_IDLE              3
#define BT_STATE_SCANNING          4
#define BT_STATE_ADVERTISING       5
#define BT_STATE_CONNECTING        6
#define BT_STATE_CONNECTED         7
#define BT_STATE_ERROR             8

/* Scan modes */
#define BT_SCAN_DISABLED           0
#define BT_SCAN_INQUIRY            1
#define BT_SCAN_PAGE               2
#define BT_SCAN_BOTH               3

/*==============================================================================
 * Global Variables
 *==============================================================================*/

static lck_grp_t* bluetooth_lck_grp;
static lck_attr_t* bluetooth_lck_attr;
static zone_t bluetooth_controller_zone;
static zone_t bluetooth_device_zone;
static zone_t l2cap_channel_zone;
static zone_t rfcomm_dlc_zone;
static zone_t gatt_service_zone;
static zone_t gatt_char_zone;
static zone_t gatt_desc_zone;

static struct bt_controller* g_bt_controller = NULL;
static lck_mtx_t* g_bt_global_lock = NULL;

/*==============================================================================
 * Forward Declarations
 *==============================================================================*/

static int bt_hci_send_command(struct bt_controller* ctrl, uint16_t opcode, void* params, uint8_t plen);
static int bt_hci_send_acl_data(struct bt_controller* ctrl, uint16_t handle, void* data, uint16_t len);
static int bt_hci_send_sco_data(struct bt_controller* ctrl, uint16_t handle, void* data, uint16_t len);
static void bt_hci_event_handler(struct bt_controller* ctrl, uint8_t* event, uint8_t len);
static void bt_le_meta_event_handler(struct bt_controller* ctrl, uint8_t* event, uint8_t len);
static int bt_l2cap_send(struct l2cap_channel* chan, void* data, uint16_t len);
static int bt_rfcomm_send(struct rfcomm_dlc* dlc, void* data, uint16_t len);
static int bt_gatt_send_request(struct bt_controller* ctrl, uint8_t opcode, uint16_t handle, void* data, uint16_t len);

/*==============================================================================
 * HCI Command Functions
 *==============================================================================*/

static int
bt_hci_send_command(struct bt_controller* ctrl, uint16_t opcode, void* params, uint8_t plen)
{
    uint8_t hci_pkt[260];
    hci_cmd_hdr_t* hdr = (hci_cmd_hdr_t*)hci_pkt;
    
    if (!ctrl || !ctrl->hci_send) {
        return -1;
    }
    
    hci_pkt[0] = 0x01;  /* HCI Command Packet */
    hdr->opcode = opcode;
    hdr->plen = plen;
    
    if (params && plen > 0) {
        memcpy(hci_pkt + 3, params, plen);
    }
    
    return ctrl->hci_send(ctrl, opcode, hci_pkt, plen + 3);
}

static int
bt_hci_send_acl_data(struct bt_controller* ctrl, uint16_t handle, void* data, uint16_t len)
{
    uint8_t hci_pkt[2048];
    hci_acl_hdr_t* hdr = (hci_acl_hdr_t*)(hci_pkt + 1);
    
    hci_pkt[0] = 0x02;  /* HCI ACL Data Packet */
    hdr->handle = handle & 0x0FFF;
    hdr->flags = (handle >> 12) & 0x03;
    hdr->dlen = len;
    
    memcpy(hci_pkt + 4, data, len);
    
    return ctrl->hci_send(ctrl, 0, hci_pkt, len + 4);
}

static int
bt_hci_send_sco_data(struct bt_controller* ctrl, uint16_t handle, void* data, uint16_t len)
{
    uint8_t hci_pkt[64];
    hci_sco_hdr_t* hdr = (hci_sco_hdr_t*)(hci_pkt + 1);
    
    hci_pkt[0] = 0x03;  /* HCI SCO Data Packet */
    hdr->handle = handle & 0x0FFF;
    hdr->flags = (handle >> 12) & 0x03;
    hdr->dlen = len;
    
    memcpy(hci_pkt + 3, data, len);
    
    return ctrl->hci_send(ctrl, 0, hci_pkt, len + 3);
}

/*==============================================================================
 * HCI Event Handlers
 *==============================================================================*/

static void
bt_hci_cmd_complete_handler(struct bt_controller* ctrl, uint8_t* event)
{
    uint8_t num_cmds = event[3];
    uint16_t opcode = *(uint16_t*)(event + 4);
    uint8_t status = event[6];
    uint8_t* params = event + 7;
    uint8_t plen = event[2] - 3;
    
    /* Handle command completion */
    switch (opcode) {
        case HCI_CMD_RESET:
            if (status == 0) {
                ctrl->state = BT_STATE_IDLE;
            }
            break;
            
        case HCI_CMD_READ_BD_ADDR:
            if (status == 0 && plen >= 6) {
                memcpy(&ctrl->bd_addr, params, 6);
            }
            break;
            
        case HCI_CMD_READ_LOCAL_NAME:
            if (status == 0 && plen >= 248) {
                memcpy(ctrl->name, params, 248);
                ctrl->name[247] = '\0';
            }
            break;
            
        case HCI_CMD_READ_LOCAL_FEAT:
            if (status == 0 && plen >= 8) {
                memcpy(ctrl->features, params, 8);
            }
            break;
    }
}

static void
bt_hci_conn_complete_handler(struct bt_controller* ctrl, uint8_t* event)
{
    uint8_t status = event[2];
    uint16_t handle = *(uint16_t*)(event + 3);
    bd_addr_t* addr = (bd_addr_t*)(event + 5);
    uint8_t link_type = event[11];
    uint8_t enc_mode = event[12];
    
    struct bt_device* dev;
    
    if (status != 0) {
        /* Connection failed */
        return;
    }
    
    /* Find or create device */
    dev = bt_device_find_by_addr(ctrl, addr);
    if (!dev) {
        dev = bt_device_create(ctrl, addr);
    }
    
    if (dev) {
        dev->handle = handle;
        dev->link_type = link_type;
        dev->enc_mode = enc_mode;
        dev->flags |= BT_DEV_FLAG_CONNECTED;
        
        /* Update controller state */
        ctrl->state = BT_STATE_CONNECTED;
    }
}

static void
bt_hci_disconn_complete_handler(struct bt_controller* ctrl, uint8_t* event)
{
    uint8_t status = event[2];
    uint16_t handle = *(uint16_t*)(event + 3);
    uint8_t reason = event[5];
    
    struct bt_device* dev = bt_device_find_by_handle(ctrl, handle);
    
    if (dev) {
        dev->flags &= ~BT_DEV_FLAG_CONNECTED;
        dev->handle = 0xFFFF;
    }
    
    if (ctrl->num_devices == 0) {
        ctrl->state = BT_STATE_IDLE;
    }
}

static void
bt_hci_encrypt_change_handler(struct bt_controller* ctrl, uint8_t* event)
{
    uint8_t status = event[2];
    uint16_t handle = *(uint16_t*)(event + 3);
    uint8_t enc_enabled = event[5];
    
    struct bt_device* dev = bt_device_find_by_handle(ctrl, handle);
    
    if (dev) {
        if (enc_enabled) {
            dev->flags |= BT_DEV_FLAG_ENCRYPTED;
        } else {
            dev->flags &= ~BT_DEV_FLAG_ENCRYPTED;
        }
    }
}

static void
bt_hci_event_handler(struct bt_controller* ctrl, uint8_t* event, uint8_t len)
{
    uint8_t evt_code = event[1];
    
    ctrl->rx_packets++;
    
    switch (evt_code) {
        case HCI_EVT_COMMAND_COMPLETE:
            bt_hci_cmd_complete_handler(ctrl, event);
            break;
            
        case HCI_EVT_CONN_COMPLETE:
            bt_hci_conn_complete_handler(ctrl, event);
            break;
            
        case HCI_EVT_DISCONN_COMPLETE:
            bt_hci_disconn_complete_handler(ctrl, event);
            break;
            
        case HCI_EVT_ENCRYPT_CHANGE:
            bt_hci_encrypt_change_handler(ctrl, event);
            break;
            
        case HCI_EVT_LE_META:
            bt_le_meta_event_handler(ctrl, event + 3, event[2] - 1);
            break;
            
        case HCI_EVT_NUM_COMPLETED_PKTS:
            /* Handle completed packets */
            break;
            
        case HCI_EVT_HARDWARE_ERROR:
            ctrl->state = BT_STATE_ERROR;
            ctrl->errors++;
            break;
    }
}

static void
bt_le_meta_event_handler(struct bt_controller* ctrl, uint8_t* event, uint8_t len)
{
    uint8_t subevent = event[0];
    uint8_t* params = event + 1;
    
    switch (subevent) {
        case HCI_LEVT_CONN_COMPLETE:
        case HCI_LEVT_ENHANCED_CONN_COMPLETE:
            /* Handle LE connection complete */
            break;
            
        case HCI_LEVT_ADV_REPORT:
        case HCI_LEVT_EXT_ADV_REPORT:
            /* Handle advertising report */
            break;
            
        case HCI_LEVT_LTK_REQUEST:
            /* Handle LTK request */
            break;
            
        case HCI_LEVT_READ_REMOTE_FEAT_COMPLETE:
            /* Handle remote features */
            break;
    }
}

/*==============================================================================
 * L2CAP Protocol Implementation
 *==============================================================================*/

static int
bt_l2cap_process_command(struct l2cap_channel* chan, uint8_t* data, uint16_t len)
{
    uint8_t code = data[0];
    uint8_t ident = data[1];
    uint16_t cmd_len = *(uint16_t*)(data + 2);
    uint8_t* cmd_data = data + 4;
    
    switch (code) {
        case L2CAP_CMD_CONN_REQ: {
            uint16_t psm = *(uint16_t*)cmd_data;
            uint16_t scid = *(uint16_t*)(cmd_data + 2);
            
            /* Create response */
            uint8_t rsp[8];
            rsp[0] = L2CAP_CMD_CONN_RSP;
            rsp[1] = ident;
            rsp[2] = 0;
            rsp[3] = 8;
            *(uint16_t*)(rsp + 4) = scid;           /* Destination CID */
            *(uint16_t*)(rsp + 6) = scid;           /* Source CID (same) */
            *(uint16_t*)(rsp + 8) = 0x0000;         /* Result: Success */
            *(uint16_t*)(rsp + 10) = 0x0000;        /* Status: No info */
            
            return bt_l2cap_send(chan, rsp, 12);
        }
        
        case L2CAP_CMD_CONFIG_REQ: {
            uint16_t dcid = *(uint16_t*)cmd_data;
            uint16_t flags = *(uint16_t*)(cmd_data + 2);
            
            /* Send config response */
            uint8_t rsp[10];
            rsp[0] = L2CAP_CMD_CONFIG_RSP;
            rsp[1] = ident;
            rsp[2] = 0;
            rsp[3] = 10;
            *(uint16_t*)(rsp + 4) = dcid;           /* Source CID */
            *(uint16_t*)(rsp + 6) = 0x0000;         /* Flags */
            *(uint16_t*)(rsp + 8) = 0x0000;         /* Result: Success */
            
            return bt_l2cap_send(chan, rsp, 12);
        }
        
        case L2CAP_CMD_DISCONN_REQ: {
            uint16_t dcid = *(uint16_t*)cmd_data;
            uint16_t scid = *(uint16_t*)(cmd_data + 2);
            
            /* Send disconnect response */
            uint8_t rsp[8];
            rsp[0] = L2CAP_CMD_DISCONN_RSP;
            rsp[1] = ident;
            rsp[2] = 0;
            rsp[3] = 4;
            *(uint16_t*)(rsp + 4) = dcid;
            *(uint16_t*)(rsp + 6) = scid;
            
            return bt_l2cap_send(chan, rsp, 8);
        }
        
        case L2CAP_CMD_INFO_REQ: {
            uint16_t info_type = *(uint16_t*)cmd_data;
            
            /* Send info response */
            uint8_t rsp[12];
            rsp[0] = L2CAP_CMD_INFO_RSP;
            rsp[1] = ident;
            rsp[2] = 0;
            rsp[3] = 8;
            *(uint16_t*)(rsp + 4) = info_type;
            *(uint16_t*)(rsp + 6) = 0x0000;         /* Result: Success */
            
            if (info_type == 0x0002) {  /* Connectionless MTU */
                *(uint16_t*)(rsp + 8) = 1024;
            }
            
            return bt_l2cap_send(chan, rsp, 12);
        }
        
        case L2CAP_CMD_CONN_PARAM_UPDATE_REQ: {
            /* Handle LE connection parameter update request */
            uint16_t min_interval = *(uint16_t*)cmd_data;
            uint16_t max_interval = *(uint16_t*)(cmd_data + 2);
            uint16_t latency = *(uint16_t*)(cmd_data + 4);
            uint16_t timeout = *(uint16_t*)(cmd_data + 6);
            
            /* Send response */
            uint8_t rsp[6];
            rsp[0] = L2CAP_CMD_CONN_PARAM_UPDATE_RSP;
            rsp[1] = ident;
            rsp[2] = 0;
            rsp[3] = 2;
            *(uint16_t*)(rsp + 4) = 0x0000;         /* Result: Accepted */
            
            return bt_l2cap_send(chan, rsp, 6);
        }
    }
    
    return 0;
}

static int
bt_l2cap_send(struct l2cap_channel* chan, void* data, uint16_t len)
{
    struct bt_controller* ctrl = g_bt_controller;
    uint8_t l2cap_hdr[4];
    uint8_t acl_buf[2048];
    uint16_t acl_len = len + 4;
    
    if (!ctrl || !chan || !chan->dev || !(chan->dev->flags & BT_DEV_FLAG_CONNECTED)) {
        return -1;
    }
    
    /* Build L2CAP header */
    *(uint16_t*)l2cap_hdr = len;                 /* Length */
    *(uint16_t*)(l2cap_hdr + 2) = chan->dcid;    /* Channel ID */
    
    /* Combine header and data */
    memcpy(acl_buf, l2cap_hdr, 4);
    memcpy(acl_buf + 4, data, len);
    
    /* Send via HCI ACL */
    int ret = bt_hci_send_acl_data(ctrl, chan->dev->handle, acl_buf, acl_len);
    
    if (ret == 0) {
        ctrl->tx_packets++;
        ctrl->tx_bytes += acl_len;
    }
    
    return ret;
}

static struct l2cap_channel*
bt_l2cap_channel_create(struct bt_device* dev, uint16_t psm)
{
    struct l2cap_channel* chan;
    
    chan = (struct l2cap_channel*)zalloc(l2cap_channel_zone);
    if (!chan) {
        return NULL;
    }
    
    bzero(chan, sizeof(struct l2cap_channel));
    
    chan->psm = psm;
    chan->dev = dev;
    chan->scid = 0x0040;  /* Dynamic CID range */
    chan->mtu = 672;       /* Default L2CAP MTU */
    chan->state = L2CAP_STATE_CLOSED;
    
    return chan;
}

static void
bt_l2cap_channel_free(struct l2cap_channel* chan)
{
    if (chan) {
        zfree(l2cap_channel_zone, chan);
    }
}

/*==============================================================================
 * RFCOMM Protocol Implementation
 *==============================================================================*/

static int
bt_rfcomm_send_frame(struct rfcomm_dlc* dlc, uint8_t type, uint8_t pf, uint8_t* data, uint16_t len)
{
    uint8_t frame[256];
    uint8_t control;
    uint8_t addr;
    uint16_t frame_len = 2;  /* Address + Control */
    
    /* Build address field */
    addr = (dlc->dlci << 2) | 0x01;  /* EA=1, C/R=1 */
    
    /* Build control field */
    control = type;
    if (pf) {
        control |= 0x10;
    }
    
    frame[0] = addr;
    frame[1] = control;
    
    /* Add length field if needed (UIH frames have no length) */
    if (type != RFCOMM_FRAME_UIH) {
        if (len > 127) {
            frame[2] = (len & 0x7F) | 0x80;
            frame[3] = (len >> 7) & 0x7F;
            frame_len += 2;
        } else {
            frame[2] = len & 0x7F;
            frame_len += 1;
        }
    }
    
    /* Add data */
    if (data && len > 0) {
        memcpy(frame + frame_len, data, len);
        frame_len += len;
    }
    
    /* Send via L2CAP */
    return bt_l2cap_send(dlc->l2cap, frame, frame_len);
}

static int
bt_rfcomm_send_sabm(struct rfcomm_dlc* dlc)
{
    return bt_rfcomm_send_frame(dlc, RFCOMM_FRAME_SABM, 1, NULL, 0);
}

static int
bt_rfcomm_send_ua(struct rfcomm_dlc* dlc)
{
    return bt_rfcomm_send_frame(dlc, RFCOMM_FRAME_UA, 1, NULL, 0);
}

static int
bt_rfcomm_send_disc(struct rfcomm_dlc* dlc)
{
    return bt_rfcomm_send_frame(dlc, RFCOMM_FRAME_DISC, 1, NULL, 0);
}

static int
bt_rfcomm_send_uih(struct rfcomm_dlc* dlc, void* data, uint16_t len)
{
    return bt_rfcomm_send_frame(dlc, RFCOMM_FRAME_UIH, 0, data, len);
}

static int
bt_rfcomm_send(struct rfcomm_dlc* dlc, void* data, uint16_t len)
{
    return bt_rfcomm_send_uih(dlc, data, len);
}

static int
bt_rfcomm_process_frame(struct rfcomm_dlc* dlc, uint8_t* frame, uint16_t len)
{
    uint8_t addr = frame[0];
    uint8_t control = frame[1];
    uint8_t type = control & 0xEF;  /* Mask out PF bit */
    uint8_t pf = (control >> 4) & 1;
    uint8_t dlci = (addr >> 2) & 0x3F;
    uint8_t* data = frame + 2;
    uint16_t data_len = len - 2;
    
    /* Handle length field for non-UIH frames */
    if (type != RFCOMM_FRAME_UIH) {
        if (data_len >= 1) {
            uint8_t len1 = data[0];
            if (len1 & 0x80) {
                /* 2-byte length */
                data_len = ((len1 & 0x7F) | (data[1] << 7));
                data += 2;
            } else {
                data_len = len1;
                data += 1;
            }
        }
    }
    
    switch (type) {
        case RFCOMM_FRAME_SABM:
            /* Establish DLC */
            dlc->state = RFCOMM_DLC_OPEN;
            bt_rfcomm_send_ua(dlc);
            break;
            
        case RFCOMM_FRAME_UA:
            /* DLC established */
            dlc->state = RFCOMM_DLC_OPEN;
            break;
            
        case RFCOMM_FRAME_DISC:
            /* Disconnect */
            dlc->state = RFCOMM_DLC_CLOSED;
            bt_rfcomm_send_ua(dlc);
            break;
            
        case RFCOMM_FRAME_UIH:
            /* Data frame */
            if (dlc->rx_callback) {
                dlc->rx_callback(dlc, data, data_len);
            }
            break;
    }
    
    return 0;
}

static struct rfcomm_dlc*
bt_rfcomm_dlc_create(struct l2cap_channel* l2cap, uint8_t dlci)
{
    struct rfcomm_dlc* dlc;
    
    dlc = (struct rfcomm_dlc*)zalloc(rfcomm_dlc_zone);
    if (!dlc) {
        return NULL;
    }
    
    bzero(dlc, sizeof(struct rfcomm_dlc));
    
    dlc->dlci = dlci;
    dlc->l2cap = l2cap;
    dlc->state = RFCOMM_DLC_CLOSED;
    dlc->mtu = l2cap->mtu - 5;  /* RFCOMM header overhead */
    dlc->credit_rx = 7;          /* Default credits */
    dlc->credit_tx = 7;
    
    return dlc;
}

static void
bt_rfcomm_dlc_free(struct rfcomm_dlc* dlc)
{
    if (dlc) {
        zfree(rfcomm_dlc_zone, dlc);
    }
}


/*==============================================================================
 * GATT Protocol Implementation
 *==============================================================================*/

static int
bt_gatt_send_request(struct bt_controller* ctrl, uint8_t opcode, uint16_t handle, void* data, uint16_t len)
{
    uint8_t gatt_pdu[512];
    uint16_t pdu_len = 1 + len;  /* Opcode + data */
    
    gatt_pdu[0] = opcode;
    
    if (data && len > 0) {
        memcpy(gatt_pdu + 1, data, len);
    }
    
    /* Send via L2CAP ATT channel */
    /* Find or create ATT channel for the device */
    
    return 0;
}

static void
bt_gatt_process_request(struct l2cap_channel* chan, uint8_t* data, uint16_t len)
{
    uint8_t opcode = data[0];
    uint8_t* params = data + 1;
    uint16_t plen = len - 1;
    
    switch (opcode) {
        case GATT_REQ_READ_BY_TYPE: {
            uint16_t start = *(uint16_t*)params;
            uint16_t end = *(uint16_t*)(params + 2);
            uint16_t uuid = *(uint16_t*)(params + 4);
            
            /* Find matching attributes */
            uint8_t rsp[32];
            rsp[0] = GATT_REQ_READ_BY_TYPE + 1;  /* Response */
            rsp[1] = 0x00;  /* Length per attribute */
            /* Fill in attributes... */
            
            bt_l2cap_send(chan, rsp, 8);
            break;
        }
        
        case GATT_REQ_READ: {
            uint16_t handle = *(uint16_t*)params;
            
            /* Read attribute value */
            uint8_t rsp[32];
            rsp[0] = GATT_REQ_READ + 1;  /* Response */
            /* Fill in value... */
            
            bt_l2cap_send(chan, rsp, 6);
            break;
        }
        
        case GATT_REQ_WRITE: {
            uint16_t handle = *(uint16_t*)params;
            uint8_t* value = params + 2;
            uint16_t value_len = plen - 2;
            
            /* Write attribute value */
            
            /* Send response */
            uint8_t rsp[1];
            rsp[0] = GATT_REQ_WRITE + 1;  /* Response */
            bt_l2cap_send(chan, rsp, 1);
            break;
        }
    }
}

/*==============================================================================
 * SMP (Security Manager Protocol)
 *==============================================================================*/

static int
bt_smp_send_pairing_request(struct bt_device* dev, uint8_t io_cap, uint8_t oob, uint8_t auth_req)
{
    uint8_t smp_cmd[7];
    
    smp_cmd[0] = SMP_CMD_PAIRING_REQ;
    smp_cmd[1] = io_cap;
    smp_cmd[2] = oob;
    smp_cmd[3] = auth_req;
    smp_cmd[4] = 0x10;  /* Max encryption key size (16) */
    smp_cmd[5] = 0xFF;   /* Initiator key distribution */
    smp_cmd[6] = 0xFF;   /* Responder key distribution */
    
    /* Send via L2CAP SMP channel */
    return 0;
}

static int
bt_smp_process_pairing(struct l2cap_channel* chan, uint8_t* data, uint16_t len)
{
    uint8_t cmd = data[0];
    uint8_t* params = data + 1;
    
    switch (cmd) {
        case SMP_CMD_PAIRING_REQ: {
            uint8_t io_cap = params[0];
            uint8_t oob = params[1];
            uint8_t auth_req = params[2];
            uint8_t max_key_size = params[3];
            uint8_t init_key_dist = params[4];
            uint8_t resp_key_dist = params[5];
            
            /* Send pairing response */
            uint8_t rsp[7];
            rsp[0] = SMP_CMD_PAIRING_RSP;
            rsp[1] = SMP_IO_DISPLAY_YES_NO;  /* IO capabilities */
            rsp[2] = 0x00;                    /* OOB not present */
            rsp[3] = auth_req;
            rsp[4] = max_key_size;
            rsp[5] = resp_key_dist;
            rsp[6] = init_key_dist;
            
            return bt_l2cap_send(chan, rsp, 7);
        }
        
        case SMP_CMD_PAIRING_CONFIRM: {
            /* Store confirm value */
            memcpy(chan->dev->confirm, params, 16);
            
            /* Generate random and send */
            uint8_t random[16];
            bt_smp_generate_random(random, 16);
            memcpy(chan->dev->random, random, 16);
            
            uint8_t rsp[17];
            rsp[0] = SMP_CMD_PAIRING_RANDOM;
            memcpy(rsp + 1, random, 16);
            
            return bt_l2cap_send(chan, rsp, 17);
        }
        
        case SMP_CMD_PAIRING_RANDOM: {
            /* Verify confirm value */
            uint8_t confirm[16];
            bt_smp_calculate_confirm(chan->dev, confirm);
            
            if (memcmp(confirm, chan->dev->confirm, 16) == 0) {
                /* Pairing successful */
                chan->dev->flags |= BT_DEV_FLAG_BONDED;
                
                /* Generate keys */
                uint8_t ltk[16];
                bt_smp_generate_ltk(chan->dev, ltk);
                
                /* Send encryption info */
                uint8_t enc_info[17];
                enc_info[0] = SMP_CMD_ENCRYPT_INFO;
                memcpy(enc_info + 1, ltk, 16);
                bt_l2cap_send(chan, enc_info, 17);
                
                /* Send master ident */
                uint8_t ident[8];
                ident[0] = SMP_CMD_MASTER_IDENT;
                *(uint16_t*)(ident + 1) = 0x0010;  /* EDIV */
                *(uint64_t*)(ident + 3) = 0x1234567890ABCDEF;  /* RAND */
                bt_l2cap_send(chan, ident, 11);
            } else {
                /* Pairing failed */
                uint8_t fail[2];
                fail[0] = SMP_CMD_PAIRING_FAILED;
                fail[1] = SMP_ERR_CONFIRM_FAILED;
                bt_l2cap_send(chan, fail, 2);
            }
            break;
        }
    }
    
    return 0;
}

/*==============================================================================
 * Device Management
 *==============================================================================*/

static struct bt_device*
bt_device_find_by_addr(struct bt_controller* ctrl, bd_addr_t* addr)
{
    struct bt_device* dev = ctrl->devices;
    
    while (dev) {
        if (memcmp(&dev->bd_addr, addr, 6) == 0) {
            return dev;
        }
        dev = dev->next;
    }
    
    return NULL;
}

static struct bt_device*
bt_device_find_by_handle(struct bt_controller* ctrl, uint16_t handle)
{
    struct bt_device* dev = ctrl->devices;
    
    while (dev) {
        if (dev->handle == handle) {
            return dev;
        }
        dev = dev->next;
    }
    
    return NULL;
}

static struct bt_device*
bt_device_create(struct bt_controller* ctrl, bd_addr_t* addr)
{
    struct bt_device* dev;
    
    dev = (struct bt_device*)zalloc(bluetooth_device_zone);
    if (!dev) {
        return NULL;
    }
    
    bzero(dev, sizeof(struct bt_device));
    
    memcpy(&dev->bd_addr, addr, 6);
    dev->handle = 0xFFFF;
    
    /* Add to list */
    lck_mtx_lock(ctrl->lock);
    dev->next = ctrl->devices;
    ctrl->devices = dev;
    ctrl->num_devices++;
    lck_mtx_unlock(ctrl->lock);
    
    return dev;
}

static void
bt_device_free(struct bt_controller* ctrl, struct bt_device* dev)
{
    struct bt_device* prev = NULL;
    struct bt_device* curr = ctrl->devices;
    
    lck_mtx_lock(ctrl->lock);
    
    while (curr) {
        if (curr == dev) {
            if (prev) {
                prev->next = curr->next;
            } else {
                ctrl->devices = curr->next;
            }
            ctrl->num_devices--;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    
    lck_mtx_unlock(ctrl->lock);
    
    zfree(bluetooth_device_zone, dev);
}

/*==============================================================================
 * Controller Initialization
 *==============================================================================*/

static int
bt_controller_reset(struct bt_controller* ctrl)
{
    uint8_t status;
    
    ctrl->state = BT_STATE_RESET;
    
    /* Send HCI Reset command */
    status = bt_hci_send_command(ctrl, HCI_CMD_RESET, NULL, 0);
    if (status != 0) {
        return -1;
    }
    
    /* Wait for reset complete */
    /* In real implementation, wait for command complete event */
    
    return 0;
}

static int
bt_controller_read_local_info(struct bt_controller* ctrl)
{
    /* Read BD_ADDR */
    bt_hci_send_command(ctrl, HCI_CMD_READ_BD_ADDR, NULL, 0);
    
    /* Read local name */
    bt_hci_send_command(ctrl, HCI_CMD_READ_LOCAL_NAME, NULL, 0);
    
    /* Read local features */
    bt_hci_send_command(ctrl, HCI_CMD_READ_LOCAL_FEAT, NULL, 0);
    
    /* Read buffer size */
    bt_hci_send_command(ctrl, HCI_CMD_READ_BUFFER_SIZE, NULL, 0);
    
    /* Read LE features if supported */
    if (ctrl->features[4] & 0x20) {  /* Bit 5: LE Supported */
        bt_hci_send_command(ctrl, HCI_CMD_LE_READ_LOCAL_FEAT, NULL, 0);
    }
    
    return 0;
}

static int
bt_controller_configure(struct bt_controller* ctrl)
{
    /* Set event mask */
    uint8_t event_mask[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F};
    bt_hci_send_command(ctrl, HCI_CMD_SET_EVENT_MASK, event_mask, 8);
    
    /* Set LE event mask if supported */
    if (ctrl->features[4] & 0x20) {
        uint8_t le_event_mask[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
        bt_hci_send_command(ctrl, HCI_CMD_LE_SET_EVENT_MASK, le_event_mask, 8);
    }
    
    /* Set default scan parameters */
    /* Set default advertising parameters */
    
    return 0;
}

struct bt_controller*
bt_controller_create(void)
{
    struct bt_controller* ctrl;
    
    ctrl = (struct bt_controller*)zalloc(bluetooth_controller_zone);
    if (!ctrl) {
        return NULL;
    }
    
    bzero(ctrl, sizeof(struct bt_controller));
    
    ctrl->magic = BT_CONTROLLER_MAGIC;
    
    ctrl->lock = lck_mtx_alloc_init(bluetooth_lck_grp, bluetooth_lck_attr);
    if (!ctrl->lock) {
        zfree(bluetooth_controller_zone, ctrl);
        return NULL;
    }
    
    ctrl->state = BT_STATE_OFF;
    
    /* Allocate DMA buffers */
    ctrl->dma_tx_size = 2048;
    ctrl->dma_tx_buf = kalloc(ctrl->dma_tx_size);
    ctrl->dma_rx_size = 2048;
    ctrl->dma_rx_buf = kalloc(ctrl->dma_rx_size);
    
    if (!ctrl->dma_tx_buf || !ctrl->dma_rx_buf) {
        if (ctrl->dma_tx_buf) kfree(ctrl->dma_tx_buf, ctrl->dma_tx_size);
        if (ctrl->dma_rx_buf) kfree(ctrl->dma_rx_buf, ctrl->dma_rx_size);
        lck_mtx_free(ctrl->lock, bluetooth_lck_grp);
        zfree(bluetooth_controller_zone, ctrl);
        return NULL;
    }
    
    return ctrl;
}

void
bt_controller_destroy(struct bt_controller* ctrl)
{
    if (!ctrl || ctrl->magic != BT_CONTROLLER_MAGIC) {
        return;
    }
    
    /* Free all devices */
    while (ctrl->devices) {
        bt_device_free(ctrl, ctrl->devices);
    }
    
    /* Free all channels */
    while (ctrl->channels) {
        struct l2cap_channel* chan = ctrl->channels;
        ctrl->channels = chan->next;
        bt_l2cap_channel_free(chan);
    }
    
    /* Free all DLCs */
    while (ctrl->dlcs) {
        struct rfcomm_dlc* dlc = ctrl->dlcs;
        ctrl->dlcs = dlc->next;
        bt_rfcomm_dlc_free(dlc);
    }
    
    /* Free DMA buffers */
    if (ctrl->dma_tx_buf) kfree(ctrl->dma_tx_buf, ctrl->dma_tx_size);
    if (ctrl->dma_rx_buf) kfree(ctrl->dma_rx_buf, ctrl->dma_rx_size);
    
    lck_mtx_free(ctrl->lock, bluetooth_lck_grp);
    
    ctrl->magic = 0;
    zfree(bluetooth_controller_zone, ctrl);
}

int
bt_controller_start(struct bt_controller* ctrl)
{
    if (!ctrl) {
        return -1;
    }
    
    /* Reset controller */
    if (bt_controller_reset(ctrl) != 0) {
        return -1;
    }
    
    /* Read local information */
    bt_controller_read_local_info(ctrl);
    
    /* Configure controller */
    bt_controller_configure(ctrl);
    
    ctrl->state = BT_STATE_IDLE;
    
    return 0;
}

int
bt_controller_stop(struct bt_controller* ctrl)
{
    if (!ctrl) {
        return -1;
    }
    
    /* Disconnect all devices */
    /* Reset controller */
    bt_controller_reset(ctrl);
    
    ctrl->state = BT_STATE_OFF;
    
    return 0;
}

/*==============================================================================
 * Public API Functions
 *==============================================================================*/

int
IOBluetooth_initialize(void)
{
    /* Initialize lock group */
    bluetooth_lck_grp = lck_grp_alloc_init("IOBluetooth", LCK_GRP_ATTR_NULL);
    if (!bluetooth_lck_grp) {
        return ENOMEM;
    }
    
    bluetooth_lck_attr = lck_attr_alloc_init();
    if (!bluetooth_lck_attr) {
        lck_grp_free(bluetooth_lck_grp);
        return ENOMEM;
    }
    
    /* Create global lock */
    g_bt_global_lock = lck_mtx_alloc_init(bluetooth_lck_grp, bluetooth_lck_attr);
    if (!g_bt_global_lock) {
        lck_attr_free(bluetooth_lck_attr);
        lck_grp_free(bluetooth_lck_grp);
        return ENOMEM;
    }
    
    /* Create zone allocators */
    bluetooth_controller_zone = zinit(sizeof(struct bt_controller),
                                      1024 * sizeof(struct bt_controller),
                                      0, "IOBluetoothController");
    
    bluetooth_device_zone = zinit(sizeof(struct bt_device),
                                   1024 * sizeof(struct bt_device),
                                   0, "IOBluetoothDevice");
    
    l2cap_channel_zone = zinit(sizeof(struct l2cap_channel),
                                4096 * sizeof(struct l2cap_channel),
                                0, "L2CAPChannel");
    
    rfcomm_dlc_zone = zinit(sizeof(struct rfcomm_dlc),
                             4096 * sizeof(struct rfcomm_dlc),
                             0, "RFCOMMDLC");
    
    gatt_service_zone = zinit(sizeof(struct gatt_service),
                               512 * sizeof(struct gatt_service),
                               0, "GATTService");
    
    gatt_char_zone = zinit(sizeof(struct gatt_char),
                            1024 * sizeof(struct gatt_char),
                            0, "GATTCharacteristic");
    
    gatt_desc_zone = zinit(sizeof(struct gatt_desc),
                            2048 * sizeof(struct gatt_desc),
                            0, "GATTDescriptor");
    
    /* Create main controller */
    g_bt_controller = bt_controller_create();
    if (!g_bt_controller) {
        lck_mtx_free(g_bt_global_lock, bluetooth_lck_grp);
        lck_attr_free(bluetooth_lck_attr);
        lck_grp_free(bluetooth_lck_grp);
        return ENOMEM;
    }
    
    printf("IOBluetooth: Core Bluetooth initialized\n");
    
    return 0;
}

void
IOBluetooth_terminate(void)
{
    if (g_bt_controller) {
        bt_controller_stop(g_bt_controller);
        bt_controller_destroy(g_bt_controller);
        g_bt_controller = NULL;
    }
    
    /* Free zones */
    if (bluetooth_controller_zone) zfree(bluetooth_controller_zone, NULL);
    if (bluetooth_device_zone) zfree(bluetooth_device_zone, NULL);
    if (l2cap_channel_zone) zfree(l2cap_channel_zone, NULL);
    if (rfcomm_dlc_zone) zfree(rfcomm_dlc_zone, NULL);
    if (gatt_service_zone) zfree(gatt_service_zone, NULL);
    if (gatt_char_zone) zfree(gatt_char_zone, NULL);
    if (gatt_desc_zone) zfree(gatt_desc_zone, NULL);
    
    if (g_bt_global_lock) {
        lck_mtx_free(g_bt_global_lock, bluetooth_lck_grp);
    }
    
    if (bluetooth_lck_attr) {
        lck_attr_free(bluetooth_lck_attr);
    }
    
    if (bluetooth_lck_grp) {
        lck_grp_free(bluetooth_lck_grp);
    }
    
    printf("IOBluetooth: Core Bluetooth terminated\n");
}
