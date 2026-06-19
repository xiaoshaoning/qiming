#include "libqsim/wave_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Per-signal channel ── */

typedef struct wave_channel {
    qsim_value_t *samples;
    uint64_t *sample_times;
    uint32_t capacity;
    uint32_t count;    /* total samples recorded */
    uint32_t head;     /* index of oldest sample */
    uint32_t tail;     /* index where next sample goes */
    qsim_value_t initial;
} wave_channel_t;

static void channel_init(wave_channel_t *ch, uint32_t capacity)
{
    ch->samples = calloc(capacity, sizeof(qsim_value_t));
    ch->sample_times = calloc(capacity, sizeof(uint64_t));
    ch->capacity = capacity;
    ch->count = 0;
    ch->head = 0;
    ch->tail = 0;
    ch->initial.state = QSIM_X;
    ch->initial.strength = QSIM_STRENGTH_STRONG;
}

static void channel_destroy(wave_channel_t *ch)
{
    free(ch->samples);
    free(ch->sample_times);
}

static void channel_record(wave_channel_t *ch, qsim_value_t value, uint64_t time)
{
    ch->samples[ch->tail] = value;
    ch->sample_times[ch->tail] = time;
    ch->tail = (ch->tail + 1) % ch->capacity;
    if (ch->count < ch->capacity) {
        ch->count++;
    } else {
        /* Overwriting oldest: advance head */
        ch->head = (ch->head + 1) % ch->capacity;
    }
}

/* ── Wave buffer ── */

struct qsim_wave_buffer {
    wave_channel_t *channels;
    uint32_t signal_count;
    uint32_t channel_capacity;
    char **signal_names;
    int names_set;
};

qsim_wave_buffer_t *qsim_wave_buffer_create(uint32_t signal_count,
                                              uint32_t capacity_per_channel)
{
    qsim_wave_buffer_t *buf = calloc(1, sizeof(qsim_wave_buffer_t));
    if (!buf) return NULL;

    buf->channels = calloc(signal_count, sizeof(wave_channel_t));
    if (!buf->channels) {
        free(buf);
        return NULL;
    }

    buf->signal_count = signal_count;
    buf->channel_capacity = capacity_per_channel;

    for (uint32_t i = 0; i < signal_count; i++)
        channel_init(&buf->channels[i], capacity_per_channel);

    return buf;
}

void qsim_wave_buffer_destroy(qsim_wave_buffer_t *buf)
{
    if (!buf) return;
    for (uint32_t i = 0; i < buf->signal_count; i++)
        channel_destroy(&buf->channels[i]);
    free(buf->channels);
    if (buf->signal_names) {
        for (uint32_t i = 0; i < buf->signal_count; i++)
            free(buf->signal_names[i]);
        free(buf->signal_names);
    }
    free(buf);
}

void qsim_wave_buffer_set_names(qsim_wave_buffer_t *buf,
                                 const char **signal_names, uint32_t count)
{
    if (!buf || !signal_names) return;
    if (buf->signal_names) {
        for (uint32_t i = 0; i < buf->signal_count; i++)
            free(buf->signal_names[i]);
        free(buf->signal_names);
    }
    uint32_t n = count < buf->signal_count ? count : buf->signal_count;
    buf->signal_names = calloc(n, sizeof(char *));
    for (uint32_t i = 0; i < n; i++)
        buf->signal_names[i] = strdup(signal_names[i]);
    buf->names_set = 1;
}

void qsim_wave_buffer_record(qsim_wave_buffer_t *buf, uint32_t signal_id,
                              qsim_value_t value, uint64_t time)
{
    if (!buf || signal_id >= buf->signal_count) return;

    wave_channel_t *ch = &buf->channels[signal_id];

    /* Set initial value on first record */
    if (ch->count == 0) {
        ch->initial = value;
    }

    channel_record(ch, value, time);
}

size_t qsim_wave_buffer_query(const qsim_wave_buffer_t *buf,
                               uint32_t signal_id,
                               uint64_t t_start, uint64_t t_end,
                               qsim_value_t *out_values,
                               uint64_t *out_times,
                               size_t max_samples)
{
    if (!buf || signal_id >= buf->signal_count) return 0;

    const wave_channel_t *ch = &buf->channels[signal_id];
    size_t written = 0;

    for (uint32_t i = 0; i < ch->count && written < max_samples; i++) {
        uint32_t idx = (ch->head + i) % ch->capacity;
        uint64_t t = ch->sample_times[idx];
        if (t >= t_start && t < t_end) {
            if (out_values) out_values[written] = ch->samples[idx];
            if (out_times) out_times[written] = t;
            written++;
        }
    }

    return written;
}

qsim_value_t qsim_wave_buffer_initial(const qsim_wave_buffer_t *buf,
                                       uint32_t signal_id)
{
    if (!buf || signal_id >= buf->signal_count) {
        qsim_value_t x = QSIM_VAL_X;
        return x;
    }
    return buf->channels[signal_id].initial;
}

int qsim_wave_buffer_export_vcd(const qsim_wave_buffer_t *buf,
                                 const char *filepath)
{
    if (!buf || !filepath) return -1;

    FILE *f = fopen(filepath, "w");
    if (!f) return -1;

    fprintf(f, "$version Qiming Simulator 0.1 $end\n");
    fprintf(f, "$timescale 1 ns $end\n");

    /* Define signals with VCD identifiers ($var wire N ID name $end) */
    uint64_t max_time = 0;
    for (uint32_t i = 0; i < buf->signal_count; i++) {
        const char *name = (buf->signal_names && i < buf->signal_count)
                           ? buf->signal_names[i] : "unknown";
        fprintf(f, "$var wire 1 x%x %s $end\n", i, name);

        /* Find max time */
        const wave_channel_t *ch = &buf->channels[i];
        if (ch->count > 0) {
            uint32_t last_idx = (ch->head + ch->count - 1) % ch->capacity;
            if (ch->sample_times[last_idx] > max_time)
                max_time = ch->sample_times[last_idx];
        }
    }

    fprintf(f, "$enddefinitions $end\n");
    fprintf(f, "$dumpvars\n");

    /* Write initial values */
    for (uint32_t i = 0; i < buf->signal_count; i++) {
        qsim_value_t init = qsim_wave_buffer_initial(buf, i);
        char c = 'x';
        switch (init.state) {
            case QSIM_0: c = '0'; break;
            case QSIM_1: c = '1'; break;
            case QSIM_X: c = 'x'; break;
            case QSIM_Z: c = 'z'; break;
            case QSIM_U: c = 'x'; break;
            case QSIM_W: c = 'x'; break;
            case QSIM_L: c = '0'; break;
            case QSIM_H: c = '1'; break;
            case QSIM_DC: c = 'x'; break;
        }
        fprintf(f, "%cx%x\n", c, i);
    }
    fprintf(f, "$end\n");

    /* Write transitions in time order using a simple merge approach */
    for (uint64_t t = 0; t <= max_time; t++) {
        int wrote_time = 0;
        for (uint32_t i = 0; i < buf->signal_count; i++) {
            const wave_channel_t *ch = &buf->channels[i];
            for (uint32_t j = 0; j < ch->count; j++) {
                uint32_t idx = (ch->head + j) % ch->capacity;
                if (ch->sample_times[idx] == t) {
                    if (!wrote_time) {
                        fprintf(f, "#%llu\n", (unsigned long long)t);
                        wrote_time = 1;
                    }
                    char c = 'x';
                    switch (ch->samples[idx].state) {
                        case QSIM_0: c = '0'; break;
                        case QSIM_1: c = '1'; break;
                        case QSIM_X: c = 'x'; break;
                        case QSIM_Z: c = 'z'; break;
                        case QSIM_U: c = 'x'; break;
                        case QSIM_W: c = 'x'; break;
                        case QSIM_L: c = '0'; break;
                        case QSIM_H: c = '1'; break;
                        case QSIM_DC: c = 'x'; break;
                    }
                    fprintf(f, "%cx%x\n", c, i);
                }
            }
        }
    }

    fclose(f);
    return 0;
}
