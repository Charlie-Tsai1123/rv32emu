/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <portaudio.h>

#include "utils.h"
#include "virtio.h"

#define VSND_DEV_CNT_MAX 1
#define VSND_QUEUE_NUM_MAX 1024
#define VSND_QUEUE_COUNT 4
#define VSND_QUEUE (vsnd->queues[vsnd->queue_sel])

#define VSND_CNFA_FRAME_SZ 2 /* S16 = 2 bytes per sample */
#define VSND_FLUSH_QUEUE 0x4
#define VSND_MAX_PENDING_BUFS 6

enum {
    VSND_QUEUE_CTRL = 0,
    VSND_QUEUE_EVT = 1,
    VSND_QUEUE_TX = 2,
    VSND_QUEUE_RX = 3,
};

enum {
    VSND_FEATURES_0 = 0,
    VSND_FEATURES_1 = 1, /* VIRTIO_F_VERSION_1 */
};

enum {
    /* jack control request types */
    VIRTIO_SND_R_JACK_INFO = 1,

    /* PCM control request types */
    VIRTIO_SND_R_PCM_INFO = 0x0100,
    VIRTIO_SND_R_PCM_SET_PARAMS,
    VIRTIO_SND_R_PCM_PREPARE,
    VIRTIO_SND_R_PCM_RELEASE,
    VIRTIO_SND_R_PCM_START,
    VIRTIO_SND_R_PCM_STOP,

    /* channel map control request types */
    VIRTIO_SND_R_CHMAP_INFO = 0x0200,

    /* jack event types */
    VIRTIO_SND_EVT_JACK_CONNECTED = 0x1000,
    VIRTIO_SND_EVT_JACK_DISCONNECTED,

    /* PCM event types */
    VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED = 0x1100,
    VIRTIO_SND_EVT_PCM_XRUN,

    /* common status codes */
    VIRTIO_SND_S_OK = 0x8000,
    VIRTIO_SND_S_BAD_MSG,
    VIRTIO_SND_S_NOT_SUPP,
    VIRTIO_SND_S_IO_ERR,
};

/* Unit: Hz */
#define SND_PCM_RATE \
    _(5512)          \
    _(8000)          \
    _(11025)         \
    _(16000)         \
    _(22050)         \
    _(32000)         \
    _(44100)         \
    _(48000)         \
    _(64000)         \
    _(88200)         \
    _(96000)         \
    _(176400)        \
    _(192000)        \
    _(384000)

enum {
#define _(rate) VIRTIO_SND_PCM_RATE_##rate,
    SND_PCM_RATE
#undef _
    VIRTIO_SND_PCM_RATE_COUNT,
};

static const int pcm_rate_tbl[] = {
#define _(rate) [VIRTIO_SND_PCM_RATE_##rate] = rate,
    SND_PCM_RATE
#undef _
};

enum {
    VIRTIO_SND_PCM_F_SHMEM_HOST = 0,
    VIRTIO_SND_PCM_F_SHMEM_GUEST,
    VIRTIO_SND_PCM_F_MSG_POLLING,
    VIRTIO_SND_PCM_F_EVT_SHMEM_PERIODS,
    VIRTIO_SND_PCM_F_EVT_XRUNS,
};

enum {
#define _(samp_fmt) VIRTIO_SND_PCM_FMT_##samp_fmt
    _(IMA_ADPCM) = 0,
    _(MU_LAW),
    _(A_LAW),
    _(S8),
    _(U8),
    _(S16),
    _(U16),
    _(S18_3),
    _(U18_3),
    _(S20_3),
    _(U20_3),
    _(S24_3),
    _(U24_3),
    _(S20),
    _(U20),
    _(S24),
    _(U24),
    _(S32),
    _(U32),
    _(FLOAT),
    _(FLOAT64),
    _(DSD_U8),
    _(DSD_U16),
    _(DSD_U32),
    _(IEC958_SUBFRAME),
#undef _
};

enum {
#define _(chmap_pos) VIRTIO_SND_CHMAP_##chmap_pos
    _(NONE) = 0,
    _(NA),
    _(MONO),
    _(FL),
    _(FR),
    _(RL),
    _(RR),
    _(FC),
    _(LFE),
    _(SL),
    _(SR),
    _(RC),
    _(FLC),
    _(FRC),
    _(RLC),
    _(RRC),
    _(FLW),
    _(FRW),
    _(FLH),
    _(FCH),
    _(FRH),
    _(TC),
    _(TFL),
    _(TFR),
    _(TFC),
    _(TRL),
    _(TRR),
    _(TRC),
    _(TFLC),
    _(TFRC),
    _(TSL),
    _(TSR),
    _(LLFE),
    _(RLFE),
    _(BC),
    _(BLC),
    _(BRC),
#undef _
};

enum {
    VIRTIO_SND_D_OUTPUT = 0,
    VIRTIO_SND_D_INPUT,
};

typedef struct {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
    uint32_t controls;
} virtio_snd_config_t;

typedef struct {
    uint32_t code;
} virtio_snd_hdr_t;

typedef struct {
    uint32_t hda_fn_nid;
} virtio_snd_info_t;

typedef struct {
    virtio_snd_hdr_t hdr;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
} virtio_snd_query_info_t;

typedef struct {
    virtio_snd_info_t hdr;
    uint32_t features;
    uint32_t hda_reg_defconf;
    uint32_t hda_reg_caps;
    uint8_t connected;
    uint8_t padding[7];
} virtio_snd_jack_info_t;

typedef struct {
    virtio_snd_info_t hdr;
    uint32_t features;
    uint64_t formats;
    uint64_t rates;
    uint8_t direction;
    uint8_t channels_min;
    uint8_t channels_max;
    uint8_t padding[5];
} virtio_snd_pcm_info_t;

typedef struct {
    virtio_snd_hdr_t hdr;
    uint32_t stream_id;
} virtio_snd_pcm_hdr_t;

typedef struct {
    virtio_snd_pcm_hdr_t hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
    uint8_t padding;
} virtio_snd_pcm_set_params_t;

typedef struct {
    uint32_t stream_id;
} virtio_snd_pcm_xfer_t;

typedef struct {
    uint32_t status;
    uint32_t latency_bytes;
} virtio_snd_pcm_status_t;

#define VIRTIO_SND_CHMAP_MAX_SIZE 18

typedef struct {
    virtio_snd_info_t hdr;
    uint8_t direction;
    uint8_t channels;
    uint8_t positions[VIRTIO_SND_CHMAP_MAX_SIZE];
} virtio_snd_chmap_info_t;

typedef struct {
    pthread_cond_t readable;
    pthread_cond_t writable;
    int buf_ev_notify;
    bool releasing;
    pthread_mutex_t lock;
} virtio_snd_queue_lock_t;

typedef struct {
    uint32_t stream_id;
} vsnd_stream_sel_t;

typedef struct vsnd_buf_queue_node {
    uint8_t *addr;
    uint32_t len;
    uint32_t pos;
    struct vsnd_buf_queue_node *next;
    struct vsnd_buf_queue_node *prev;
} vsnd_buf_queue_node_t;

typedef struct {
    virtio_snd_jack_info_t j;
    virtio_snd_pcm_info_t p;
    virtio_snd_chmap_info_t c;
    virtio_snd_pcm_set_params_t pp;
    PaStream *pa_stream;

    virtio_snd_queue_lock_t lock;
    vsnd_buf_queue_node_t *buf_head;
    vsnd_buf_queue_node_t *buf_tail;

    vsnd_stream_sel_t v;
    uint32_t epoch;
} virtio_snd_prop_t;

typedef struct {
    virtio_snd_config_t config;
    virtio_snd_prop_t props[VSND_DEV_CNT_MAX];

    pthread_mutex_t tx_mutex;
    pthread_cond_t tx_cond;
    int tx_ev_notify;
    bool tx_thread_stop;
    bool tx_thread_started;
    pthread_t tx_thread;

    bool pa_initialized;
    virtio_snd_state_t *vsnd;
} virtio_snd_priv_t;

typedef int (*vsnd_virtq_cb)(virtio_snd_state_t *vsnd,
                             virtio_snd_queue_t *queue,
                             uint32_t desc_idx,
                             uint32_t *plen);

static int virtio_snd_stream_cb(const void *input,
                                void *output,
                                unsigned long frame_cnt,
                                const PaStreamCallbackTimeInfo *time_info,
                                PaStreamCallbackFlags status_flags,
                                void *user_data);

static void virtio_queue_notify_handler(virtio_snd_state_t *vsnd, int index);

static inline virtio_snd_priv_t *vsnd_priv(virtio_snd_state_t *vsnd)
{
    return (virtio_snd_priv_t *) vsnd->priv;
}

static inline uint32_t vsnd_min(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static bool vsnd_stream_accepts_tx(uint32_t code)
{
    return code == VIRTIO_SND_R_PCM_PREPARE ||
           code == VIRTIO_SND_R_PCM_START;
}

static void vsnd_queue_push(virtio_snd_prop_t *props,
                            vsnd_buf_queue_node_t *node)
{
    node->next = NULL;
    node->prev = props->buf_tail;

    if (props->buf_tail)
        props->buf_tail->next = node;
    else
        props->buf_head = node;

    props->buf_tail = node;
}

static void vsnd_queue_remove(virtio_snd_prop_t *props,
                              vsnd_buf_queue_node_t *node)
{
    if (node->prev)
        node->prev->next = node->next;
    else
        props->buf_head = node->next;

    if (node->next)
        node->next->prev = node->prev;
    else
        props->buf_tail = node->prev;
}

static void vsnd_clear_buf_queue(virtio_snd_prop_t *props)
{
    vsnd_buf_queue_node_t *node = props->buf_head;
    while (node) {
        vsnd_buf_queue_node_t *next = node->next;
        free(node->addr);
        free(node);
        node = next;
    }

    props->buf_head = NULL;
    props->buf_tail = NULL;
}

static void virtio_snd_set_fail(virtio_snd_state_t *vsnd)
{
    vsnd->status |= VIRTIO_STATUS_DEVICE_NEEDS_RESET;

    if (vsnd->status & VIRTIO_STATUS_DRIVER_OK)
        vsnd->interrupt_status |= VIRTIO_INT_CONF_CHANGE;

    rv_log_error("virtio-snd: DEVICE_NEEDS_RESET");
}

static bool vsnd_guest_range_ok(uint64_t addr, uint64_t len)
{
#if MEM_SIZE < 0x100000000ULL
    if (addr >= MEM_SIZE)
        return false;
    if (len > MEM_SIZE)
        return false;
    if (addr + len < addr)
        return false;
    if (addr + len > MEM_SIZE)
        return false;
#endif
    return true;
}

static bool vsnd_word_range_ok(uint32_t word_addr, uint32_t words)
{
#if MEM_SIZE < 0x100000000ULL
    uint64_t byte_addr = (uint64_t) word_addr << 2;
    uint64_t byte_len = (uint64_t) words << 2;
    return vsnd_guest_range_ok(byte_addr, byte_len);
#else
    (void) word_addr;
    (void) words;
    return true;
#endif
}

static bool vsnd_check_word_range(virtio_snd_state_t *vsnd,
                                  uint32_t word_addr,
                                  uint32_t words)
{
    if (!vsnd_word_range_ok(word_addr, words)) {
        virtio_snd_set_fail(vsnd);
        return false;
    }

    return true;
}

static inline uint32_t vsnd_preprocess(virtio_snd_state_t *vsnd, uint32_t addr)
{
#if MEM_SIZE < 0x100000000ULL
    if ((addr >= MEM_SIZE) || (addr & 0b11)) {
#else
    if (addr & 0b11) {
#endif
        virtio_snd_set_fail(vsnd);
        return 0;
    }

    return addr >> 2;
}

static void vsnd_reset_stream(virtio_snd_prop_t *props)
{
    pthread_mutex_lock(&props->lock.lock);
    props->epoch++;
    props->lock.releasing = true;
    pthread_cond_broadcast(&props->lock.readable);
    pthread_cond_broadcast(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);

    if (props->pa_stream) {
        if (Pa_IsStreamActive(props->pa_stream) == 1)
            Pa_StopStream(props->pa_stream);
        Pa_CloseStream(props->pa_stream);
        props->pa_stream = NULL;
    }

    pthread_mutex_lock(&props->lock.lock);
    vsnd_clear_buf_queue(props);
    props->lock.buf_ev_notify = 0;
    props->lock.releasing = false;
    pthread_mutex_unlock(&props->lock.lock);

    memset(&props->pp, 0, sizeof(props->pp));
    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
}

static void virtio_snd_update_status(virtio_snd_state_t *vsnd, uint32_t status)
{
    vsnd->status |= status;

    if (status)
        return;

    uint32_t *ram = vsnd->ram;
    void *priv = vsnd->priv;

    if (priv) {
        virtio_snd_priv_t *p = (virtio_snd_priv_t *) priv;
        for (uint32_t i = 0; i < VSND_DEV_CNT_MAX; i++)
            vsnd_reset_stream(&p->props[i]);
    }

    memset(vsnd, 0, sizeof(*vsnd));
    vsnd->ram = ram;
    vsnd->priv = priv;
}

static void *vsnd_guest_ptr(virtio_snd_state_t *vsnd, uint64_t addr, uint32_t len)
{
    if (!vsnd_guest_range_ok(addr, len)) {
        virtio_snd_set_fail(vsnd);
        return NULL;
    }

    return (void *) ((uintptr_t) vsnd->ram + (uintptr_t) addr);
}

static bool vsnd_collect_desc_chain(virtio_snd_state_t *vsnd,
                                    virtio_snd_queue_t *queue,
                                    uint32_t desc_idx,
                                    struct virtq_desc *chain,
                                    uint32_t *chain_cnt)
{
    if (!queue->queue_num || queue->queue_num > VSND_QUEUE_NUM_MAX)
        return false;

    uint32_t cnt = 0;

    for (;;) {
        if (desc_idx >= queue->queue_num)
            return false;

        if (cnt >= queue->queue_num)
            return false;

        uint32_t desc_word = queue->queue_desc + desc_idx * 4;
        if (!vsnd_check_word_range(vsnd, desc_word, 4))
            return false;

        struct virtq_desc *desc =
            (struct virtq_desc *) &vsnd->ram[desc_word];

        if (!vsnd_guest_range_ok(desc->addr, desc->len))
            return false;

        chain[cnt] = *desc;
        cnt++;

        if (!(desc->flags & VIRTIO_DESC_F_NEXT))
            break;

        desc_idx = desc->next;
    }

    *chain_cnt = cnt;
    return true;
}

static void virtio_snd_read_jack_info_handler(virtio_snd_state_t *vsnd,
                                              virtio_snd_jack_info_t *info,
                                              const virtio_snd_query_info_t *query,
                                              uint32_t *payload_len)
{
    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    uint32_t cnt = query->count;

    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t id = query->start_id + i;
        if (id >= VSND_DEV_CNT_MAX)
            break;

        info[i].hdr.hda_fn_nid = 0;
        info[i].features = 0;
        info[i].hda_reg_defconf = 0;
        info[i].hda_reg_caps = 0;
        info[i].connected = 1;
        memset(info[i].padding, 0, sizeof(info[i].padding));

        p->props[id].j = info[i];
    }

    *payload_len = cnt * sizeof(*info);
}

static void virtio_snd_read_pcm_info_handler(virtio_snd_state_t *vsnd,
                                             virtio_snd_pcm_info_t *info,
                                             const virtio_snd_query_info_t *query,
                                             uint32_t *payload_len)
{
    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    uint32_t cnt = query->count;

    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t id = query->start_id + i;
        if (id >= VSND_DEV_CNT_MAX)
            break;

        info[i].hdr.hda_fn_nid = 0;
        info[i].features = 0;
        info[i].formats = 1ULL << VIRTIO_SND_PCM_FMT_S16;
        info[i].rates = 0;

#define _(rate) info[i].rates |= 1ULL << VIRTIO_SND_PCM_RATE_##rate;
        SND_PCM_RATE
#undef _

        info[i].direction = VIRTIO_SND_D_OUTPUT;
        info[i].channels_min = 1;
        info[i].channels_max = 1;
        memset(info[i].padding, 0, sizeof(info[i].padding));

        p->props[id].p = info[i];
    }

    *payload_len = cnt * sizeof(*info);
}

static void virtio_snd_read_chmap_info_handler(
    virtio_snd_state_t *vsnd,
    virtio_snd_chmap_info_t *info,
    const virtio_snd_query_info_t *query,
    uint32_t *payload_len)
{
    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    uint32_t cnt = query->count;

    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t id = query->start_id + i;
        if (id >= VSND_DEV_CNT_MAX)
            break;

        memset(&info[i], 0, sizeof(info[i]));
        info[i].hdr.hda_fn_nid = 0;
        info[i].direction = VIRTIO_SND_D_OUTPUT;
        info[i].channels = 1;
        info[i].positions[0] = VIRTIO_SND_CHMAP_MONO;

        p->props[id].c = info[i];
    }

    *payload_len = cnt * sizeof(*info);
}

static uint32_t virtio_snd_pcm_set_params(
    virtio_snd_state_t *vsnd,
    const virtio_snd_pcm_set_params_t *request,
    uint32_t *payload_len)
{
    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    uint32_t id = request->hdr.stream_id;

    if (id >= VSND_DEV_CNT_MAX)
        return VIRTIO_SND_S_BAD_MSG;

    if (request->channels != 1)
        return VIRTIO_SND_S_NOT_SUPP;

    if (request->format != VIRTIO_SND_PCM_FMT_S16)
        return VIRTIO_SND_S_NOT_SUPP;

    if (request->rate >= VIRTIO_SND_PCM_RATE_COUNT ||
        pcm_rate_tbl[request->rate] == 0)
        return VIRTIO_SND_S_NOT_SUPP;

    virtio_snd_prop_t *props = &p->props[id];
    uint32_t code = props->pp.hdr.hdr.code;

    if (code != VIRTIO_SND_R_PCM_RELEASE &&
        code != VIRTIO_SND_R_PCM_SET_PARAMS &&
        code != VIRTIO_SND_R_PCM_PREPARE)
        return VIRTIO_SND_S_BAD_MSG;

    props->pp = *request;
    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;

    *payload_len = 0;
    return VIRTIO_SND_S_OK;
}

static uint32_t virtio_snd_pcm_prepare(virtio_snd_state_t *vsnd,
                                       const virtio_snd_pcm_hdr_t *request,
                                       uint32_t *payload_len)
{
    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    uint32_t stream_id = request->stream_id;

    if (stream_id >= VSND_DEV_CNT_MAX)
        return VIRTIO_SND_S_BAD_MSG;

    virtio_snd_prop_t *props = &p->props[stream_id];
    uint32_t code = props->pp.hdr.hdr.code;

    if (code != VIRTIO_SND_R_PCM_RELEASE &&
        code != VIRTIO_SND_R_PCM_SET_PARAMS &&
        code != VIRTIO_SND_R_PCM_PREPARE)
        return VIRTIO_SND_S_BAD_MSG;

    if (props->pp.channels != 1 ||
        props->pp.format != VIRTIO_SND_PCM_FMT_S16 ||
        props->pp.rate >= VIRTIO_SND_PCM_RATE_COUNT ||
        pcm_rate_tbl[props->pp.rate] == 0)
        return VIRTIO_SND_S_BAD_MSG;

    pthread_mutex_lock(&props->lock.lock);
    props->epoch++;
    props->lock.releasing = true;
    pthread_cond_broadcast(&props->lock.readable);
    pthread_cond_broadcast(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);

    if (props->pa_stream) {
        if (Pa_IsStreamActive(props->pa_stream) == 1)
            Pa_StopStream(props->pa_stream);
        Pa_CloseStream(props->pa_stream);
        props->pa_stream = NULL;
    }

    pthread_mutex_lock(&props->lock.lock);
    vsnd_clear_buf_queue(props);
    props->lock.buf_ev_notify = 0;
    props->lock.releasing = false;
    pthread_mutex_unlock(&props->lock.lock);

    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_PREPARE;
    props->v.stream_id = stream_id;

    uint32_t channels = props->pp.channels;
    uint32_t rate = (uint32_t) pcm_rate_tbl[props->pp.rate];
    uint32_t period_frames = rate / 10;

    if (!period_frames)
        period_frames = 1;

    PaStreamParameters params = {
        .device = Pa_GetDefaultOutputDevice(),
        .channelCount = (int) channels,
        .sampleFormat = paInt16,
        .suggestedLatency = 0.1,
        .hostApiSpecificStreamInfo = NULL,
    };

    if (params.device == paNoDevice)
        return VIRTIO_SND_S_IO_ERR;

    PaError err = Pa_OpenStream(&props->pa_stream, NULL, &params, rate,
                                period_frames, paClipOff,
                                virtio_snd_stream_cb, &props->v);
    if (err != paNoError) {
        rv_log_error("PortAudio Pa_OpenStream failed: %s",
                     Pa_GetErrorText(err));
        props->pa_stream = NULL;
        return VIRTIO_SND_S_IO_ERR;
    }

    *payload_len = 0;
    return VIRTIO_SND_S_OK;
}

static uint32_t virtio_snd_pcm_start(virtio_snd_state_t *vsnd,
                                     const virtio_snd_pcm_hdr_t *request,
                                     uint32_t *payload_len)
{
    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    uint32_t stream_id = request->stream_id;

    if (stream_id >= VSND_DEV_CNT_MAX)
        return VIRTIO_SND_S_BAD_MSG;

    virtio_snd_prop_t *props = &p->props[stream_id];
    uint32_t code = props->pp.hdr.hdr.code;

    if (code != VIRTIO_SND_R_PCM_PREPARE && code != VIRTIO_SND_R_PCM_STOP)
        return VIRTIO_SND_S_BAD_MSG;

    if (!props->pa_stream)
        return VIRTIO_SND_S_IO_ERR;

    pthread_mutex_lock(&props->lock.lock);
    props->lock.releasing = false;
    pthread_mutex_unlock(&props->lock.lock);

    PaError err = Pa_StartStream(props->pa_stream);
    if (err != paNoError) {
        rv_log_error("PortAudio Pa_StartStream failed: %s",
                     Pa_GetErrorText(err));
        return VIRTIO_SND_S_IO_ERR;
    }

    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_START;

    *payload_len = 0;
    return VIRTIO_SND_S_OK;
}

static uint32_t virtio_snd_pcm_stop(virtio_snd_state_t *vsnd,
                                    const virtio_snd_pcm_hdr_t *request,
                                    uint32_t *payload_len)
{
    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    uint32_t stream_id = request->stream_id;

    if (stream_id >= VSND_DEV_CNT_MAX)
        return VIRTIO_SND_S_BAD_MSG;

    virtio_snd_prop_t *props = &p->props[stream_id];
    uint32_t code = props->pp.hdr.hdr.code;

    if (code != VIRTIO_SND_R_PCM_START)
        return VIRTIO_SND_S_BAD_MSG;

    if (!props->pa_stream)
        return VIRTIO_SND_S_IO_ERR;

    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_STOP;

    pthread_mutex_lock(&props->lock.lock);
    props->epoch++;
    props->lock.releasing = true;
    pthread_cond_broadcast(&props->lock.readable);
    pthread_cond_broadcast(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);

    PaError err = paNoError;
    if (Pa_IsStreamActive(props->pa_stream) == 1)
        err = Pa_StopStream(props->pa_stream);

    if (err != paNoError) {
        rv_log_error("PortAudio Pa_StopStream failed: %s",
                     Pa_GetErrorText(err));
        return VIRTIO_SND_S_IO_ERR;
    }

    pthread_mutex_lock(&props->lock.lock);
    vsnd_clear_buf_queue(props);
    props->lock.buf_ev_notify = 0;
    pthread_cond_broadcast(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);

    *payload_len = 0;
    return VIRTIO_SND_S_OK;
}

static uint32_t virtio_snd_pcm_release(virtio_snd_state_t *vsnd,
                                       const virtio_snd_pcm_hdr_t *request,
                                       uint32_t *payload_len)
{
    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    uint32_t stream_id = request->stream_id;

    if (stream_id >= VSND_DEV_CNT_MAX)
        return VIRTIO_SND_S_BAD_MSG;

    virtio_snd_prop_t *props = &p->props[stream_id];
    uint32_t code = props->pp.hdr.hdr.code;

    if (code != VIRTIO_SND_R_PCM_PREPARE &&
        code != VIRTIO_SND_R_PCM_STOP &&
        code != VIRTIO_SND_R_PCM_START)
        return VIRTIO_SND_S_BAD_MSG;

    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_RELEASE;

    pthread_mutex_lock(&props->lock.lock);
    props->epoch++;
    props->lock.releasing = true;
    pthread_cond_broadcast(&props->lock.readable);
    pthread_cond_broadcast(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);

    if (props->pa_stream) {
        if (Pa_IsStreamActive(props->pa_stream) == 1)
            Pa_StopStream(props->pa_stream);

        PaError err = Pa_CloseStream(props->pa_stream);
        if (err != paNoError)
            rv_log_error("PortAudio Pa_CloseStream failed: %s",
                         Pa_GetErrorText(err));
        props->pa_stream = NULL;
    }

    pthread_mutex_lock(&props->lock.lock);
    vsnd_clear_buf_queue(props);
    props->lock.buf_ev_notify = 0;
    pthread_cond_broadcast(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);

    virtio_queue_notify_handler(vsnd, VSND_FLUSH_QUEUE | VSND_QUEUE_TX);

    pthread_mutex_lock(&props->lock.lock);
    vsnd_clear_buf_queue(props);
    props->lock.buf_ev_notify = 0;
    props->lock.releasing = false;
    pthread_cond_broadcast(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);

    *payload_len = 0;
    return VIRTIO_SND_S_OK;
}

static bool __virtio_snd_frame_enqueue(virtio_snd_state_t *vsnd,
                                       const void *payload,
                                       uint32_t n,
                                       uint32_t stream_id,
                                       uint32_t expected_epoch)
{
    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    virtio_snd_prop_t *props = &p->props[stream_id];

    pthread_mutex_lock(&props->lock.lock);

    while (props->lock.buf_ev_notify >= VSND_MAX_PENDING_BUFS &&
           !props->lock.releasing && props->epoch == expected_epoch)
        pthread_cond_wait(&props->lock.writable, &props->lock.lock);

    if (props->lock.releasing || props->epoch != expected_epoch ||
        !vsnd_stream_accepts_tx(props->pp.hdr.hdr.code)) {
        pthread_mutex_unlock(&props->lock.lock);
        return false;
    }

    vsnd_buf_queue_node_t *node = calloc(1, sizeof(*node));
    if (!node) {
        pthread_mutex_unlock(&props->lock.lock);
        return false;
    }

    node->addr = malloc(n);
    if (!node->addr) {
        free(node);
        pthread_mutex_unlock(&props->lock.lock);
        return false;
    }

    memcpy(node->addr, payload, n);
    node->len = n;
    node->pos = 0;

    vsnd_queue_push(props, node);

    pthread_mutex_unlock(&props->lock.lock);
    return true;
}

static int virtio_snd_stream_cb(const void *input,
                                void *output,
                                unsigned long frame_cnt,
                                const PaStreamCallbackTimeInfo *time_info,
                                PaStreamCallbackFlags status_flags,
                                void *user_data)
{
    (void) input;
    (void) time_info;
    (void) status_flags;

    vsnd_stream_sel_t *sel = (vsnd_stream_sel_t *) user_data;
    uint32_t stream_id = sel->stream_id;

    /*
     * user_data points to props->v. Recover the containing virtio_snd_prop_t
     * using the offset of the v field.
     */
    virtio_snd_prop_t *props =
        (virtio_snd_prop_t *) ((uintptr_t) sel -
                               offsetof(virtio_snd_prop_t, v));

    int channels = props->pp.channels ? props->pp.channels : 1;
    uint32_t out_bytes = (uint32_t) frame_cnt * channels * VSND_CNFA_FRAME_SZ;
    uint8_t *out = (uint8_t *) output;

    (void) stream_id;

    pthread_mutex_lock(&props->lock.lock);

    while (props->lock.buf_ev_notify < 1 && !props->lock.releasing)
        pthread_cond_wait(&props->lock.readable, &props->lock.lock);

    if (props->lock.releasing) {
        pthread_mutex_unlock(&props->lock.lock);
        memset(output, 0, out_bytes);
        return paContinue;
    }

    uint32_t written = 0;
    while (props->buf_head && written < out_bytes) {
        vsnd_buf_queue_node_t *node = props->buf_head;
        uint32_t left = out_bytes - written;
        uint32_t actual = node->len - node->pos;
        uint32_t len = vsnd_min(left, actual);

        memcpy(out + written, node->addr + node->pos, len);

        written += len;
        node->pos += len;

        if (node->pos >= node->len) {
            vsnd_queue_remove(props, node);
            free(node->addr);
            free(node);
        }
    }

    if (written < out_bytes)
        memset(out + written, 0, out_bytes - written);

    if (props->lock.buf_ev_notify > 0)
        props->lock.buf_ev_notify--;

    pthread_cond_signal(&props->lock.writable);
    pthread_mutex_unlock(&props->lock.lock);

    return paContinue;
}

static int virtio_snd_tx_desc_handler(virtio_snd_state_t *vsnd,
                                      virtio_snd_queue_t *queue,
                                      uint32_t desc_idx,
                                      uint32_t *plen)
{
    struct virtq_desc chain[VSND_QUEUE_NUM_MAX];
    uint32_t cnt = 0;

    *plen = 0;

    if (!vsnd_collect_desc_chain(vsnd, queue, desc_idx, chain, &cnt))
        return -1;

    if (cnt < 3)
        return -1;

    struct virtq_desc *xfer_desc = &chain[0];
    struct virtq_desc *status_desc = &chain[cnt - 1];

    if (xfer_desc->flags & VIRTIO_DESC_F_WRITE)
        return -1;

    if (!(status_desc->flags & VIRTIO_DESC_F_WRITE))
        return -1;

    if (xfer_desc->len < sizeof(virtio_snd_pcm_xfer_t))
        return -1;

    if (status_desc->len < sizeof(virtio_snd_pcm_status_t))
        return -1;

    virtio_snd_pcm_xfer_t *xfer =
        vsnd_guest_ptr(vsnd, xfer_desc->addr, sizeof(*xfer));
    virtio_snd_pcm_status_t *status =
        vsnd_guest_ptr(vsnd, status_desc->addr, sizeof(*status));

    if (!xfer || !status)
        return -1;

    uint32_t stream_id = xfer->stream_id;
    bool bad = stream_id >= VSND_DEV_CNT_MAX;
    bool enqueued = false;
    uint32_t ret_len = 0;
    uint32_t epoch = 0;

    virtio_snd_prop_t *props = NULL;
    if (!bad) {
        virtio_snd_priv_t *p = vsnd_priv(vsnd);
        props = &p->props[stream_id];

        pthread_mutex_lock(&props->lock.lock);
        epoch = props->epoch;
        bool accepting = !props->lock.releasing &&
                         vsnd_stream_accepts_tx(props->pp.hdr.hdr.code);
        pthread_mutex_unlock(&props->lock.lock);

        if (!accepting) {
            status->status = VIRTIO_SND_S_OK;
            status->latency_bytes = 0;
            *plen = sizeof(*status);
            return 0;
        }
    }

    for (uint32_t i = 1; i + 1 < cnt; i++) {
        struct virtq_desc *payload_desc = &chain[i];

        if (payload_desc->flags & VIRTIO_DESC_F_WRITE) {
            bad = true;
            continue;
        }

        void *payload = vsnd_guest_ptr(vsnd, payload_desc->addr,
                                       payload_desc->len);
        if (!payload) {
            bad = true;
            continue;
        }

        if (!bad && __virtio_snd_frame_enqueue(vsnd, payload,
                                               payload_desc->len,
                                               stream_id, epoch))
            enqueued = true;

        ret_len += payload_desc->len;
    }

    status->status = bad ? VIRTIO_SND_S_IO_ERR : VIRTIO_SND_S_OK;
    status->latency_bytes = ret_len;
    *plen = sizeof(*status);

    if (!bad && enqueued && props) {
        pthread_mutex_lock(&props->lock.lock);
        if (!props->lock.releasing && props->epoch == epoch) {
            props->lock.buf_ev_notify++;
            pthread_cond_signal(&props->lock.readable);
        }
        pthread_mutex_unlock(&props->lock.lock);
    }

    return 0;
}

static int virtio_snd_io_desc_flush_handler(virtio_snd_state_t *vsnd,
                                            virtio_snd_queue_t *queue,
                                            uint32_t desc_idx,
                                            uint32_t *plen)
{
    struct virtq_desc chain[VSND_QUEUE_NUM_MAX];
    uint32_t cnt = 0;

    *plen = 0;

    if (!vsnd_collect_desc_chain(vsnd, queue, desc_idx, chain, &cnt))
        return 0;

    if (cnt < 3)
        return 0;

    struct virtq_desc *xfer_desc = &chain[0];
    struct virtq_desc *status_desc = &chain[cnt - 1];

    if (xfer_desc->len < sizeof(virtio_snd_pcm_xfer_t))
        return 0;

    if (status_desc->len < sizeof(virtio_snd_pcm_status_t))
        return 0;

    virtio_snd_pcm_xfer_t *xfer =
        vsnd_guest_ptr(vsnd, xfer_desc->addr, sizeof(*xfer));
    virtio_snd_pcm_status_t *status =
        vsnd_guest_ptr(vsnd, status_desc->addr, sizeof(*status));

    if (!xfer || !status)
        return 0;

    bool bad = xfer->stream_id >= VSND_DEV_CNT_MAX;

    status->status = bad ? VIRTIO_SND_S_IO_ERR : VIRTIO_SND_S_OK;
    status->latency_bytes = 0;
    *plen = sizeof(*status);

    return 0;
}

static uint32_t virtio_snd_ctrl_process(virtio_snd_state_t *vsnd,
                                        const void *query,
                                        void *info,
                                        uint32_t type,
                                        uint32_t info_len,
                                        uint32_t *payload_len)
{
    *payload_len = 0;

    switch (type) {
    case VIRTIO_SND_R_JACK_INFO: {
        const virtio_snd_query_info_t *q = query;
        uint64_t need = (uint64_t) q->count * sizeof(virtio_snd_jack_info_t);
        if (info_len < need)
            return VIRTIO_SND_S_BAD_MSG;
        virtio_snd_read_jack_info_handler(vsnd, info, q, payload_len);
        return VIRTIO_SND_S_OK;
    }
    case VIRTIO_SND_R_PCM_INFO: {
        const virtio_snd_query_info_t *q = query;
        uint64_t need = (uint64_t) q->count * sizeof(virtio_snd_pcm_info_t);
        if (info_len < need)
            return VIRTIO_SND_S_BAD_MSG;
        virtio_snd_read_pcm_info_handler(vsnd, info, q, payload_len);
        return VIRTIO_SND_S_OK;
    }
    case VIRTIO_SND_R_CHMAP_INFO: {
        const virtio_snd_query_info_t *q = query;
        uint64_t need = (uint64_t) q->count * sizeof(virtio_snd_chmap_info_t);
        if (info_len < need)
            return VIRTIO_SND_S_BAD_MSG;
        virtio_snd_read_chmap_info_handler(vsnd, info, q, payload_len);
        return VIRTIO_SND_S_OK;
    }
    case VIRTIO_SND_R_PCM_SET_PARAMS:
        return virtio_snd_pcm_set_params(vsnd, query, payload_len);
    case VIRTIO_SND_R_PCM_PREPARE:
        return virtio_snd_pcm_prepare(vsnd, query, payload_len);
    case VIRTIO_SND_R_PCM_RELEASE:
        return virtio_snd_pcm_release(vsnd, query, payload_len);
    case VIRTIO_SND_R_PCM_START:
        return virtio_snd_pcm_start(vsnd, query, payload_len);
    case VIRTIO_SND_R_PCM_STOP:
        return virtio_snd_pcm_stop(vsnd, query, payload_len);
    default:
        return VIRTIO_SND_S_NOT_SUPP;
    }
}

static int virtio_snd_ctrl_desc_handler(virtio_snd_state_t *vsnd,
                                        virtio_snd_queue_t *queue,
                                        uint32_t desc_idx,
                                        uint32_t *plen)
{
    struct virtq_desc chain[VSND_QUEUE_NUM_MAX];
    uint32_t cnt = 0;

    if (!vsnd_collect_desc_chain(vsnd, queue, desc_idx, chain, &cnt))
        return -1;

    if (cnt < 2)
        return -1;

    struct virtq_desc *request_desc = &chain[0];
    struct virtq_desc *response_desc = &chain[1];

    if (request_desc->flags & VIRTIO_DESC_F_WRITE)
        return -1;

    if (!(response_desc->flags & VIRTIO_DESC_F_WRITE))
        return -1;

    if (request_desc->len < sizeof(virtio_snd_hdr_t))
        return -1;

    if (response_desc->len < sizeof(virtio_snd_hdr_t))
        return -1;

    virtio_snd_hdr_t *request_hdr =
        vsnd_guest_ptr(vsnd, request_desc->addr, sizeof(*request_hdr));
    virtio_snd_hdr_t *response =
        vsnd_guest_ptr(vsnd, response_desc->addr, sizeof(*response));

    if (!request_hdr || !response)
        return -1;

    uint32_t type = request_hdr->code;
    void *query = vsnd_guest_ptr(vsnd, request_desc->addr, request_desc->len);
    if (!query)
        return -1;

    void *info = NULL;
    uint32_t info_len = 0;

    if (cnt >= 3) {
        struct virtq_desc *info_desc = &chain[2];

        if (!(info_desc->flags & VIRTIO_DESC_F_WRITE))
            return -1;

        info = vsnd_guest_ptr(vsnd, info_desc->addr, info_desc->len);
        if (!info)
            return -1;

        info_len = info_desc->len;
    }

    switch (type) {
    case VIRTIO_SND_R_JACK_INFO:
    case VIRTIO_SND_R_PCM_INFO:
    case VIRTIO_SND_R_CHMAP_INFO:
        if (request_desc->len < sizeof(virtio_snd_query_info_t) || !info)
            return -1;
        break;
    case VIRTIO_SND_R_PCM_SET_PARAMS:
        if (request_desc->len < sizeof(virtio_snd_pcm_set_params_t))
            return -1;
        break;
    case VIRTIO_SND_R_PCM_PREPARE:
    case VIRTIO_SND_R_PCM_RELEASE:
    case VIRTIO_SND_R_PCM_START:
    case VIRTIO_SND_R_PCM_STOP:
        if (request_desc->len < sizeof(virtio_snd_pcm_hdr_t))
            return -1;
        break;
    default:
        break;
    }

    uint32_t payload_len = 0;
    uint32_t status =
        virtio_snd_ctrl_process(vsnd, query, info, type, info_len, &payload_len);

    response->code = status;

    *plen = sizeof(*response) + payload_len;

    if (status != VIRTIO_SND_S_OK)
        rv_log_error("virtio-snd: CTRL type=0x%x status=0x%x", type, status);

    return 0;
}

static vsnd_virtq_cb virtio_snd_queue_op(int index)
{
    switch (index) {
    case VSND_QUEUE_CTRL:
        return virtio_snd_ctrl_desc_handler;
    case VSND_QUEUE_TX:
        return virtio_snd_tx_desc_handler;
    case VSND_FLUSH_QUEUE | VSND_QUEUE_TX:
        return virtio_snd_io_desc_flush_handler;
    default:
        return NULL;
    }
}

static void virtio_queue_notify_handler(virtio_snd_state_t *vsnd, int index)
{
    uint32_t *ram = vsnd->ram;
    virtio_snd_queue_t *queue = &vsnd->queues[index & 0x03];

    if (vsnd->status & VIRTIO_STATUS_DEVICE_NEEDS_RESET)
        return;

    if (!((vsnd->status & VIRTIO_STATUS_DRIVER_OK) && queue->ready)) {
        virtio_snd_set_fail(vsnd);
        return;
    }

    if (!queue->queue_num || queue->queue_num > VSND_QUEUE_NUM_MAX) {
        virtio_snd_set_fail(vsnd);
        return;
    }

    if (!vsnd_check_word_range(vsnd, queue->queue_avail, 1))
        return;

    if (!vsnd_check_word_range(vsnd, queue->queue_used, 1))
        return;

    uint16_t new_avail = ram[queue->queue_avail] >> 16;

    if (new_avail - queue->last_avail > (uint16_t) queue->queue_num) {
        virtio_snd_set_fail(vsnd);
        return;
    }

    if (queue->last_avail == new_avail)
        return;

    uint16_t new_used = ram[queue->queue_used] >> 16;

    while (queue->last_avail != new_avail) {
        uint16_t queue_idx = queue->last_avail % queue->queue_num;
        uint32_t avail_word = queue->queue_avail + 1 + queue_idx / 2;

        if (!vsnd_check_word_range(vsnd, avail_word, 1))
            return;

        uint16_t buffer_idx =
            ram[avail_word] >> (16 * (queue_idx % 2));

        if (buffer_idx >= queue->queue_num) {
            virtio_snd_set_fail(vsnd);
            return;
        }

        uint32_t len = 0;
        vsnd_virtq_cb handler = virtio_snd_queue_op(index);
        if (!handler) {
            virtio_snd_set_fail(vsnd);
            return;
        }

        int result = handler(vsnd, queue, buffer_idx, &len);
        if (result != 0) {
            virtio_snd_set_fail(vsnd);
            return;
        }

        uint32_t used_addr =
            queue->queue_used + 1 + (new_used % queue->queue_num) * 2;

        if (!vsnd_check_word_range(vsnd, used_addr, 2))
            return;

        ram[used_addr] = buffer_idx;
        ram[used_addr + 1] = len;
        // if ((index & 0x03) == VSND_QUEUE_TX && ((new_used + 1) % 256 == 0))
        //     rv_log_error("virtio-snd: TX used_idx=%u", new_used + 1);

        queue->last_avail++;
        new_used++;
    }

    ram[queue->queue_used] &= MASK(16);
    ram[queue->queue_used] |= ((uint32_t) new_used) << 16;

    if (!(ram[queue->queue_avail] & 1))
        vsnd->interrupt_status |= VIRTIO_INT_USED_RING;

    
}

static void *virtio_snd_tx_thread(void *arg)
{
    virtio_snd_state_t *vsnd = (virtio_snd_state_t *) arg;
    virtio_snd_priv_t *p = vsnd_priv(vsnd);

    for (;;) {
        pthread_mutex_lock(&p->tx_mutex);

        while (p->tx_ev_notify <= 0 && !p->tx_thread_stop)
            pthread_cond_wait(&p->tx_cond, &p->tx_mutex);

        if (p->tx_thread_stop) {
            pthread_mutex_unlock(&p->tx_mutex);
            break;
        }

        p->tx_ev_notify--;
        pthread_mutex_unlock(&p->tx_mutex);

        virtio_queue_notify_handler(vsnd, VSND_QUEUE_TX);
    }

    return NULL;
}

static uint32_t virtio_snd_config_read(virtio_snd_state_t *vsnd,
                                       uint32_t word_offset)
{
    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    uint32_t *cfg = (uint32_t *) &p->config;

    if (word_offset >= sizeof(p->config) / sizeof(uint32_t)) {
        virtio_snd_set_fail(vsnd);
        return 0;
    }

    return cfg[word_offset];
}

static void virtio_snd_config_write(virtio_snd_state_t *vsnd,
                                    uint32_t word_offset,
                                    uint32_t value)
{
    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    uint32_t *cfg = (uint32_t *) &p->config;

    if (word_offset >= sizeof(p->config) / sizeof(uint32_t)) {
        virtio_snd_set_fail(vsnd);
        return;
    }

    cfg[word_offset] = value;
}

uint32_t virtio_snd_read(virtio_snd_state_t *vsnd, uint32_t addr)
{
    addr = addr >> 2;

#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(MagicValue):
        return VIRTIO_MAGIC_NUMBER;
    case _(Version):
        return VIRTIO_VERSION;
    case _(DeviceID):
        return VIRTIO_SND_DEV_ID;
    case _(VendorID):
        return VIRTIO_VENDOR_ID;
    case _(DeviceFeatures):
        return vsnd->device_features_sel == 0
                   ? VSND_FEATURES_0 | vsnd->device_features
                   : (vsnd->device_features_sel == 1 ? VSND_FEATURES_1 : 0);
    case _(QueueNumMax):
        return VSND_QUEUE_NUM_MAX;
    case _(QueueReady):
        return (uint32_t) VSND_QUEUE.ready;
    case _(InterruptStatus):
        return vsnd->interrupt_status;
    case _(Status):
        return vsnd->status;
    case _(ConfigGeneration):
        return VIRTIO_CONFIG_GENERATE;
    default:
        if (addr >= _(Config))
            return virtio_snd_config_read(vsnd, addr - _(Config));
        return 0;
    }
#undef _
}

void virtio_snd_write(virtio_snd_state_t *vsnd, uint32_t addr, uint32_t value)
{
    addr = addr >> 2;

#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(DeviceFeaturesSel):
        vsnd->device_features_sel = value;
        break;
    case _(DriverFeatures):
        if (vsnd->driver_features_sel == 0)
            vsnd->driver_features = value;
        break;
    case _(DriverFeaturesSel):
        vsnd->driver_features_sel = value;
        break;
    case _(QueueSel):
        if (value < ARRAY_SIZE(vsnd->queues))
            vsnd->queue_sel = value;
        else
            virtio_snd_set_fail(vsnd);
        break;
    case _(QueueNum):
        if (value > 0 && value <= VSND_QUEUE_NUM_MAX)
            VSND_QUEUE.queue_num = value;
        else
            virtio_snd_set_fail(vsnd);
        break;
    case _(QueueReady):
        VSND_QUEUE.ready = value & 1;
        if (value & 1)
            VSND_QUEUE.last_avail = vsnd->ram[VSND_QUEUE.queue_avail] >> 16;
        break;
    case _(QueueDescLow):
        VSND_QUEUE.queue_desc = vsnd_preprocess(vsnd, value);
        break;
    case _(QueueDescHigh):
        if (value)
            virtio_snd_set_fail(vsnd);
        break;
    case _(QueueDriverLow):
        VSND_QUEUE.queue_avail = vsnd_preprocess(vsnd, value);
        break;
    case _(QueueDriverHigh):
        if (value)
            virtio_snd_set_fail(vsnd);
        break;
    case _(QueueDeviceLow):
        VSND_QUEUE.queue_used = vsnd_preprocess(vsnd, value);
        break;
    case _(QueueDeviceHigh):
        if (value)
            virtio_snd_set_fail(vsnd);
        break;
    case _(QueueNotify):
        if (value >= ARRAY_SIZE(vsnd->queues)) {
            virtio_snd_set_fail(vsnd);
            break;
        }

        switch (value) {
        case VSND_QUEUE_CTRL:
            virtio_queue_notify_handler(vsnd, value);
            break;

        case VSND_QUEUE_TX: {
            virtio_snd_priv_t *p = vsnd_priv(vsnd);
            pthread_mutex_lock(&p->tx_mutex);
            p->tx_ev_notify++;
            pthread_cond_signal(&p->tx_cond);
            pthread_mutex_unlock(&p->tx_mutex);
            break;
        }

        case VSND_QUEUE_EVT:
            break;

        case VSND_QUEUE_RX:
            break;

        default:
            break;
        }
        break;
    case _(InterruptACK):
        vsnd->interrupt_status &= ~value;
        break;
    case _(Status):
        virtio_snd_update_status(vsnd, value);
        break;
    default:
        if (addr >= _(Config))
            virtio_snd_config_write(vsnd, addr - _(Config), value);
        break;
    }
#undef _
}

static bool vsnd_init_prop(virtio_snd_prop_t *props)
{
    memset(props, 0, sizeof(*props));

    if (pthread_mutex_init(&props->lock.lock, NULL))
        return false;

    if (pthread_cond_init(&props->lock.readable, NULL)) {
        pthread_mutex_destroy(&props->lock.lock);
        return false;
    }

    if (pthread_cond_init(&props->lock.writable, NULL)) {
        pthread_cond_destroy(&props->lock.readable);
        pthread_mutex_destroy(&props->lock.lock);
        return false;
    }

    props->pp.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    return true;
}

static void vsnd_destroy_prop(virtio_snd_prop_t *props)
{
    vsnd_reset_stream(props);
    pthread_cond_destroy(&props->lock.readable);
    pthread_cond_destroy(&props->lock.writable);
    pthread_mutex_destroy(&props->lock.lock);
}

bool virtio_snd_init(virtio_snd_state_t *vsnd)
{
    if (!vsnd || !vsnd->priv)
        return false;

    virtio_snd_priv_t *p = vsnd_priv(vsnd);
    p->vsnd = vsnd;

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        rv_log_error("PortAudio Pa_Initialize failed: %s",
                     Pa_GetErrorText(err));
        return false;
    }

    p->pa_initialized = true;

    if (pthread_create(&p->tx_thread, NULL, virtio_snd_tx_thread, vsnd) != 0) {
        rv_log_error("cannot create virtio-snd TX thread");
        Pa_Terminate();
        p->pa_initialized = false;
        return false;
    }

    p->tx_thread_started = true;
    return true;
}

virtio_snd_state_t *vsnd_new(void)
{
    virtio_snd_state_t *vsnd = calloc(1, sizeof(*vsnd));
    assert(vsnd);

    virtio_snd_priv_t *p = calloc(1, sizeof(*p));
    if (!p) {
        free(vsnd);
        return NULL;
    }

    p->config.jacks = 1;
    p->config.streams = 1;
    p->config.chmaps = 1;
    p->config.controls = 0;

    if (pthread_mutex_init(&p->tx_mutex, NULL)) {
        free(p);
        free(vsnd);
        return NULL;
    }

    if (pthread_cond_init(&p->tx_cond, NULL)) {
        pthread_mutex_destroy(&p->tx_mutex);
        free(p);
        free(vsnd);
        return NULL;
    }

    for (uint32_t i = 0; i < VSND_DEV_CNT_MAX; i++) {
        if (!vsnd_init_prop(&p->props[i])) {
            for (uint32_t j = 0; j < i; j++)
                vsnd_destroy_prop(&p->props[j]);
            pthread_cond_destroy(&p->tx_cond);
            pthread_mutex_destroy(&p->tx_mutex);
            free(p);
            free(vsnd);
            return NULL;
        }
    }

    vsnd->priv = p;
    return vsnd;
}

void vsnd_delete(virtio_snd_state_t *vsnd)
{
    if (!vsnd)
        return;

    virtio_snd_priv_t *p = vsnd_priv(vsnd);

    if (p) {
        if (p->tx_thread_started) {
            pthread_mutex_lock(&p->tx_mutex);
            p->tx_thread_stop = true;
            pthread_cond_signal(&p->tx_cond);
            pthread_mutex_unlock(&p->tx_mutex);
            pthread_join(p->tx_thread, NULL);
            p->tx_thread_started = false;
        }

        for (uint32_t i = 0; i < VSND_DEV_CNT_MAX; i++)
            vsnd_destroy_prop(&p->props[i]);

        if (p->pa_initialized) {
            Pa_Terminate();
            p->pa_initialized = false;
        }

        pthread_cond_destroy(&p->tx_cond);
        pthread_mutex_destroy(&p->tx_mutex);
        free(p);
    }

    free(vsnd);
}