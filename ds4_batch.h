#ifndef DS4_BATCH_H
#define DS4_BATCH_H

#ifndef DS4_H
#include "ds4.h"
#endif

/* Experimental public batch inference API.
 *
 * The API is intentionally backend-neutral: callers work with claimed slots and
 * token steps, while optimized graph packing remains private to the runtime.
 * The session-slots backend is the portable baseline and stores one ordinary
 * ds4_session per slot. */

#define DS4_BATCH_MAX_SLOTS 64

typedef struct ds4_batch ds4_batch;

typedef enum {
    DS4_BATCH_BACKEND_SESSION_SLOTS = 0,
    DS4_BATCH_BACKEND_SHARED_DECODE = 1,
} ds4_batch_backend;

typedef struct {
    int ctx_size;
    int max_slots;
    ds4_batch_backend backend;
} ds4_batch_options;

typedef struct {
    int slot;
    int token;
} ds4_batch_step;

typedef struct {
    int slot;
    const int *tokens;
    int n_tokens;
    int refresh_logits;
} ds4_batch_prefill_segment;

const char *ds4_batch_backend_name_for_value(ds4_batch_backend backend);
int ds4_batch_backend_from_name(const char *name, ds4_batch_backend *out);

int ds4_batch_create_with_options(ds4_batch **out, ds4_engine *e,
                                  const ds4_batch_options *opt,
                                  char *err, size_t errlen);
int ds4_batch_create(ds4_batch **out, ds4_engine *e, int ctx_size, int max_slots);
int ds4_batch_create_with_backend(ds4_batch **out, ds4_engine *e, int ctx_size,
                                  int max_slots, const char *backend_name);
void ds4_batch_free(ds4_batch *b);

int ds4_batch_max_slots(ds4_batch *b);
int ds4_batch_ctx(ds4_batch *b);
int ds4_batch_prefill_capacity(ds4_batch *b);
const char *ds4_batch_backend_name(ds4_batch *b);

void ds4_batch_claim_slot(ds4_batch *b, int slot);
void ds4_batch_release_slot(ds4_batch *b, int slot);
uint64_t ds4_batch_slot_generation(ds4_batch *b, int slot);
int ds4_batch_slot_occupied(ds4_batch *b, int slot);
int ds4_batch_slot_logits_valid(ds4_batch *b, int slot);
int ds4_batch_slot_payload_valid(ds4_batch *b, int slot);
int ds4_batch_slot_can_save(ds4_batch *b, int slot);
void ds4_batch_mark_slot_error(ds4_batch *b, int slot);
void ds4_batch_set_progress(ds4_batch *b, int slot, ds4_session_progress_fn fn, void *ud);
void ds4_batch_set_display_progress(ds4_batch *b, int slot, ds4_session_progress_fn fn, void *ud);

int ds4_batch_sync(ds4_batch *b, int slot, const ds4_tokens *prompt, char *err, size_t errlen);
ds4_session_rewrite_result ds4_batch_rewrite_slot_from_common(
        ds4_batch *b, int slot, const ds4_tokens *prompt, int common,
        char *err, size_t errlen);
int ds4_batch_common_prefix(ds4_batch *b, int slot, const ds4_tokens *prompt);
int ds4_batch_argmax(ds4_batch *b, int slot);
int ds4_batch_argmax_excluding(ds4_batch *b, int slot, int excluded_id);
int ds4_batch_sample(ds4_batch *b, int slot, float temperature, int top_k,
                     float top_p, float min_p, uint64_t *rng);
int ds4_batch_top_logprobs(ds4_batch *b, int slot, ds4_token_score *out, int k);
int ds4_batch_token_logprob(ds4_batch *b, int slot, int token, ds4_token_score *out);

int ds4_batch_eval(ds4_batch *b, const ds4_batch_step *steps, int n_steps,
                   char *err, size_t errlen);
int ds4_batch_eval_top(ds4_batch *b, const ds4_batch_step *steps, int n_steps,
                       int *top_tokens, char *err, size_t errlen);
int ds4_batch_prefill(ds4_batch *b, const ds4_batch_step *steps,
                      const int *refresh_logits, int n_steps,
                      char *err, size_t errlen);
int ds4_batch_prefill_segments(ds4_batch *b,
                               const ds4_batch_prefill_segment *segments,
                               int n_segments, char *err, size_t errlen);

void ds4_batch_invalidate_slot(ds4_batch *b, int slot);
void ds4_batch_rewind_slot(ds4_batch *b, int slot, int pos);
int ds4_batch_pos(ds4_batch *b, int slot);
const ds4_tokens *ds4_batch_tokens(ds4_batch *b, int slot);

uint64_t ds4_batch_slot_payload_bytes(ds4_batch *b, int slot);
int ds4_batch_save_slot_payload(ds4_batch *b, int slot, FILE *fp,
                                char *err, size_t errlen);
int ds4_batch_load_slot_payload(ds4_batch *b, int slot, FILE *fp,
                                uint64_t payload_bytes,
                                char *err, size_t errlen);
int ds4_batch_save_slot_snapshot(ds4_batch *b, int slot,
                                 ds4_session_snapshot *snap,
                                 char *err, size_t errlen);
int ds4_batch_load_slot_snapshot(ds4_batch *b, int slot,
                                 const ds4_session_snapshot *snap,
                                 char *err, size_t errlen);

#endif
