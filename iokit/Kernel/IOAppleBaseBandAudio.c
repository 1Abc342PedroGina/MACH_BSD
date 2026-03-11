/*
 * Copyright (c) 2012-2022 Apple Inc. All rights reserved.
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
 * AppleBasebandAudio.c
 * Audio Interface for Baseband Cellular Modem
 * Handles voice calls, vocoding, PCM routing, and audio processing
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/conf.h>

#include <kern/kern_types.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/zalloc.h>

#include <machine/machine_routines.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/IOReturn.h>
#include <IOKit/assert.h>
#include <IOKit/IOTypes.h>

#include <libkern/OSAtomic.h>
#include <libkern/OSMalloc.h>

#include <dev/audio/audio_driver.h>
#include <dev/audio/audio_support.h>

/*==============================================================================
 * Baseband Audio Types and Constants
 *==============================================================================*/

#define APPLE_BASEBAND_AUDIO_REV         0x200     /* Driver revision */

/* Audio Formats */
#define BB_AUDIO_FORMAT_PCM              0x01      /* Raw PCM */
#define BB_AUDIO_FORMAT_ALAW              0x02      /* A-law compressed */
#define BB_AUDIO_FORMAT_ULAW              0x03      /* μ-law compressed */
#define BB_AUDIO_FORMAT_AMR                0x04      /* Adaptive Multi-Rate */
#define BB_AUDIO_FORMAT_AMR_WB             0x05      /* AMR Wideband */
#define BB_AUDIO_FORMAT_EVS                0x06      /* Enhanced Voice Services */
#define BB_AUDIO_FORMAT_QCELP              0x07      /* Qualcomm CDMA */
#define BB_AUDIO_FORMAT_EVRC               0x08      /* Enhanced Variable Rate Codec */
#define BB_AUDIO_FORMAT_HR                  0x09      /* Half Rate */
#define BB_AUDIO_FORMAT_FR                  0x0A      /* Full Rate */
#define BB_AUDIO_FORMAT_EFR                 0x0B      /* Enhanced Full Rate */

/* Sample Rates */
#define BB_SAMPLE_RATE_8KHZ               8000      /* Narrowband */
#define BB_SAMPLE_RATE_16KHZ              16000     /* Wideband */
#define BB_SAMPLE_RATE_32KHZ              32000     /* Super-wideband */
#define BB_SAMPLE_RATE_48KHZ              48000     /* Fullband */

/* Audio Channels */
#define BB_AUDIO_CHANNEL_MONO              1
#define BB_AUDIO_CHANNEL_STEREO            2

/* Bit Depths */
#define BB_BIT_DEPTH_16                    16
#define BB_BIT_DEPTH_24                    24
#define BB_BIT_DEPTH_32                    32

/* Audio Paths */
#define BB_AUDIO_PATH_NONE                 0
#define BB_AUDIO_PATH_EARPIECE              1
#define BB_AUDIO_PATH_SPEAKER               2
#define BB_AUDIO_PATH_HEADSET               3
#define BB_AUDIO_PATH_BLUETOOTH             4
#define BB_AUDIO_PATH_CARKIT                5
#define BB_AUDIO_PATH_LINE_OUT              6
#define BB_AUDIO_PATH_DIGITAL                7

/* Audio Directions */
#define BB_AUDIO_DIR_PLAYBACK              0x01     /* Output (speaker) */
#define BB_AUDIO_DIR_CAPTURE               0x02     /* Input (microphone) */
#define BB_AUDIO_DIR_DUPLEX                0x03     /* Both */

/* Audio States */
#define BB_AUDIO_STATE_IDLE                 0
#define BB_AUDIO_STATE_INITIALIZING         1
#define BB_AUDIO_STATE_ACTIVE               2
#define BB_AUDIO_STATE_MUTED                 3
#define BB_AUDIO_STATE_SUSPENDED             4
#define BB_AUDIO_STATE_ERROR                 5

/* Codec Modes */
#define BB_CODEC_MODE_NB                    0      /* Narrowband (8kHz) */
#define BB_CODEC_MODE_WB                    1      /* Wideband (16kHz) */
#define BB_CODEC_MODE_SWB                    2      /* Super-wideband (32kHz) */
#define BB_CODEC_MODE_FB                     3      /* Fullband (48kHz) */

/* DTMF Tones */
#define BB_DTMF_0                          0
#define BB_DTMF_1                          1
#define BB_DTMF_2                          2
#define BB_DTMF_3                          3
#define BB_DTMF_4                          4
#define BB_DTMF_5                          5
#define BB_DTMF_6                          6
#define BB_DTMF_7                          7
#define BB_DTMF_8                          8
#define BB_DTMF_9                          9
#define BB_DTMF_STAR                        10
#define BB_DTMF_POUND                       11
#define BB_DTMF_A                           12
#define BB_DTMF_B                           13
#define BB_DTMF_C                           14
#define BB_DTMF_D                           15

/* DTMF Durations (ms) */
#define BB_DTMF_DURATION_SHORT             80
#define BB_DTMF_DURATION_MEDIUM             150
#define BB_DTMF_DURATION_LONG               300

/* Echo Cancellation Modes */
#define BB_EC_MODE_OFF                      0
#define BB_EC_MODE_STANDARD                  1
#define BB_EC_MODE_AGGRESSIVE                2
#define BB_EC_MODE_HANDS_FREE                3

/* Noise Suppression Levels */
#define BB_NS_LEVEL_OFF                     0
#define BB_NS_LEVEL_LOW                      1
#define BB_NS_LEVEL_MEDIUM                   2
#define BB_NS_LEVEL_HIGH                     3

/* Audio Effects */
#define BB_EFFECT_NONE                      0
#define BB_EFFECT_VOICE_ENHANCEMENT          1
#define BB_EFFECT_WIND_NOISE_REDUCTION       2
#define BB_EFFECT_MUSIC_MODE                 3
#define BB_EFFECT_THEATER_MODE               4

/* Audio Buffer Sizes */
#define BB_AUDIO_BUFFER_SIZE_10MS           80      /* 10ms at 8kHz 16-bit mono */
#define BB_AUDIO_BUFFER_SIZE_20MS           160     /* 20ms at 8kHz 16-bit mono */
#define BB_AUDIO_BUFFER_SIZE_40MS           320     /* 40ms at 8kHz 16-bit mono */
#define BB_AUDIO_BUFFER_SIZE_60MS           480     /* 60ms at 8kHz 16-bit mono */
#define BB_AUDIO_MAX_BUFFERS                 8

/*==============================================================================
 * Baseband Audio Error Codes
 *==============================================================================*/

#define BB_AUDIO_SUCCESS                    0
#define BB_AUDIO_ERR_GENERAL                 -1
#define BB_AUDIO_ERR_NOT_READY                -2
#define BB_AUDIO_ERR_INVALID_PARAM            -3
#define BB_AUDIO_ERR_NO_MEMORY                -4
#define BB_AUDIO_ERR_BUSY                      -5
#define BB_AUDIO_ERR_TIMEOUT                   -6
#define BB_AUDIO_ERR_HARDWARE                  -7
#define BB_AUDIO_ERR_CODEC                      -8
#define BB_AUDIO_ERR_PATH                       -9
#define BB_AUDIO_ERR_FORMAT                     -10

/*==============================================================================
 * Baseband Audio Registers (Hardware Interface)
 *==============================================================================*/

struct baseband_audio_registers {
    uint32_t    bb_audio_id;                 /* Audio interface ID */
    uint32_t    bb_audio_rev;                 /* Audio revision */
    uint32_t    bb_audio_control;              /* Audio control */
    uint32_t    bb_audio_status;               /* Audio status */
    uint32_t    bb_audio_int_status;           /* Interrupt status */
    uint32_t    bb_audio_int_enable;           /* Interrupt enable */
    uint32_t    bb_audio_int_clear;            /* Interrupt clear */
    uint32_t    bb_audio_format;                /* Audio format */
    uint32_t    bb_audio_sample_rate;           /* Sample rate */
    uint32_t    bb_audio_channels;              /* Channel config */
    uint32_t    bb_audio_bit_depth;             /* Bit depth */
    uint32_t    bb_audio_volume_main;           /* Main volume */
    uint32_t    bb_audio_volume_headset;        /* Headset volume */
    uint32_t    bb_audio_volume_speaker;        /* Speaker volume */
    uint32_t    bb_audio_volume_mic;            /* Microphone gain */
    uint32_t    bb_audio_mute;                   /* Mute control */
    uint32_t    bb_audio_path_select;            /* Audio path select */
    uint32_t    bb_audio_ec_control;             /* Echo cancellation */
    uint32_t    bb_audio_ns_control;             /* Noise suppression */
    uint32_t    bb_audio_side_tone;              /* Side tone level */
    uint32_t    bb_audio_dtmf_control;           /* DTMF control */
    uint32_t    bb_audio_dtmf_freq_high;         /* DTMF high frequency */
    uint32_t    bb_audio_dtmf_freq_low;          /* DTMF low frequency */
    uint32_t    bb_audio_dtmf_duration;          /* DTMF duration */
    uint32_t    bb_audio_tx_fifo;                /* TX FIFO data */
    uint32_t    bb_audio_rx_fifo;                /* RX FIFO data */
    uint32_t    bb_audio_fifo_status;            /* FIFO status */
    uint32_t    bb_audio_dma_addr;                /* DMA address */
    uint32_t    bb_audio_dma_size;                /* DMA size */
    uint32_t    bb_audio_dma_control;             /* DMA control */
    uint32_t    bb_audio_power_state;             /* Power state */
    uint32_t    bb_audio_debug;                   /* Debug register */
};

/* Interrupt bits */
#define BB_AUDIO_INT_TX_READY               (1 << 0)
#define BB_AUDIO_INT_RX_READY               (1 << 1)
#define BB_AUDIO_INT_TX_UNDERRUN             (1 << 2)
#define BB_AUDIO_INT_RX_OVERRUN              (1 << 3)
#define BB_AUDIO_INT_DMA_DONE                (1 << 4)
#define BB_AUDIO_INT_DTMF_DONE               (1 << 5)
#define BB_AUDIO_INT_PATH_CHANGE              (1 << 6)
#define BB_AUDIO_INT_VOLUME_CHANGE            (1 << 7)
#define BB_AUDIO_INT_ERROR                    (1 << 15)

/* Control bits */
#define BB_AUDIO_CTRL_ENABLE                 (1 << 0)
#define BB_AUDIO_CTRL_RESET                   (1 << 1)
#define BB_AUDIO_CTRL_LOOPBACK                (1 << 2)
#define BB_AUDIO_CTRL_SUSPEND                 (1 << 3)
#define BB_AUDIO_CTRL_RESUME                  (1 << 4)

/* Status bits */
#define BB_AUDIO_STATUS_READY                 (1 << 0)
#define BB_AUDIO_STATUS_ACTIVE                (1 << 1)
#define BB_AUDIO_STATUS_MUTED                  (1 << 2)
#define BB_AUDIO_STATUS_ERROR                  (1 << 7)

/*==============================================================================
 * Baseband Audio Buffer Structure
 *==============================================================================*/

struct bb_audio_buffer {
    void*               data;                 /* Buffer data */
    uint32_t            size;                 /* Buffer size */
    uint32_t            length;               /* Valid data length */
    uint32_t            flags;                /* Buffer flags */
    uint64_t            timestamp;             /* Timestamp */
    uint32_t            sequence;              /* Sequence number */
    uint32_t            samples;               /* Number of samples */
    struct bb_audio_buffer* next;              /* Next in queue */
};

/* Buffer flags */
#define BB_BUF_FLAG_VALID                    (1 << 0)
#define BB_BUF_FLAG_SILENCE                   (1 << 1)
#define BB_BUF_FLAG_DTMF                       (1 << 2)
#define BB_BUF_FLAG_TONE                        (1 << 3)
#define BB_BUF_FLAG_CODEC                       (1 << 4)

/*==============================================================================
 * Audio Codec Context
 *==============================================================================*/

struct bb_audio_codec {
    uint32_t            type;                 /* Codec type */
    uint32_t            mode;                 /* Codec mode (NB/WB/SWB/FB) */
    uint32_t            bitrate;              /* Bitrate in bps */
    uint32_t            frame_size;           /* Frame size in bytes */
    uint32_t            frame_duration;       /* Frame duration in ms */
    void*               codec_data;           /* Codec private data */
    
    /* Codec operations */
    int (*encode)(struct bb_audio_codec* codec, void* input, void* output, uint32_t* out_len);
    int (*decode)(struct bb_audio_codec* codec, void* input, uint32_t in_len, void* output);
    int (*reset)(struct bb_audio_codec* codec);
    void (*free)(struct bb_audio_codec* codec);
};

/*==============================================================================
 * Audio Path Configuration
 *==============================================================================*/

struct bb_audio_path {
    uint32_t            path_id;              /* Path identifier */
    uint32_t            direction;            /* Playback/capture/duplex */
    uint32_t            volume_min;           /* Minimum volume */
    uint32_t            volume_max;           /* Maximum volume */
    uint32_t            volume_default;       /* Default volume */
    uint32_t            current_volume;       /* Current volume */
    uint32_t            mute;                  /* Mute state */
    uint32_t            gain_min;              /* Minimum gain (mic) */
    uint32_t            gain_max;              /* Maximum gain (mic) */
    uint32_t            current_gain;          /* Current gain (mic) */
    uint32_t            ec_enabled;            /* Echo cancellation */
    uint32_t            ns_enabled;            /* Noise suppression */
    uint32_t            side_tone_level;       /* Side tone level */
};

/*==============================================================================
 * DTMF Tone Generator
 *==============================================================================*/

struct bb_dtmf_tone {
    uint32_t            digit;                 /* DTMF digit */
    uint32_t            high_freq;             /* High frequency (Hz) */
    uint32_t            low_freq;              /* Low frequency (Hz) */
    uint32_t            duration;              /* Duration in ms */
    uint32_t            pause;                 /* Pause after tone */
    uint32_t            amplitude;             /* Amplitude (0-100) */
};

/* DTMF frequency pairs */
static const struct bb_dtmf_tone bb_dtmf_table[] = {
    {BB_DTMF_0,     1336, 941,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_1,     1209, 697,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_2,     1336, 697,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_3,     1477, 697,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_4,     1209, 770,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_5,     1336, 770,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_6,     1477, 770,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_7,     1209, 852,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_8,     1336, 852,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_9,     1477, 852,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_STAR,  1209, 941,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_POUND, 1477, 941,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_A,     1633, 697,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_B,     1633, 770,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_C,     1633, 852,   BB_DTMF_DURATION_MEDIUM, 50, 80},
    {BB_DTMF_D,     1633, 941,   BB_DTMF_DURATION_MEDIUM, 50, 80}
};

/*==============================================================================
 * Main Baseband Audio Device Structure
 *==============================================================================*/

struct apple_baseband_audio {
    uint32_t            magic;                /* Magic number */
#define BB_AUDIO_MAGIC          0x41554449    /* "AUDI" */
    
    /* Hardware interface */
    void*               baseband_dev;         /* Parent baseband device */
    IOMemoryMap*        memory_map;           /* Register memory map */
    volatile uint32_t*  registers;            /* Register base */
    uint64_t            reg_phys;             /* Physical address */
    uint64_t            reg_size;             /* Register size */
    
    /* IOKit integration */
    OSObject*           provider;             /* IOKit provider */
    IOWorkLoop*         work_loop;            /* Work loop */
    IOInterruptSource*  interrupt_source;     /* Interrupt source */
    IOCommandGate*      command_gate;         /* Command gate */
    
    /* Audio state */
    uint32_t            state;                 /* Current state */
    uint32_t            format;                 /* Audio format */
    uint32_t            sample_rate;            /* Sample rate */
    uint32_t            channels;               /* Number of channels */
    uint32_t            bit_depth;              /* Bit depth */
    uint32_t            direction;              /* Direction flags */
    
    /* Path management */
    struct bb_audio_path paths[8];              /* Audio paths */
    uint32_t            current_path;           /* Current active path */
    uint32_t            num_paths;              /* Number of paths */
    
    /* Volume control */
    uint32_t            main_volume;            /* Main volume */
    uint32_t            speaker_volume;         /* Speaker volume */
    uint32_t            headset_volume;         /* Headset volume */
    uint32_t            mic_gain;                /* Microphone gain */
    uint32_t            mute;                    /* Master mute */
    
    /* Audio processing */
    uint32_t            ec_mode;                 /* Echo cancellation */
    uint32_t            ns_level;                /* Noise suppression */
    uint32_t            side_tone;               /* Side tone level */
    uint32_t            effects;                 /* Audio effects */
    
    /* Codec management */
    struct bb_audio_codec* tx_codec;            /* Transmit codec */
    struct bb_audio_codec* rx_codec;            /* Receive codec */
    lck_mtx_t*          codec_lock;              /* Codec lock */
    
    /* Buffer management */
    struct bb_audio_buffer* tx_buffers;          /* TX buffer pool */
    struct bb_audio_buffer* rx_buffers;          /* RX buffer pool */
    struct bb_audio_buffer* tx_queue;            /* TX queue */
    struct bb_audio_buffer* rx_queue;            /* RX queue */
    lck_mtx_t*          buffer_lock;             /* Buffer lock */
    uint32_t            tx_buffer_count;         /* TX buffer count */
    uint32_t            rx_buffer_count;         /* RX buffer count */
    zone_t              buffer_zone;             /* Buffer zone */
    
    /* DMA management */
    uint64_t            dma_address;             /* DMA buffer address */
    uint32_t            dma_size;                 /* DMA buffer size */
    uint32_t            dma_position;             /* Current DMA position */
    uint32_t            dma_active;               /* DMA active flag */
    
    /* DTMF generator */
    struct bb_dtmf_tone current_tone;            /* Current DTMF tone */
    uint32_t            dtmf_active;              /* DTMF active */
    uint32_t            dtmf_queue[32];           /* DTMF queue */
    uint32_t            dtmf_head;                /* DTMF queue head */
    uint32_t            dtmf_tail;                /* DTMF queue tail */
    lck_mtx_t*          dtmf_lock;                /* DTMF lock */
    thread_call_t       dtmf_thread;              /* DTMF thread */
    
    /* Statistics */
    uint64_t            tx_bytes;                 /* Transmitted bytes */
    uint64_t            rx_bytes;                 /* Received bytes */
    uint64_t            tx_packets;                /* Transmitted packets */
    uint64_t            rx_packets;                /* Received packets */
    uint64_t            tx_underruns;              /* TX underruns */
    uint64_t            rx_overruns;               /* RX overruns */
    uint64_t            dtmf_count;                /* DTMF tones generated */
    uint64_t            errors;                    /* Error count */
    
    /* Power management */
    uint32_t            power_state;               /* Power state */
    uint64_t            last_activity;             /* Last activity time */
    uint32_t            thermal_level;             /* Thermal level */
    
    /* Locking */
    lck_mtx_t*          audio_lock;                /* Main audio lock */
    
    /* Callback interfaces */
    void*               voice_callback_data;       /* Voice callback data */
    int (*voice_callback)(void* data, void* samples, uint32_t count);
    
    /* Audio queue interface */
    void*               audio_queue;                /* Audio queue reference */
};

/*==============================================================================
 * Global Variables
 *==============================================================================*/

static lck_grp_t* apple_bb_audio_lck_grp;
static lck_attr_t* apple_bb_audio_lck_attr;
static zone_t apple_bb_audio_zone;
static struct apple_baseband_audio* g_bb_audio_devices[4];
static uint32_t g_num_bb_audio_devices = 0;
static lck_mtx_t* g_bb_audio_global_lock;

/*==============================================================================
 * Forward Declarations
 *==============================================================================*/

static int apple_baseband_audio_init_hw(struct apple_baseband_audio* dev);
static int apple_baseband_audio_reset_hw(struct apple_baseband_audio* dev);
static int apple_baseband_audio_start(struct apple_baseband_audio* dev);
static int apple_baseband_audio_stop(struct apple_baseband_audio* dev);
static void apple_baseband_audio_interrupt_handler(struct apple_baseband_audio* dev);
static int apple_baseband_audio_set_path(struct apple_baseband_audio* dev, uint32_t path);
static int apple_baseband_audio_set_volume(struct apple_baseband_audio* dev, uint32_t volume);
static int apple_baseband_audio_set_mic_gain(struct apple_baseband_audio* dev, uint32_t gain);
static int apple_baseband_audio_set_mute(struct apple_baseband_audio* dev, uint32_t mute);
static int apple_baseband_audio_play_dtmf(struct apple_baseband_audio* dev, uint32_t digit, uint32_t duration);
static int apple_baseband_audio_codec_select(struct apple_baseband_audio* dev, uint32_t format, uint32_t sample_rate);

/*==============================================================================
 * Audio Buffer Management
 *==============================================================================*/

static struct bb_audio_buffer*
apple_baseband_audio_alloc_buffer(struct apple_baseband_audio* dev, uint32_t size)
{
    struct bb_audio_buffer* buf;
    
    if (!dev || size == 0 || size > 8192) {
        return NULL;
    }
    
    lck_mtx_lock(dev->buffer_lock);
    
    /* Get from pool */
    buf = dev->tx_buffers;
    if (buf) {
        dev->tx_buffers = buf->next;
        lck_mtx_unlock(dev->buffer_lock);
        
        /* Allocate data */
        buf->data = kalloc(size);
        if (!buf->data) {
            /* Return to pool */
            lck_mtx_lock(dev->buffer_lock);
            buf->next = dev->tx_buffers;
            dev->tx_buffers = buf;
            lck_mtx_unlock(dev->buffer_lock);
            return NULL;
        }
        
        buf->size = size;
        buf->length = 0;
        buf->flags = 0;
        buf->timestamp = 0;
        buf->sequence = 0;
        buf->samples = 0;
        buf->next = NULL;
        
        return buf;
    }
    
    lck_mtx_unlock(dev->buffer_lock);
    
    /* Pool empty, allocate new */
    buf = (struct bb_audio_buffer*)kalloc(sizeof(struct bb_audio_buffer));
    if (!buf) {
        return NULL;
    }
    
    buf->data = kalloc(size);
    if (!buf->data) {
        kfree(buf, sizeof(struct bb_audio_buffer));
        return NULL;
    }
    
    buf->size = size;
    buf->length = 0;
    buf->flags = 0;
    buf->timestamp = 0;
    buf->sequence = 0;
    buf->samples = 0;
    buf->next = NULL;
    
    return buf;
}

static void
apple_baseband_audio_free_buffer(struct apple_baseband_audio* dev, struct bb_audio_buffer* buf)
{
    if (!dev || !buf) {
        return;
    }
    
    if (buf->data) {
        kfree(buf->data, buf->size);
        buf->data = NULL;
    }
    
    lck_mtx_lock(dev->buffer_lock);
    buf->next = dev->tx_buffers;
    dev->tx_buffers = buf;
    lck_mtx_unlock(dev->buffer_lock);
}

static int
apple_baseband_audio_queue_tx(struct apple_baseband_audio* dev, void* data, uint32_t length)
{
    struct bb_audio_buffer* buf;
    struct bb_audio_buffer* last;
    
    if (!dev || !data || length == 0) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    /* Allocate buffer */
    buf = apple_baseband_audio_alloc_buffer(dev, length);
    if (!buf) {
        return BB_AUDIO_ERR_NO_MEMORY;
    }
    
    /* Copy data */
    memcpy(buf->data, data, length);
    buf->length = length;
    buf->samples = length / ((dev->bit_depth / 8) * dev->channels);
    clock_get_uptime(&buf->timestamp);
    
    /* Queue buffer */
    lck_mtx_lock(dev->buffer_lock);
    
    if (!dev->tx_queue) {
        dev->tx_queue = buf;
    } else {
        last = dev->tx_queue;
        while (last->next) {
            last = last->next;
        }
        last->next = buf;
    }
    
    dev->tx_buffer_count++;
    
    lck_mtx_unlock(dev->buffer_lock);
    
    /* Trigger TX if needed */
    if (dev->registers) {
        dev->registers[12] = 1;  /* Trigger TX */
    }
    
    return BB_AUDIO_SUCCESS;
}

static int
apple_baseband_audio_dequeue_rx(struct apple_baseband_audio* dev, void* data, uint32_t* length)
{
    struct bb_audio_buffer* buf;
    
    if (!dev || !data || !length) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->buffer_lock);
    
    buf = dev->rx_queue;
    if (!buf) {
        lck_mtx_unlock(dev->buffer_lock);
        *length = 0;
        return BB_AUDIO_SUCCESS;
    }
    
    dev->rx_queue = buf->next;
    dev->rx_buffer_count--;
    
    lck_mtx_unlock(dev->buffer_lock);
    
    /* Copy data */
    *length = (buf->length < *length) ? buf->length : *length;
    memcpy(data, buf->data, *length);
    
    /* Free buffer */
    apple_baseband_audio_free_buffer(dev, buf);
    
    return BB_AUDIO_SUCCESS;
}

/*==============================================================================
 * DTMF Tone Generation
 *==============================================================================*/

static void
apple_baseband_audio_generate_dtmf_tone(struct apple_baseband_audio* dev, 
                                         const struct bb_dtmf_tone* tone)
{
    uint32_t sample_rate = dev->sample_rate;
    uint32_t duration_samples = (tone->duration * sample_rate) / 1000;
    uint32_t pause_samples = (tone->pause * sample_rate) / 1000;
    uint32_t total_samples = duration_samples + pause_samples;
    
    /* Allocate buffer for tone */
    uint32_t buffer_size = total_samples * (dev->bit_depth / 8) * dev->channels;
    struct bb_audio_buffer* buf = apple_baseband_audio_alloc_buffer(dev, buffer_size);
    
    if (!buf) {
        return;
    }
    
    /* Generate sine waves for both frequencies */
    float high_freq = (float)tone->high_freq;
    float low_freq = (float)tone->low_freq;
    float amplitude = (float)tone->amplitude / 100.0f;
    
    int16_t* samples = (int16_t*)buf->data;
    uint32_t i;
    
    for (i = 0; i < duration_samples; i++) {
        float t = (float)i / (float)sample_rate;
        float sample = amplitude * 32767.0f * 0.5f * (
            sinf(2.0f * M_PI * high_freq * t) + 
            sinf(2.0f * M_PI * low_freq * t)
        );
        
        int16_t val = (int16_t)sample;
        
        /* Mono or stereo */
        if (dev->channels == 1) {
            samples[i] = val;
        } else {
            samples[i * 2] = val;
            samples[i * 2 + 1] = val;
        }
    }
    
    /* Add pause (silence) */
    for (; i < total_samples; i++) {
        if (dev->channels == 1) {
            samples[i] = 0;
        } else {
            samples[i * 2] = 0;
            samples[i * 2 + 1] = 0;
        }
    }
    
    buf->length = buffer_size;
    buf->samples = total_samples;
    buf->flags = BB_BUF_FLAG_DTMF;
    
    /* Queue the tone */
    lck_mtx_lock(dev->buffer_lock);
    
    if (!dev->tx_queue) {
        dev->tx_queue = buf;
    } else {
        struct bb_audio_buffer* last = dev->tx_queue;
        while (last->next) {
            last = last->next;
        }
        last->next = buf;
    }
    
    dev->tx_buffer_count++;
    dev->dtmf_count++;
    
    lck_mtx_unlock(dev->buffer_lock);
}

static void
apple_baseband_audio_dtmf_thread(thread_call_param_t param0, thread_call_param_t param1)
{
    struct apple_baseband_audio* dev = (struct apple_baseband_audio*)param0;
    uint32_t digit;
    
    if (!dev) {
        return;
    }
    
    lck_mtx_lock(dev->dtmf_lock);
    
    /* Process DTMF queue */
    while (dev->dtmf_head != dev->dtmf_tail) {
        digit = dev->dtmf_queue[dev->dtmf_head];
        dev->dtmf_head = (dev->dtmf_head + 1) % 32;
        
        lck_mtx_unlock(dev->dtmf_lock);
        
        /* Generate tone */
        if (digit < sizeof(bb_dtmf_table) / sizeof(bb_dtmf_table[0])) {
            apple_baseband_audio_generate_dtmf_tone(dev, &bb_dtmf_table[digit]);
        }
        
        lck_mtx_lock(dev->dtmf_lock);
    }
    
    dev->dtmf_active = 0;
    
    lck_mtx_unlock(dev->dtmf_lock);
}

static int
apple_baseband_audio_play_dtmf(struct apple_baseband_audio* dev, uint32_t digit, uint32_t duration)
{
    if (!dev || digit > BB_DTMF_D) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->dtmf_lock);
    
    /* Check if queue is full */
    uint32_t next_tail = (dev->dtmf_tail + 1) % 32;
    if (next_tail == dev->dtmf_head) {
        lck_mtx_unlock(dev->dtmf_lock);
        return BB_AUDIO_ERR_BUSY;
    }
    
    /* Queue DTMF digit */
    dev->dtmf_queue[dev->dtmf_tail] = digit;
    dev->dtmf_tail = next_tail;
    
    /* Start processing if not already active */
    if (!dev->dtmf_active) {
        dev->dtmf_active = 1;
        thread_call_enter(dev->dtmf_thread);
    }
    
    lck_mtx_unlock(dev->dtmf_lock);
    
    return BB_AUDIO_SUCCESS;
}

/*==============================================================================
 * Codec Management
 *==============================================================================*/

/* AMR-NB Codec (simplified implementation) */
static int
amr_nb_encode(struct bb_audio_codec* codec, void* input, void* output, uint32_t* out_len)
{
    /* In real implementation, this would call DSP or hardware codec */
    /* For simulation, just copy with compression header */
    
    uint8_t* in = (uint8_t*)input;
    uint8_t* out = (uint8_t*)output;
    uint32_t i;
    
    /* AMR header (12.2 kbps mode) */
    out[0] = 0x3C;  /* Frame type indicator */
    
    /* Simplified compression - just copy every other sample */
    for (i = 0; i < 160; i += 2) {
        out[1 + i/2] = in[i];
    }
    
    *out_len = 81;  /* 1 header + 80 bytes compressed */
    
    return BB_AUDIO_SUCCESS;
}

static int
amr_nb_decode(struct bb_audio_codec* codec, void* input, uint32_t in_len, void* output)
{
    uint8_t* in = (uint8_t*)input;
    uint8_t* out = (uint8_t*)output;
    uint32_t i;
    
    /* Check header */
    if ((in[0] & 0xFC) != 0x3C) {
        return BB_AUDIO_ERR_CODEC;
    }
    
    /* Simplified decompression */
    for (i = 0; i < 160; i++) {
        out[i] = in[1 + i/2];
    }
    
    return BB_AUDIO_SUCCESS;
}

/* PCM passthrough */
static int
pcm_encode(struct bb_audio_codec* codec, void* input, void* output, uint32_t* out_len)
{
    memcpy(output, input, *out_len);
    return BB_AUDIO_SUCCESS;
}

static int
pcm_decode(struct bb_audio_codec* codec, void* input, uint32_t in_len, void* output)
{
    memcpy(output, input, in_len);
    return BB_AUDIO_SUCCESS;
}

static struct bb_audio_codec*
apple_baseband_audio_codec_create(uint32_t format, uint32_t sample_rate)
{
    struct bb_audio_codec* codec;
    
    codec = (struct bb_audio_codec*)kalloc(sizeof(struct bb_audio_codec));
    if (!codec) {
        return NULL;
    }
    
    codec->type = format;
    codec->codec_data = NULL;
    
    switch (format) {
        case BB_AUDIO_FORMAT_PCM:
            codec->encode = pcm_encode;
            codec->decode = pcm_decode;
            codec->frame_size = 320;  /* 20ms at 16kHz */
            codec->frame_duration = 20;
            codec->bitrate = sample_rate * 16;
            break;
            
        case BB_AUDIO_FORMAT_AMR:
            codec->encode = amr_nb_encode;
            codec->decode = amr_nb_decode;
            codec->frame_size = 160;  /* 20ms at 8kHz */
            codec->frame_duration = 20;
            codec->bitrate = 12200;   /* 12.2 kbps */
            break;
            
        case BB_AUDIO_FORMAT_AMR_WB:
            /* Similar but for wideband */
            codec->frame_size = 320;  /* 20ms at 16kHz */
            codec->frame_duration = 20;
            codec->bitrate = 23850;   /* 23.85 kbps */
            break;
            
        default:
            kfree(codec, sizeof(struct bb_audio_codec));
            return NULL;
    }
    
    codec->mode = (sample_rate <= 8000) ? BB_CODEC_MODE_NB :
                  (sample_rate <= 16000) ? BB_CODEC_MODE_WB :
                  (sample_rate <= 32000) ? BB_CODEC_MODE_SWB :
                  BB_CODEC_MODE_FB;
    
    codec->reset = NULL;
    codec->free = NULL;
    
    return codec;
}

static void
apple_baseband_audio_codec_free(struct bb_audio_codec* codec)
{
    if (!codec) {
        return;
    }
    
    if (codec->free) {
        codec->free(codec);
    }
    
    if (codec->codec_data) {
        kfree(codec->codec_data, 0);  /* Size unknown in this example */
    }
    
    kfree(codec, sizeof(struct bb_audio_codec));
}

static int
apple_baseband_audio_codec_select(struct apple_baseband_audio* dev, 
                                   uint32_t format, uint32_t sample_rate)
{
    struct bb_audio_codec* new_tx_codec;
    struct bb_audio_codec* new_rx_codec;
    
    if (!dev) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->codec_lock);
    
    /* Create new codecs */
    new_tx_codec = apple_baseband_audio_codec_create(format, sample_rate);
    if (!new_tx_codec) {
        lck_mtx_unlock(dev->codec_lock);
        return BB_AUDIO_ERR_NO_MEMORY;
    }
    
    new_rx_codec = apple_baseband_audio_codec_create(format, sample_rate);
    if (!new_rx_codec) {
        apple_baseband_audio_codec_free(new_tx_codec);
        lck_mtx_unlock(dev->codec_lock);
        return BB_AUDIO_ERR_NO_MEMORY;
    }
    
    /* Replace old codecs */
    if (dev->tx_codec) {
        apple_baseband_audio_codec_free(dev->tx_codec);
    }
    if (dev->rx_codec) {
        apple_baseband_audio_codec_free(dev->rx_codec);
    }
    
    dev->tx_codec = new_tx_codec;
    dev->rx_codec = new_rx_codec;
    dev->format = format;
    dev->sample_rate = sample_rate;
    
    lck_mtx_unlock(dev->codec_lock);
    
    return BB_AUDIO_SUCCESS;
}

/*==============================================================================
 * Audio Path Management
 *==============================================================================*/

static int
apple_baseband_audio_set_path(struct apple_baseband_audio* dev, uint32_t path)
{
    int i;
    
    if (!dev) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    /* Find path */
    for (i = 0; i < dev->num_paths; i++) {
        if (dev->paths[i].path_id == path) {
            break;
        }
    }
    
    if (i >= dev->num_paths) {
        return BB_AUDIO_ERR_PATH;
    }
    
    lck_mtx_lock(dev->audio_lock);
    
    /* Update hardware */
    if (dev->registers) {
        dev->registers[16] = path;  /* Path select register */
    }
    
    dev->current_path = path;
    
    /* Apply path-specific settings */
    if (dev->paths[i].direction & BB_AUDIO_DIR_PLAYBACK) {
        dev->registers[13] = dev->paths[i].current_volume;  /* Volume */
    }
    
    if (dev->paths[i].direction & BB_AUDIO_DIR_CAPTURE) {
        dev->registers[15] = dev->paths[i].current_gain;    /* Mic gain */
    }
    
    lck_mtx_unlock(dev->audio_lock);
    
    return BB_AUDIO_SUCCESS;
}

static int
apple_baseband_audio_set_volume(struct apple_baseband_audio* dev, uint32_t volume)
{
    struct bb_audio_path* path;
    
    if (!dev) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->audio_lock);
    
    /* Find current path */
    path = NULL;
    for (int i = 0; i < dev->num_paths; i++) {
        if (dev->paths[i].path_id == dev->current_path) {
            path = &dev->paths[i];
            break;
        }
    }
    
    if (!path) {
        lck_mtx_unlock(dev->audio_lock);
        return BB_AUDIO_ERR_PATH;
    }
    
    /* Clamp volume */
    if (volume < path->volume_min) volume = path->volume_min;
    if (volume > path->volume_max) volume = path->volume_max;
    
    /* Update hardware */
    if (dev->registers) {
        dev->registers[13] = volume;  /* Volume register */
    }
    
    path->current_volume = volume;
    dev->main_volume = volume;
    
    lck_mtx_unlock(dev->audio_lock);
    
    return BB_AUDIO_SUCCESS;
}

static int
apple_baseband_audio_set_mic_gain(struct apple_baseband_audio* dev, uint32_t gain)
{
    struct bb_audio_path* path;
    
    if (!dev) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->audio_lock);
    
    /* Find current path */
    path = NULL;
    for (int i = 0; i < dev->num_paths; i++) {
        if (dev->paths[i].path_id == dev->current_path) {
            path = &dev->paths[i];
            break;
        }
    }
    
    if (!path) {
        lck_mtx_unlock(dev->audio_lock);
        return BB_AUDIO_ERR_PATH;
    }
    
    /* Clamp gain */
    if (gain < path->gain_min) gain = path->gain_min;
    if (gain > path->gain_max) gain = path->gain_max;
    
    /* Update hardware */
    if (dev->registers) {
        dev->registers[15] = gain;  /* Mic gain register */
    }
    
    path->current_gain = gain;
    dev->mic_gain = gain;
    
    lck_mtx_unlock(dev->audio_lock);
    
    return BB_AUDIO_SUCCESS;
}

static int
apple_baseband_audio_set_mute(struct apple_baseband_audio* dev, uint32_t mute)
{
    if (!dev) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->audio_lock);
    
    /* Update hardware */
    if (dev->registers) {
        if (mute) {
            dev->registers[17] |= 1;   /* Set mute */
        } else {
            dev->registers[17] &= ~1;  /* Clear mute */
        }
    }
    
    dev->mute = mute;
    
    lck_mtx_unlock(dev->audio_lock);
    
    return BB_AUDIO_SUCCESS;
}

/*==============================================================================
 * Audio Processing Controls
 *==============================================================================*/

static int
apple_baseband_audio_set_echo_cancellation(struct apple_baseband_audio* dev, uint32_t mode)
{
    if (!dev || mode > BB_EC_MODE_HANDS_FREE) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->audio_lock);
    
    if (dev->registers) {
        dev->registers[18] = mode;  /* EC control register */
    }
    
    dev->ec_mode = mode;
    
    lck_mtx_unlock(dev->audio_lock);
    
    return BB_AUDIO_SUCCESS;
}

static int
apple_baseband_audio_set_noise_suppression(struct apple_baseband_audio* dev, uint32_t level)
{
    if (!dev || level > BB_NS_LEVEL_HIGH) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->audio_lock);
    
    if (dev->registers) {
        dev->registers[19] = level;  /* NS control register */
    }
    
    dev->ns_level = level;
    
    lck_mtx_unlock(dev->audio_lock);
    
    return BB_AUDIO_SUCCESS;
}

static int
apple_baseband_audio_set_side_tone(struct apple_baseband_audio* dev, uint32_t level)
{
    if (!dev || level > 100) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->audio_lock);
    
    if (dev->registers) {
        dev->registers[20] = level;  /* Side tone register */
    }
    
    dev->side_tone = level;
    
    lck_mtx_unlock(dev->audio_lock);
    
    return BB_AUDIO_SUCCESS;
}

/*==============================================================================
 * Hardware Interrupt Handler
 *==============================================================================*/

static void
apple_baseband_audio_interrupt_handler(struct apple_baseband_audio* dev)
{
    uint32_t int_status;
    
    if (!dev || !dev->registers) {
        return;
    }
    
    /* Read interrupt status */
    int_status = dev->registers[6];  /* Interrupt status register */
    
    /* Clear interrupts */
    dev->registers[8] = int_status;  /* Interrupt clear register */
    
    /* Handle TX ready */
    if (int_status & BB_AUDIO_INT_TX_READY) {
        /* Process TX queue */
        lck_mtx_lock(dev->buffer_lock);
        
        if (dev->tx_queue) {
            struct bb_audio_buffer* buf = dev->tx_queue;
            dev->tx_queue = buf->next;
            dev->tx_buffer_count--;
            
            lck_mtx_unlock(dev->buffer_lock);
            
            /* Write to TX FIFO */
            uint32_t* fifo = (uint32_t*)(dev->registers + 24);
            uint32_t words = buf->length / 4;
            uint32_t* data = (uint32_t*)buf->data;
            
            for (uint32_t i = 0; i < words; i++) {
                *fifo = data[i];
            }
            
            /* Update statistics */
            dev->tx_bytes += buf->length;
            dev->tx_packets++;
            
            /* Free buffer */
            apple_baseband_audio_free_buffer(dev, buf);
        }
        
        lck_mtx_unlock(dev->buffer_lock);
    }
    
    /* Handle RX ready */
    if (int_status & BB_AUDIO_INT_RX_READY) {
        /* Read from RX FIFO */
        uint32_t* fifo = (uint32_t*)(dev->registers + 25);
        uint32_t words = 80;  /* Typical RX buffer size in words */
        
        struct bb_audio_buffer* buf = apple_baseband_audio_alloc_buffer(dev, words * 4);
        if (buf) {
            uint32_t* data = (uint32_t*)buf->data;
            
            for (uint32_t i = 0; i < words; i++) {
                data[i] = *fifo;
            }
            
            buf->length = words * 4;
            buf->samples = words;
            
            /* Queue for processing */
            lck_mtx_lock(dev->buffer_lock);
            
            if (!dev->rx_queue) {
                dev->rx_queue = buf;
            } else {
                struct bb_audio_buffer* last = dev->rx_queue;
                while (last->next) {
                    last = last->next;
                }
                last->next = buf;
            }
            
            dev->rx_buffer_count++;
            dev->rx_bytes += buf->length;
            dev->rx_packets++;
            
            lck_mtx_unlock(dev->buffer_lock);
        }
    }
    
    /* Handle underrun */
    if (int_status & BB_AUDIO_INT_TX_UNDERRUN) {
        dev->tx_underruns++;
    }
    
    /* Handle overrun */
    if (int_status & BB_AUDIO_INT_RX_OVERRUN) {
        dev->rx_overruns++;
    }
    
    /* Handle error */
    if (int_status & BB_AUDIO_INT_ERROR) {
        dev->errors++;
    }
}

/*==============================================================================
 * Hardware Initialization
 *==============================================================================*/

static int
apple_baseband_audio_init_hw(struct apple_baseband_audio* dev)
{
    if (!dev || !dev->registers) {
        return BB_AUDIO_ERR_NOT_READY;
    }
    
    /* Reset audio controller */
    dev->registers[4] = BB_AUDIO_CTRL_RESET;  /* Control register */
    delay(10000);  /* 10ms delay */
    
    /* Configure audio format */
    dev->registers[9] = dev->format;           /* Format register */
    dev->registers[10] = dev->sample_rate;     /* Sample rate */
    dev->registers[11] = dev->channels;        /* Channels */
    dev->registers[12] = dev->bit_depth;       /* Bit depth */
    
    /* Configure interrupts */
    dev->registers[7] = BB_AUDIO_INT_TX_READY | BB_AUDIO_INT_RX_READY |
                        BB_AUDIO_INT_TX_UNDERRUN | BB_AUDIO_INT_RX_OVERRUN |
                        BB_AUDIO_INT_ERROR;
    
    /* Enable audio */
    dev->registers[4] |= BB_AUDIO_CTRL_ENABLE;
    
    return BB_AUDIO_SUCCESS;
}

static int
apple_baseband_audio_reset_hw(struct apple_baseband_audio* dev)
{
    if (!dev || !dev->registers) {
        return BB_AUDIO_ERR_NOT_READY;
    }
    
    /* Disable audio */
    dev->registers[4] &= ~BB_AUDIO_CTRL_ENABLE;
    
    /* Reset */
    dev->registers[4] = BB_AUDIO_CTRL_RESET;
    delay(10000);
    
    /* Re-initialize */
    return apple_baseband_audio_init_hw(dev);
}

/*==============================================================================
 * Start/Stop Audio
 *==============================================================================*/

static int
apple_baseband_audio_start(struct apple_baseband_audio* dev)
{
    if (!dev) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->audio_lock);
    
    if (dev->state != BB_AUDIO_STATE_IDLE) {
        lck_mtx_unlock(dev->audio_lock);
        return BB_AUDIO_ERR_BUSY;
    }
    
    /* Initialize hardware */
    int ret = apple_baseband_audio_init_hw(dev);
    if (ret != BB_AUDIO_SUCCESS) {
        lck_mtx_unlock(dev->audio_lock);
        return ret;
    }
    
    dev->state = BB_AUDIO_STATE_ACTIVE;
    dev->last_activity = mach_absolute_time();
    
    lck_mtx_unlock(dev->audio_lock);
    
    return BB_AUDIO_SUCCESS;
}

static int
apple_baseband_audio_stop(struct apple_baseband_audio* dev)
{
    if (!dev) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->audio_lock);
    
    /* Disable audio */
    if (dev->registers) {
        dev->registers[4] &= ~BB_AUDIO_CTRL_ENABLE;
    }
    
    /* Clear queues */
    lck_mtx_lock(dev->buffer_lock);
    
    while (dev->tx_queue) {
        struct bb_audio_buffer* buf = dev->tx_queue;
        dev->tx_queue = buf->next;
        apple_baseband_audio_free_buffer(dev, buf);
    }
    
    while (dev->rx_queue) {
        struct bb_audio_buffer* buf = dev->rx_queue;
        dev->rx_queue = buf->next;
        apple_baseband_audio_free_buffer(dev, buf);
    }
    
    dev->tx_buffer_count = 0;
    dev->rx_buffer_count = 0;
    
    lck_mtx_unlock(dev->buffer_lock);
    
    dev->state = BB_AUDIO_STATE_IDLE;
    
    lck_mtx_unlock(dev->audio_lock);
    
    return BB_AUDIO_SUCCESS;
}

/*==============================================================================
 * Voice Call Audio Processing
 *==============================================================================*/

int
apple_baseband_audio_start_voice_call(struct apple_baseband_audio* dev,
                                       uint32_t codec_type,
                                       uint32_t sample_rate)
{
    if (!dev) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    /* Select appropriate codec for voice call */
    int ret = apple_baseband_audio_codec_select(dev, codec_type, sample_rate);
    if (ret != BB_AUDIO_SUCCESS) {
        return ret;
    }
    
    /* Configure for voice call (duplex) */
    dev->direction = BB_AUDIO_DIR_DUPLEX;
    
    /* Set appropriate audio path (usually earpiece) */
    apple_baseband_audio_set_path(dev, BB_AUDIO_PATH_EARPIECE);
    
    /* Enable echo cancellation for voice calls */
    apple_baseband_audio_set_echo_cancellation(dev, BB_EC_MODE_STANDARD);
    
    /* Enable noise suppression */
    apple_baseband_audio_set_noise_suppression(dev, BB_NS_LEVEL_MEDIUM);
    
    /* Start audio */
    return apple_baseband_audio_start(dev);
}

int
apple_baseband_audio_stop_voice_call(struct apple_baseband_audio* dev)
{
    if (!dev) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    /* Stop audio */
    return apple_baseband_audio_stop(dev);
}

int
apple_baseband_audio_send_voice_frame(struct apple_baseband_audio* dev,
                                       void* pcm_samples,
                                       uint32_t num_samples)
{
    uint8_t encoded[512];
    uint32_t encoded_len = sizeof(encoded);
    int ret;
    
    if (!dev || !pcm_samples) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    lck_mtx_lock(dev->codec_lock);
    
    if (!dev->tx_codec) {
        lck_mtx_unlock(dev->codec_lock);
        return BB_AUDIO_ERR_NOT_READY;
    }
    
    /* Encode PCM using selected codec */
    ret = dev->tx_codec->encode(dev->tx_codec, pcm_samples, encoded, &encoded_len);
    if (ret != BB_AUDIO_SUCCESS) {
        lck_mtx_unlock(dev->codec_lock);
        return ret;
    }
    
    lck_mtx_unlock(dev->codec_lock);
    
    /* Queue encoded frame for transmission to baseband */
    ret = apple_baseband_audio_queue_tx(dev, encoded, encoded_len);
    
    return ret;
}

int
apple_baseband_audio_receive_voice_frame(struct apple_baseband_audio* dev,
                                          void* pcm_samples,
                                          uint32_t* num_samples)
{
    uint8_t encoded[512];
    uint32_t encoded_len = sizeof(encoded);
    int ret;
    
    if (!dev || !pcm_samples) {
        return BB_AUDIO_ERR_INVALID_PARAM;
    }
    
    /* Get encoded frame from RX queue */
    ret = apple_baseband_audio_dequeue_rx(dev, encoded, &encoded_len);
    if (ret != BB_AUDIO_SUCCESS || encoded_len == 0) {
        return ret;
    }
    
    lck_mtx_lock(dev->codec_lock);
    
    if (!dev->rx_codec) {
        lck_mtx_unlock(dev->codec_lock);
        return BB_AUDIO_ERR_NOT_READY;
    }
    
    /* Decode to PCM */
    ret = dev->rx_codec->decode(dev->rx_codec, encoded, encoded_len, pcm_samples);
    if (ret != BB_AUDIO_SUCCESS) {
        lck_mtx_unlock(dev->codec_lock);
        return ret;
    }
    
    /* Calculate number of samples */
    *num_samples = encoded_len * 8 / dev->bit_depth;  /* Simplified */
    
    lck_mtx_unlock(dev->codec_lock);
    
    return BB_AUDIO_SUCCESS;
}

/*==============================================================================
 * Driver Initialization
 *==============================================================================*/

static int
apple_baseband_audio_configure_paths(struct apple_baseband_audio* dev)
{
    /* Earpiece path */
    dev->paths[0].path_id = BB_AUDIO_PATH_EARPIECE;
    dev->paths[0].direction = BB_AUDIO_DIR_DUPLEX;
    dev->paths[0].volume_min = 0;
    dev->paths[0].volume_max = 100;
    dev->paths[0].volume_default = 70;
    dev->paths[0].current_volume = 70;
    dev->paths[0].gain_min = 0;
    dev->paths[0].gain_max = 100;
    dev->paths[0].current_gain = 80;
    
    /* Speaker path */
    dev->paths[1].path_id = BB_AUDIO_PATH_SPEAKER;
    dev->paths[1].direction = BB_AUDIO_DIR_PLAYBACK;
    dev->paths[1].volume_min = 0;
    dev->paths[1].volume_max = 100;
    dev->paths[1].volume_default = 85;
    dev->paths[1].current_volume = 85;
    
    /* Headset path */
    dev->paths[2].path_id = BB_AUDIO_PATH_HEADSET;
    dev->paths[2].direction = BB_AUDIO_DIR_DUPLEX;
    dev->paths[2].volume_min = 0;
    dev->paths[2].volume_max = 100;
    dev->paths[2].volume_default = 60;
    dev->paths[2].current_volume = 60;
    dev->paths[2].gain_min = 0;
    dev->paths[2].gain_max = 100;
    dev->paths[2].current_gain = 70;
    
    dev->num_paths = 3;
    dev->current_path = BB_AUDIO_PATH_EARPIECE;
    
    return BB_AUDIO_SUCCESS;
}

struct apple_baseband_audio*
apple_baseband_audio_create(void* baseband_dev)
{
    struct apple_baseband_audio* dev;
    
    /* Allocate device structure */
    dev = (struct apple_baseband_audio*)kalloc(sizeof(struct apple_baseband_audio));
    if (!dev) {
        return NULL;
    }
    
    bzero(dev, sizeof(struct apple_baseband_audio));
    
    dev->magic = BB_AUDIO_MAGIC;
    dev->baseband_dev = baseband_dev;
    
    /* Initialize locks */
    dev->audio_lock = lck_mtx_alloc_init(apple_bb_audio_lck_grp, apple_bb_audio_lck_attr);
    dev->buffer_lock = lck_mtx_alloc_init(apple_bb_audio_lck_grp, apple_bb_audio_lck_attr);
    dev->codec_lock = lck_mtx_alloc_init(apple_bb_audio_lck_grp, apple_bb_audio_lck_attr);
    dev->dtmf_lock = lck_mtx_alloc_init(apple_bb_audio_lck_grp, apple_bb_audio_lck_attr);
    
    if (!dev->audio_lock || !dev->buffer_lock || !dev->codec_lock || !dev->dtmf_lock) {
        goto error;
    }
    
    /* Create DTMF thread */
    dev->dtmf_thread = thread_call_allocate(
        (thread_call_func_t)apple_baseband_audio_dtmf_thread,
        (thread_call_param_t)dev);
    
    if (!dev->dtmf_thread) {
        goto error;
    }
    
    /* Set default audio parameters */
    dev->format = BB_AUDIO_FORMAT_PCM;
    dev->sample_rate = BB_SAMPLE_RATE_16KHZ;
    dev->channels = BB_AUDIO_CHANNEL_MONO;
    dev->bit_depth = BB_BIT_DEPTH_16;
    dev->state = BB_AUDIO_STATE_IDLE;
    
    /* Configure audio paths */
    apple_baseband_audio_configure_paths(dev);
    
    /* Add to global list */
    lck_mtx_lock(g_bb_audio_global_lock);
    
    if (g_num_bb_audio_devices < 4) {
        g_bb_audio_devices[g_num_bb_audio_devices++] = dev;
    }
    
    lck_mtx_unlock(g_bb_audio_global_lock);
    
    return dev;
    
error:
    if (dev->audio_lock) lck_mtx_free(dev->audio_lock, apple_bb_audio_lck_grp);
    if (dev->buffer_lock) lck_mtx_free(dev->buffer_lock, apple_bb_audio_lck_grp);
    if (dev->codec_lock) lck_mtx_free(dev->codec_lock, apple_bb_audio_lck_grp);
    if (dev->dtmf_lock) lck_mtx_free(dev->dtmf_lock, apple_bb_audio_lck_grp);
    if (dev->dtmf_thread) thread_call_free(dev->dtmf_thread);
    kfree(dev, sizeof(struct apple_baseband_audio));
    return NULL;
}

void
apple_baseband_audio_destroy(struct apple_baseband_audio* dev)
{
    if (!dev || dev->magic != BB_AUDIO_MAGIC) {
        return;
    }
    
    /* Stop audio if active */
    apple_baseband_audio_stop(dev);
    
    /* Remove from global list */
    lck_mtx_lock(g_bb_audio_global_lock);
    
    for (int i = 0; i < g_num_bb_audio_devices; i++) {
        if (g_bb_audio_devices[i] == dev) {
            g_bb_audio_devices[i] = g_bb_audio_devices[--g_num_bb_audio_devices];
            break;
        }
    }
    
    lck_mtx_unlock(g_bb_audio_global_lock);
    
    /* Free resources */
    if (dev->tx_codec) apple_baseband_audio_codec_free(dev->tx_codec);
    if (dev->rx_codec) apple_baseband_audio_codec_free(dev->rx_codec);
    
    lck_mtx_free(dev->audio_lock, apple_bb_audio_lck_grp);
    lck_mtx_free(dev->buffer_lock, apple_bb_audio_lck_grp);
    lck_mtx_free(dev->codec_lock, apple_bb_audio_lck_grp);
    lck_mtx_free(dev->dtmf_lock, apple_bb_audio_lck_grp);
    
    if (dev->dtmf_thread) thread_call_free(dev->dtmf_thread);
    
    dev->magic = 0;
    kfree(dev, sizeof(struct apple_baseband_audio));
}

/*==============================================================================
 * Kernel Module Initialization
 *==============================================================================*/

static int
apple_baseband_audio_module_start(void)
{
    /* Initialize lock group */
    apple_bb_audio_lck_grp = lck_grp_alloc_init("AppleBasebandAudio", LCK_GRP_ATTR_NULL);
    if (!apple_bb_audio_lck_grp) {
        return ENOMEM;
    }
    
    apple_bb_audio_lck_attr = lck_attr_alloc_init();
    if (!apple_bb_audio_lck_attr) {
        lck_grp_free(apple_bb_audio_lck_grp);
        return ENOMEM;
    }
    
    /* Create global lock */
    g_bb_audio_global_lock = lck_mtx_alloc_init(apple_bb_audio_lck_grp, apple_bb_audio_lck_attr);
    if (!g_bb_audio_global_lock) {
        lck_attr_free(apple_bb_audio_lck_attr);
        lck_grp_free(apple_bb_audio_lck_grp);
        return ENOMEM;
    }
    
    printf("AppleBasebandAudio: Module loaded (rev %d)\n", APPLE_BASEBAND_AUDIO_REV);
    
    return 0;
}

static void
apple_baseband_audio_module_stop(void)
{
    /* Clean up any remaining devices */
    lck_mtx_lock(g_bb_audio_global_lock);
    
    for (int i = 0; i < g_num_bb_audio_devices; i++) {
        struct apple_baseband_audio* dev = g_bb_audio_devices[i];
        if (dev) {
            apple_baseband_audio_destroy(dev);
        }
    }
    
    g_num_bb_audio_devices = 0;
    lck_mtx_unlock(g_bb_audio_global_lock);
    
    /* Free locks */
    lck_mtx_free(g_bb_audio_global_lock, apple_bb_audio_lck_grp);
    lck_attr_free(apple_bb_audio_lck_attr);
    lck_grp_free(apple_bb_audio_lck_grp);
    
    printf("AppleBasebandAudio: Module unloaded\n");
}

/* Module entry points */
int
_apple_baseband_audio_start(kmod_info_t* ki, void* data)
{
    return apple_baseband_audio_module_start();
}

int
_apple_baseband_audio_stop(kmod_info_t* ki, void* data)
{
    apple_baseband_audio_module_stop();
    return 0;
}
