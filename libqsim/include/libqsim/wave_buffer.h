#ifndef LIBDSIM_WAVE_BUFFER_H
#define LIBDSIM_WAVE_BUFFER_H

#include "libqsim/value.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ring buffer storing waveform transitions per signal. */
typedef struct qsim_wave_buffer qsim_wave_buffer_t;

/* Create a waveform buffer for `signal_count` signals.
 * Each signal gets a ring buffer with `capacity_per_channel` entries.
 * signal_names must hold signal_count strings. */
qsim_wave_buffer_t *qsim_wave_buffer_create(uint32_t signal_count,
                                              uint32_t capacity_per_channel);
void qsim_wave_buffer_destroy(qsim_wave_buffer_t *buf);

/* Set signal names (optional, for VCD export). */
void qsim_wave_buffer_set_names(qsim_wave_buffer_t *buf,
                                 const char **signal_names, uint32_t count);

/* Record a transition for signal_id at the given time. */
void qsim_wave_buffer_record(qsim_wave_buffer_t *buf, uint32_t signal_id,
                              qsim_value_t value, uint64_t time);

/* Query transitions for a signal in [t_start, t_end).
 * Returns number of transitions written to out_values/out_times. */
size_t qsim_wave_buffer_query(const qsim_wave_buffer_t *buf,
                               uint32_t signal_id,
                               uint64_t t_start, uint64_t t_end,
                               qsim_value_t *out_values,
                               uint64_t *out_times,
                               size_t max_samples);

/* Get initial value (value at time 0) for a signal. */
qsim_value_t qsim_wave_buffer_initial(const qsim_wave_buffer_t *buf,
                                       uint32_t signal_id);

/* Export to VCD format. Returns 0 on success. */
int qsim_wave_buffer_export_vcd(const qsim_wave_buffer_t *buf,
                                 const char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* LIBDSIM_WAVE_BUFFER_H */
