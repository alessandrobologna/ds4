#include "ds4.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ds4_batch_private_shared_decode ds4_batch_private_shared_decode;

int ds4_batch_private_shared_create(ds4_batch_private_shared_decode **out,
                                    ds4_engine *e,
                                    int ctx_size,
                                    int max_slots,
                                    int prefill_rows,
                                    char *err,
                                    size_t errlen);
void ds4_batch_private_shared_free(ds4_batch_private_shared_decode *sh);
int ds4_batch_private_shared_prefill_capacity(ds4_batch_private_shared_decode *sh);
ds4_session *ds4_batch_private_shared_create_slot(ds4_batch_private_shared_decode *sh,
                                                  int slot);
void ds4_batch_private_shared_free_slot(ds4_session *s);
void ds4_batch_private_shared_clear_slot(ds4_batch_private_shared_decode *sh, int slot);
int ds4_batch_private_shared_attach_slot(ds4_batch_private_shared_decode *sh,
                                         int slot,
                                         ds4_session *s,
                                         char *err,
                                         size_t errlen);
void ds4_batch_private_shared_detach_slot(ds4_batch_private_shared_decode *sh,
                                          int slot,
                                          ds4_session *s,
                                          int capture_counters);
int ds4_session_prefill_segments_many(ds4_session **sessions,
                                      const int *const *tokens,
                                      const int *n_tokens,
                                      const int *refresh_logits,
                                      int n_segments,
                                      char *err,
                                      size_t errlen);

typedef struct {
    bool occupied;
    bool logits_valid;
    bool payload_valid;
    bool error;
    bool eval_in_flight;
    bool needs_rebuild;
    uint64_t generation;
} ds4_batch_slot_state;

struct ds4_batch {
    ds4_engine *engine;
    ds4_session **slot;
    ds4_batch_slot_state *state;
    ds4_batch_private_shared_decode *shared;
    ds4_batch_backend backend;
    int max_slots;
    int ctx_size;
};

static void batch_set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg ? msg : "batch error");
}

const char *ds4_batch_backend_name_for_value(ds4_batch_backend backend) {
    switch (backend) {
    case DS4_BATCH_BACKEND_SESSION_SLOTS:
        return "session-slots";
    case DS4_BATCH_BACKEND_SHARED_DECODE:
        return "shared-decode";
    }
    return "unknown";
}

int ds4_batch_backend_from_name(const char *name, ds4_batch_backend *out) {
    if (!name || !name[0] || !strcmp(name, "session-slots")) {
        if (out) *out = DS4_BATCH_BACKEND_SESSION_SLOTS;
        return 1;
    }
    if (!strcmp(name, "shared-decode")) {
        if (out) *out = DS4_BATCH_BACKEND_SHARED_DECODE;
        return 1;
    }
    return 0;
}

static ds4_session *batch_slot(ds4_batch *b, int slot) {
    if (!b || slot < 0 || slot >= b->max_slots) return NULL;
    return b->slot[slot];
}

static ds4_batch_slot_state *batch_state(ds4_batch *b, int slot) {
    if (!b || slot < 0 || slot >= b->max_slots) return NULL;
    return &b->state[slot];
}

static void batch_mark_slot_invalid(ds4_batch *b, int slot, bool error) {
    ds4_batch_slot_state *st = batch_state(b, slot);
    if (!st) return;
    st->logits_valid = false;
    st->payload_valid = false;
    st->error = error;
    st->eval_in_flight = false;
    st->needs_rebuild = true;
    if (b && b->shared) ds4_batch_private_shared_clear_slot(b->shared, slot);
}

static int batch_require_slot(ds4_batch *b, int slot, ds4_session **out_s,
                              ds4_batch_slot_state **out_st,
                              char *err, size_t errlen) {
    ds4_session *s = batch_slot(b, slot);
    ds4_batch_slot_state *st = batch_state(b, slot);
    if (!s || !st) {
        batch_set_err(err, errlen, "invalid batch slot");
        return 1;
    }
    if (!st->occupied) {
        batch_set_err(err, errlen, "batch slot is not claimed");
        return 1;
    }
    if (st->eval_in_flight) {
        batch_set_err(err, errlen, "batch slot already has an in-flight step");
        return 1;
    }
    if (out_s) *out_s = s;
    if (out_st) *out_st = st;
    return 0;
}

static int batch_validate_steps(ds4_batch *b, const ds4_batch_step *steps,
                                int n_steps, ds4_session **sessions,
                                ds4_batch_slot_state **states,
                                char *err, size_t errlen) {
    if (!b || (!steps && n_steps > 0) || n_steps < 0 || n_steps > b->max_slots) {
        batch_set_err(err, errlen, "invalid batch step request");
        return 1;
    }
    bool seen[DS4_BATCH_MAX_SLOTS] = {0};
    for (int i = 0; i < n_steps; i++) {
        const int slot = steps[i].slot;
        if (slot < 0 || slot >= b->max_slots || seen[slot]) {
            batch_set_err(err, errlen, "batch step has an invalid or duplicate slot");
            return 1;
        }
        seen[slot] = true;
        ds4_session *s = NULL;
        ds4_batch_slot_state *st = NULL;
        if (batch_require_slot(b, slot, &s, &st, err, errlen) != 0) return 1;
        if (st->error) {
            batch_set_err(err, errlen, "batch slot is in an error state");
            return 1;
        }
        if (ds4_session_pos(s) >= ds4_session_ctx(s)) {
            batch_set_err(err, errlen, "batch slot has no context room");
            return 1;
        }
        if (sessions) sessions[i] = s;
        if (states) states[i] = st;
    }
    return 0;
}

static void batch_steps_in_flight(ds4_batch_slot_state **states, int n_steps, bool value) {
    for (int i = 0; i < n_steps; i++) {
        if (states[i]) states[i]->eval_in_flight = value;
    }
}

static void batch_finish_full_step(ds4_batch_slot_state *st) {
    if (!st) return;
    st->logits_valid = true;
    st->payload_valid = true;
    st->error = false;
    st->eval_in_flight = false;
    st->needs_rebuild = false;
}

static void batch_finish_top_step(ds4_batch_slot_state *st) {
    if (!st) return;
    st->logits_valid = false;
    st->payload_valid = false;
    st->error = false;
    st->eval_in_flight = false;
    st->needs_rebuild = false;
}

static void batch_fail_steps(ds4_batch *b, const ds4_batch_step *steps,
                             ds4_session **sessions, int n_steps) {
    for (int i = 0; i < n_steps; i++) {
        if (sessions && sessions[i]) ds4_session_invalidate(sessions[i]);
        if (steps) batch_mark_slot_invalid(b, steps[i].slot, true);
    }
}

static int batch_shared_attach_slot(ds4_batch *b, int slot, ds4_session *s,
                                    char *err, size_t errlen) {
    if (!b || b->backend != DS4_BATCH_BACKEND_SHARED_DECODE) return 0;
    return ds4_batch_private_shared_attach_slot(b->shared, slot, s, err, errlen);
}

static void batch_shared_detach_slot(ds4_batch *b, int slot, ds4_session *s,
                                     bool capture_counters) {
    if (!b || b->backend != DS4_BATCH_BACKEND_SHARED_DECODE) return;
    ds4_batch_private_shared_detach_slot(b->shared, slot, s, capture_counters ? 1 : 0);
}

static int batch_shared_attach_steps(ds4_batch *b, const ds4_batch_step *steps,
                                     ds4_session **sessions, bool *attached,
                                     int n_steps, char *err, size_t errlen) {
    if (!b || b->backend != DS4_BATCH_BACKEND_SHARED_DECODE) return 0;
    for (int i = 0; i < n_steps; i++) {
        if (batch_shared_attach_slot(b, steps[i].slot, sessions[i], err, errlen) != 0) {
            for (int j = i - 1; j >= 0; j--) {
                if (attached[j]) batch_shared_detach_slot(b, steps[j].slot, sessions[j], false);
            }
            return 1;
        }
        attached[i] = true;
    }
    return 0;
}

static void batch_shared_detach_steps(ds4_batch *b, const ds4_batch_step *steps,
                                      ds4_session **sessions, bool *attached,
                                      int n_steps, bool capture_counters) {
    if (!b || b->backend != DS4_BATCH_BACKEND_SHARED_DECODE) return;
    for (int i = 0; i < n_steps; i++) {
        if (!attached[i]) continue;
        batch_shared_detach_slot(b, steps[i].slot, sessions[i], capture_counters);
        attached[i] = false;
    }
}

static int batch_shared_attach_segments(ds4_batch *b,
                                        const ds4_batch_prefill_segment *segments,
                                        ds4_session **sessions,
                                        bool *attached,
                                        int n_segments,
                                        char *err,
                                        size_t errlen) {
    if (!b || b->backend != DS4_BATCH_BACKEND_SHARED_DECODE) return 0;
    for (int i = 0; i < n_segments; i++) {
        if (batch_shared_attach_slot(b, segments[i].slot, sessions[i], err, errlen) != 0) {
            for (int j = i - 1; j >= 0; j--) {
                if (attached[j]) batch_shared_detach_slot(b, segments[j].slot, sessions[j], false);
            }
            return 1;
        }
        attached[i] = true;
    }
    return 0;
}

static void batch_shared_detach_segments(ds4_batch *b,
                                         const ds4_batch_prefill_segment *segments,
                                         ds4_session **sessions,
                                         bool *attached,
                                         int n_segments,
                                         bool capture_counters) {
    if (!b || b->backend != DS4_BATCH_BACKEND_SHARED_DECODE) return;
    for (int i = 0; i < n_segments; i++) {
        if (!attached[i]) continue;
        batch_shared_detach_slot(b, segments[i].slot, sessions[i], capture_counters);
        attached[i] = false;
    }
}

int ds4_batch_create_with_options(ds4_batch **out, ds4_engine *e,
                                  const ds4_batch_options *opt,
                                  char *err, size_t errlen) {
    if (!out || !e || !opt || opt->ctx_size <= 0 ||
        opt->max_slots <= 0 || opt->max_slots > DS4_BATCH_MAX_SLOTS) {
        batch_set_err(err, errlen, "invalid batch options");
        return 1;
    }
    *out = NULL;
    if (opt->backend != DS4_BATCH_BACKEND_SESSION_SLOTS &&
        opt->backend != DS4_BATCH_BACKEND_SHARED_DECODE) {
        batch_set_err(err, errlen, "unknown batch backend");
        return 1;
    }
    if (opt->max_slots > 1 && ds4_engine_has_mtp(e)) {
        batch_set_err(err, errlen, "MTP is not supported with multi-slot batching");
        return 1;
    }
    ds4_batch_backend backend = opt->backend;
    if (backend == DS4_BATCH_BACKEND_SHARED_DECODE &&
        opt->max_slots == 1 && ds4_engine_has_mtp(e)) {
        backend = DS4_BATCH_BACKEND_SESSION_SLOTS;
    }

    ds4_batch *b = calloc(1, sizeof(*b));
    if (!b) {
        batch_set_err(err, errlen, "out of memory");
        return 1;
    }
    b->engine = e;
    b->backend = backend;
    b->max_slots = opt->max_slots;
    b->ctx_size = opt->ctx_size;
    b->slot = calloc((size_t)b->max_slots, sizeof(b->slot[0]));
    b->state = calloc((size_t)b->max_slots, sizeof(b->state[0]));
    if (!b->slot || !b->state) {
        ds4_batch_free(b);
        batch_set_err(err, errlen, "out of memory");
        return 1;
    }
    for (int i = 0; i < b->max_slots; i++) {
        if (b->backend == DS4_BATCH_BACKEND_SHARED_DECODE && !b->shared) {
            if (ds4_batch_private_shared_create(&b->shared,
                                                e,
                                                b->ctx_size,
                                                b->max_slots,
                                                opt->prefill_rows,
                                                err,
                                                errlen) != 0) {
                ds4_batch_free(b);
                return 1;
            }
        }
        if (b->backend == DS4_BATCH_BACKEND_SHARED_DECODE) {
            b->slot[i] = ds4_batch_private_shared_create_slot(b->shared, i);
            if (!b->slot[i]) {
                ds4_batch_free(b);
                batch_set_err(err, errlen, "failed to create shared batch slot");
                return 1;
            }
        } else {
            if (ds4_session_create(&b->slot[i], e, b->ctx_size) != 0) {
                ds4_batch_free(b);
                batch_set_err(err, errlen, "failed to create batch slot session");
                return 1;
            }
        }
    }
    *out = b;
    return 0;
}

int ds4_batch_create(ds4_batch **out, ds4_engine *e, int ctx_size, int max_slots) {
    ds4_batch_options opt = {
        .ctx_size = ctx_size,
        .max_slots = max_slots,
        .backend = DS4_BATCH_BACKEND_SESSION_SLOTS,
    };
    return ds4_batch_create_with_options(out, e, &opt, NULL, 0);
}

int ds4_batch_create_with_backend(ds4_batch **out, ds4_engine *e, int ctx_size,
                                  int max_slots, const char *backend_name) {
    ds4_batch_backend backend = DS4_BATCH_BACKEND_SESSION_SLOTS;
    if (!ds4_batch_backend_from_name(backend_name, &backend)) return 1;
    ds4_batch_options opt = {
        .ctx_size = ctx_size,
        .max_slots = max_slots,
        .backend = backend,
    };
    return ds4_batch_create_with_options(out, e, &opt, NULL, 0);
}

void ds4_batch_free(ds4_batch *b) {
    if (!b) return;
    if (b->slot) {
        for (int i = 0; i < b->max_slots; i++) {
            if (b->backend == DS4_BATCH_BACKEND_SHARED_DECODE) {
                ds4_batch_private_shared_free_slot(b->slot[i]);
            } else {
                ds4_session_free(b->slot[i]);
            }
        }
    }
    ds4_batch_private_shared_free(b->shared);
    free(b->slot);
    free(b->state);
    free(b);
}

int ds4_batch_max_slots(ds4_batch *b) {
    return b ? b->max_slots : 0;
}

int ds4_batch_ctx(ds4_batch *b) {
    return b ? b->ctx_size : 0;
}

int ds4_batch_prefill_capacity(ds4_batch *b) {
    if (!b) return 0;
    if (b->backend == DS4_BATCH_BACKEND_SHARED_DECODE) {
        const int cap = ds4_batch_private_shared_prefill_capacity(b->shared);
        if (cap > 0) return cap;
    }
    return b->ctx_size;
}

const char *ds4_batch_backend_name(ds4_batch *b) {
    return b ? ds4_batch_backend_name_for_value(b->backend) : "none";
}

void ds4_batch_claim_slot(ds4_batch *b, int slot) {
    ds4_batch_slot_state *st = batch_state(b, slot);
    if (!st) return;
    st->occupied = true;
    st->error = false;
    st->eval_in_flight = false;
    st->needs_rebuild = false;
    st->generation++;
    if (st->generation == 0) st->generation = 1;
}

void ds4_batch_release_slot(ds4_batch *b, int slot) {
    ds4_batch_slot_state *st = batch_state(b, slot);
    if (!st) return;
    st->occupied = false;
    st->eval_in_flight = false;
}

uint64_t ds4_batch_slot_generation(ds4_batch *b, int slot) {
    ds4_batch_slot_state *st = batch_state(b, slot);
    return st ? st->generation : 0;
}

int ds4_batch_slot_occupied(ds4_batch *b, int slot) {
    ds4_batch_slot_state *st = batch_state(b, slot);
    return st && st->occupied;
}

int ds4_batch_slot_logits_valid(ds4_batch *b, int slot) {
    ds4_batch_slot_state *st = batch_state(b, slot);
    return st && st->logits_valid && !st->error && !st->eval_in_flight;
}

int ds4_batch_slot_payload_valid(ds4_batch *b, int slot) {
    ds4_batch_slot_state *st = batch_state(b, slot);
    return st && st->payload_valid && !st->error && !st->eval_in_flight;
}

int ds4_batch_slot_can_save(ds4_batch *b, int slot) {
    ds4_session *s = batch_slot(b, slot);
    return s && ds4_batch_slot_logits_valid(b, slot) &&
           ds4_batch_slot_payload_valid(b, slot) &&
           ds4_session_payload_bytes(s) > 0;
}

void ds4_batch_mark_slot_error(ds4_batch *b, int slot) {
    ds4_session *s = batch_slot(b, slot);
    if (s) ds4_session_invalidate(s);
    batch_mark_slot_invalid(b, slot, true);
}

void ds4_batch_set_progress(ds4_batch *b, int slot, ds4_session_progress_fn fn, void *ud) {
    ds4_session *s = batch_slot(b, slot);
    if (s) ds4_session_set_progress(s, fn, ud);
}

void ds4_batch_set_display_progress(ds4_batch *b, int slot, ds4_session_progress_fn fn, void *ud) {
    ds4_session *s = batch_slot(b, slot);
    if (s) ds4_session_set_display_progress(s, fn, ud);
}

int ds4_batch_sync(ds4_batch *b, int slot, const ds4_tokens *prompt, char *err, size_t errlen) {
    ds4_session *s = NULL;
    ds4_batch_slot_state *st = NULL;
    if (batch_require_slot(b, slot, &s, &st, err, errlen) != 0) return 1;
    if (st->error || st->needs_rebuild) {
        ds4_session_invalidate(s);
        if (b && b->shared) ds4_batch_private_shared_clear_slot(b->shared, slot);
    }
    st->eval_in_flight = true;
    if (batch_shared_attach_slot(b, slot, s, err, errlen) != 0) {
        batch_mark_slot_invalid(b, slot, true);
        return 1;
    }
    int rc = ds4_session_sync(s, prompt, err, errlen);
    batch_shared_detach_slot(b, slot, s, rc == 0);
    st->eval_in_flight = false;
    if (rc == 0) {
        st->logits_valid = true;
        st->payload_valid = true;
        st->error = false;
        st->needs_rebuild = false;
    } else {
        batch_mark_slot_invalid(b, slot, true);
    }
    return rc;
}

ds4_session_rewrite_result ds4_batch_rewrite_slot_from_common(
        ds4_batch *b, int slot, const ds4_tokens *prompt, int common,
        char *err, size_t errlen) {
    ds4_session *s = NULL;
    ds4_batch_slot_state *st = NULL;
    if (batch_require_slot(b, slot, &s, &st, err, errlen) != 0) {
        return DS4_SESSION_REWRITE_ERROR;
    }
    if (st->error) {
        batch_set_err(err, errlen, "batch slot is in an error state");
        return DS4_SESSION_REWRITE_ERROR;
    }
    st->eval_in_flight = true;
    if (batch_shared_attach_slot(b, slot, s, err, errlen) != 0) {
        batch_mark_slot_invalid(b, slot, true);
        return DS4_SESSION_REWRITE_ERROR;
    }
    ds4_session_rewrite_result rr =
        ds4_session_rewrite_from_common(s, prompt, common, err, errlen);
    batch_shared_detach_slot(b, slot, s, rr == DS4_SESSION_REWRITE_OK);
    st->eval_in_flight = false;
    if (rr == DS4_SESSION_REWRITE_OK) {
        st->logits_valid = true;
        st->payload_valid = true;
        st->error = false;
        st->needs_rebuild = false;
    } else if (rr == DS4_SESSION_REWRITE_ERROR) {
        batch_mark_slot_invalid(b, slot, true);
    }
    return rr;
}

int ds4_batch_common_prefix(ds4_batch *b, int slot, const ds4_tokens *prompt) {
    ds4_session *s = batch_slot(b, slot);
    if (!s || !prompt || !ds4_batch_slot_payload_valid(b, slot)) return 0;
    return ds4_session_common_prefix(s, prompt);
}

int ds4_batch_argmax(ds4_batch *b, int slot) {
    ds4_session *s = batch_slot(b, slot);
    return s && ds4_batch_slot_occupied(b, slot) &&
           ds4_batch_slot_logits_valid(b, slot) ? ds4_session_argmax(s) : -1;
}

int ds4_batch_argmax_excluding(ds4_batch *b, int slot, int excluded_id) {
    ds4_session *s = batch_slot(b, slot);
    return s && ds4_batch_slot_occupied(b, slot) &&
           ds4_batch_slot_logits_valid(b, slot) ?
           ds4_session_argmax_excluding(s, excluded_id) : -1;
}

int ds4_batch_sample(ds4_batch *b, int slot, float temperature, int top_k,
                     float top_p, float min_p, uint64_t *rng) {
    ds4_session *s = batch_slot(b, slot);
    return s && ds4_batch_slot_occupied(b, slot) &&
           ds4_batch_slot_logits_valid(b, slot) ?
           ds4_session_sample(s, temperature, top_k, top_p, min_p, rng) : -1;
}

int ds4_batch_top_logprobs(ds4_batch *b, int slot, ds4_token_score *out, int k) {
    ds4_session *s = batch_slot(b, slot);
    return s && ds4_batch_slot_occupied(b, slot) &&
           ds4_batch_slot_logits_valid(b, slot) ?
           ds4_session_top_logprobs(s, out, k) : 0;
}

int ds4_batch_token_logprob(ds4_batch *b, int slot, int token, ds4_token_score *out) {
    ds4_session *s = batch_slot(b, slot);
    return s && ds4_batch_slot_occupied(b, slot) &&
           ds4_batch_slot_logits_valid(b, slot) ?
           ds4_session_token_logprob(s, token, out) : 0;
}

int ds4_batch_eval(ds4_batch *b, const ds4_batch_step *steps, int n_steps,
                   char *err, size_t errlen) {
    ds4_session *sessions[DS4_BATCH_MAX_SLOTS] = {0};
    ds4_batch_slot_state *states[DS4_BATCH_MAX_SLOTS] = {0};
    if (batch_validate_steps(b, steps, n_steps, sessions, states, err, errlen) != 0) {
        return 1;
    }
    bool attached[DS4_BATCH_MAX_SLOTS] = {0};
    if (batch_shared_attach_steps(b, steps, sessions, attached, n_steps, err, errlen) != 0) {
        batch_fail_steps(b, steps, sessions, n_steps);
        return 1;
    }
    batch_steps_in_flight(states, n_steps, true);
    int tokens[DS4_BATCH_MAX_SLOTS];
    for (int i = 0; i < n_steps; i++) tokens[i] = steps[i].token;
    if (ds4_session_eval_many(sessions, tokens, n_steps, err, errlen) != 0) {
        batch_shared_detach_steps(b, steps, sessions, attached, n_steps, false);
        batch_fail_steps(b, steps, sessions, n_steps);
        return 1;
    }
    batch_shared_detach_steps(b, steps, sessions, attached, n_steps, true);
    for (int i = 0; i < n_steps; i++) batch_finish_full_step(states[i]);
    return 0;
}

int ds4_batch_eval_top(ds4_batch *b, const ds4_batch_step *steps, int n_steps,
                       int *top_tokens, char *err, size_t errlen) {
    if (!top_tokens && n_steps > 0) {
        batch_set_err(err, errlen, "invalid batch top-token request");
        return 1;
    }
    ds4_session *sessions[DS4_BATCH_MAX_SLOTS] = {0};
    ds4_batch_slot_state *states[DS4_BATCH_MAX_SLOTS] = {0};
    if (batch_validate_steps(b, steps, n_steps, sessions, states, err, errlen) != 0) {
        return 1;
    }
    bool attached[DS4_BATCH_MAX_SLOTS] = {0};
    if (batch_shared_attach_steps(b, steps, sessions, attached, n_steps, err, errlen) != 0) {
        batch_fail_steps(b, steps, sessions, n_steps);
        return 1;
    }
    batch_steps_in_flight(states, n_steps, true);
    int tokens[DS4_BATCH_MAX_SLOTS];
    for (int i = 0; i < n_steps; i++) {
        top_tokens[i] = -1;
        tokens[i] = steps[i].token;
    }
    if (ds4_session_eval_top_many(sessions, tokens, n_steps, top_tokens,
                                  err, errlen) != 0) {
        batch_shared_detach_steps(b, steps, sessions, attached, n_steps, false);
        batch_fail_steps(b, steps, sessions, n_steps);
        return 1;
    }
    batch_shared_detach_steps(b, steps, sessions, attached, n_steps, true);
    for (int i = 0; i < n_steps; i++) {
        if (top_tokens[i] < 0) {
            batch_set_err(err, errlen, "batch top-token read failed");
            batch_fail_steps(b, steps, sessions, n_steps);
            return 1;
        }
        batch_finish_top_step(states[i]);
    }
    return 0;
}

int ds4_batch_prefill(ds4_batch *b, const ds4_batch_step *steps,
                      const int *refresh_logits, int n_steps,
                      char *err, size_t errlen) {
    if (!refresh_logits) return ds4_batch_eval(b, steps, n_steps, err, errlen);
    ds4_session *sessions[DS4_BATCH_MAX_SLOTS] = {0};
    ds4_batch_slot_state *states[DS4_BATCH_MAX_SLOTS] = {0};
    if (batch_validate_steps(b, steps, n_steps, sessions, states, err, errlen) != 0) {
        return 1;
    }
    bool attached[DS4_BATCH_MAX_SLOTS] = {0};
    if (batch_shared_attach_steps(b, steps, sessions, attached, n_steps, err, errlen) != 0) {
        batch_fail_steps(b, steps, sessions, n_steps);
        return 1;
    }
    batch_steps_in_flight(states, n_steps, true);
    for (int i = 0; i < n_steps; i++) {
        if (refresh_logits[i]) {
            if (ds4_session_eval(sessions[i], steps[i].token, err, errlen) != 0) {
                batch_shared_detach_steps(b, steps, sessions, attached, n_steps, false);
                batch_fail_steps(b, steps, sessions, n_steps);
                return 1;
            }
            batch_finish_full_step(states[i]);
        } else {
            int top_token = -1;
            if (ds4_session_eval_top(sessions[i], steps[i].token, &top_token,
                                     err, errlen) != 0) {
                batch_shared_detach_steps(b, steps, sessions, attached, n_steps, false);
                batch_fail_steps(b, steps, sessions, n_steps);
                return 1;
            }
            batch_finish_top_step(states[i]);
        }
    }
    batch_shared_detach_steps(b, steps, sessions, attached, n_steps, true);
    return 0;
}

int ds4_batch_prefill_segments(ds4_batch *b,
                               const ds4_batch_prefill_segment *segments,
                               int n_segments, char *err, size_t errlen) {
    if (!b || (!segments && n_segments > 0) ||
        n_segments < 0 || n_segments > b->max_slots) {
        batch_set_err(err, errlen, "invalid batch prefill request");
        return 1;
    }
    bool seen[DS4_BATCH_MAX_SLOTS] = {0};
    ds4_session *sessions[DS4_BATCH_MAX_SLOTS] = {0};
    ds4_batch_slot_state *states[DS4_BATCH_MAX_SLOTS] = {0};
    for (int i = 0; i < n_segments; i++) {
        const int slot = segments[i].slot;
        if (slot < 0 || slot >= b->max_slots || seen[slot] ||
            segments[i].n_tokens <= 0 || !segments[i].tokens) {
            batch_set_err(err, errlen, "invalid batch prefill segment");
            return 1;
        }
        seen[slot] = true;
        if (batch_require_slot(b, slot, &sessions[i], &states[i], err, errlen) != 0) return 1;
        if (states[i]->error) {
            batch_set_err(err, errlen, "batch slot is in an error state");
            return 1;
        }
        if (ds4_session_pos(sessions[i]) + segments[i].n_tokens > ds4_session_ctx(sessions[i])) {
            batch_set_err(err, errlen, "batch prefill segment exceeds context");
            return 1;
        }
    }
    bool attached[DS4_BATCH_MAX_SLOTS] = {0};
    if (batch_shared_attach_segments(b, segments, sessions, attached,
                                     n_segments, err, errlen) != 0) {
        for (int k = 0; k < n_segments; k++) {
            if (sessions[k]) ds4_session_invalidate(sessions[k]);
            batch_mark_slot_invalid(b, segments[k].slot, true);
        }
        return 1;
    }
    batch_steps_in_flight(states, n_segments, true);
    const int *segment_tokens[DS4_BATCH_MAX_SLOTS] = {0};
    int segment_lengths[DS4_BATCH_MAX_SLOTS] = {0};
    int refresh_logits[DS4_BATCH_MAX_SLOTS] = {0};
    bool equal_lengths = n_segments > 1;
    for (int i = 0; i < n_segments; i++) {
        segment_tokens[i] = segments[i].tokens;
        segment_lengths[i] = segments[i].n_tokens;
        refresh_logits[i] = segments[i].refresh_logits != 0;
        if (i > 0 && segment_lengths[i] != segment_lengths[0]) equal_lengths = false;
    }
    if (equal_lengths) {
        if (ds4_session_prefill_segments_many(sessions,
                                              segment_tokens,
                                              segment_lengths,
                                              refresh_logits,
                                              n_segments,
                                              err,
                                              errlen) != 0) {
            batch_shared_detach_segments(b, segments, sessions, attached,
                                         n_segments, false);
            for (int k = 0; k < n_segments; k++) {
                if (sessions[k]) ds4_session_invalidate(sessions[k]);
                batch_mark_slot_invalid(b, segments[k].slot, true);
            }
            return 1;
        }
    } else {
        int offsets[DS4_BATCH_MAX_SLOTS] = {0};
        int remaining = n_segments;
        while (remaining > 0) {
            ds4_session *active_sessions[DS4_BATCH_MAX_SLOTS] = {0};
            int active_tokens[DS4_BATCH_MAX_SLOTS] = {0};
            int active_index[DS4_BATCH_MAX_SLOTS] = {0};
            int active_slots[DS4_BATCH_MAX_SLOTS] = {0};
            int n_active = 0;
            int min_remaining_tokens = INT_MAX;
            for (int i = 0; i < n_segments; i++) {
                if (offsets[i] >= segments[i].n_tokens) continue;
                active_sessions[n_active] = sessions[i];
                active_tokens[n_active] = segments[i].tokens[offsets[i]];
                active_index[n_active] = i;
                active_slots[n_active] = segments[i].slot;
                n_active++;
                const int rem = segments[i].n_tokens - offsets[i];
                if (rem < min_remaining_tokens) min_remaining_tokens = rem;
            }
            if (n_active <= 0) break;
            if (b->backend == DS4_BATCH_BACKEND_SHARED_DECODE &&
                n_active > 1 && min_remaining_tokens > 1) {
                const int *active_segment_tokens[DS4_BATCH_MAX_SLOTS] = {0};
                int active_segment_lengths[DS4_BATCH_MAX_SLOTS] = {0};
                int active_refresh_logits[DS4_BATCH_MAX_SLOTS] = {0};
                for (int k = 0; k < n_active; k++) {
                    const int i = active_index[k];
                    active_segment_tokens[k] = segments[i].tokens + offsets[i];
                    active_segment_lengths[k] = min_remaining_tokens;
                    active_refresh_logits[k] =
                        segments[i].refresh_logits &&
                        offsets[i] + min_remaining_tokens == segments[i].n_tokens;
                }
                if (ds4_session_prefill_segments_many(active_sessions,
                                                      active_segment_tokens,
                                                      active_segment_lengths,
                                                      active_refresh_logits,
                                                      n_active,
                                                      err,
                                                      errlen) != 0) {
                    batch_shared_detach_segments(b, segments, sessions, attached,
                                                 n_segments, false);
                    for (int k = 0; k < n_segments; k++) {
                        if (sessions[k]) ds4_session_invalidate(sessions[k]);
                        batch_mark_slot_invalid(b, segments[k].slot, true);
                    }
                    return 1;
                }
                for (int k = 0; k < n_active; k++) {
                    const int i = active_index[k];
                    offsets[i] += min_remaining_tokens;
                    if (offsets[i] == segments[i].n_tokens) remaining--;
                }
                continue;
            }
            if (b->backend != DS4_BATCH_BACKEND_SHARED_DECODE) {
                for (int i = 1; i < n_active; i++) {
                    ds4_session *session = active_sessions[i];
                    int token = active_tokens[i];
                    int index = active_index[i];
                    int slot = active_slots[i];
                    int j = i - 1;
                    while (j >= 0 && active_slots[j] > slot) {
                        active_sessions[j + 1] = active_sessions[j];
                        active_tokens[j + 1] = active_tokens[j];
                        active_index[j + 1] = active_index[j];
                        active_slots[j + 1] = active_slots[j];
                        j--;
                    }
                    active_sessions[j + 1] = session;
                    active_tokens[j + 1] = token;
                    active_index[j + 1] = index;
                    active_slots[j + 1] = slot;
                }
            }
            int rc = 0;
            if (b->backend == DS4_BATCH_BACKEND_SHARED_DECODE) {
                for (int k = 0; rc == 0 && k < n_active; k++) {
                    rc = ds4_session_eval(active_sessions[k], active_tokens[k],
                                          err, errlen);
                }
            } else {
                rc = ds4_session_eval_many(active_sessions, active_tokens, n_active,
                                           err, errlen);
            }
            if (rc != 0) {
                batch_shared_detach_segments(b, segments, sessions, attached,
                                             n_segments, false);
                for (int k = 0; k < n_segments; k++) {
                    if (sessions[k]) ds4_session_invalidate(sessions[k]);
                    batch_mark_slot_invalid(b, segments[k].slot, true);
                }
                return 1;
            }
            for (int k = 0; k < n_active; k++) {
                const int i = active_index[k];
                offsets[i]++;
                if (offsets[i] == segments[i].n_tokens) remaining--;
            }
        }
    }
    batch_shared_detach_segments(b, segments, sessions, attached, n_segments, true);
    for (int i = 0; i < n_segments; i++) {
        if (segments[i].refresh_logits) batch_finish_full_step(states[i]);
        else batch_finish_top_step(states[i]);
    }
    return 0;
}

void ds4_batch_invalidate_slot(ds4_batch *b, int slot) {
    ds4_session *s = batch_slot(b, slot);
    if (s) ds4_session_invalidate(s);
    batch_mark_slot_invalid(b, slot, false);
}

void ds4_batch_rewind_slot(ds4_batch *b, int slot, int pos) {
    ds4_session *s = batch_slot(b, slot);
    if (s) ds4_session_rewind(s, pos);
    batch_mark_slot_invalid(b, slot, false);
}

int ds4_batch_pos(ds4_batch *b, int slot) {
    ds4_session *s = batch_slot(b, slot);
    return s ? ds4_session_pos(s) : 0;
}

const ds4_tokens *ds4_batch_tokens(ds4_batch *b, int slot) {
    ds4_session *s = batch_slot(b, slot);
    return s ? ds4_session_tokens(s) : NULL;
}

uint64_t ds4_batch_slot_payload_bytes(ds4_batch *b, int slot) {
    ds4_session *s = batch_slot(b, slot);
    if (!s || !ds4_batch_slot_can_save(b, slot)) return 0;
    if (batch_shared_attach_slot(b, slot, s, NULL, 0) != 0) return 0;
    uint64_t bytes = ds4_session_payload_bytes(s);
    batch_shared_detach_slot(b, slot, s, false);
    return bytes;
}

int ds4_batch_save_slot_payload(ds4_batch *b, int slot, FILE *fp,
                                char *err, size_t errlen) {
    ds4_session *s = batch_slot(b, slot);
    if (!s || !fp) {
        batch_set_err(err, errlen, "invalid batch slot payload save");
        return 1;
    }
    if (!ds4_batch_slot_can_save(b, slot)) {
        batch_set_err(err, errlen, "batch slot has no checkpoint-safe payload to save");
        return 1;
    }
    if (batch_shared_attach_slot(b, slot, s, err, errlen) != 0) return 1;
    int rc = ds4_session_save_payload(s, fp, err, errlen);
    batch_shared_detach_slot(b, slot, s, false);
    return rc;
}

int ds4_batch_load_slot_payload(ds4_batch *b, int slot, FILE *fp,
                                uint64_t payload_bytes,
                                char *err, size_t errlen) {
    ds4_session *s = batch_slot(b, slot);
    if (!s || !fp) {
        batch_set_err(err, errlen, "invalid batch slot payload load");
        return 1;
    }
    ds4_batch_slot_state *st = batch_state(b, slot);
    if (st) st->eval_in_flight = true;
    if (batch_shared_attach_slot(b, slot, s, err, errlen) != 0) {
        if (st) batch_mark_slot_invalid(b, slot, true);
        return 1;
    }
    int rc = ds4_session_load_payload(s, fp, payload_bytes, err, errlen);
    batch_shared_detach_slot(b, slot, s, rc == 0);
    if (st) {
        st->eval_in_flight = false;
        if (rc == 0) {
            st->logits_valid = true;
            st->payload_valid = true;
            st->error = false;
            st->needs_rebuild = false;
        } else {
            batch_mark_slot_invalid(b, slot, true);
        }
    }
    return rc;
}

int ds4_batch_save_slot_snapshot(ds4_batch *b, int slot,
                                 ds4_session_snapshot *snap,
                                 char *err, size_t errlen) {
    ds4_session *s = batch_slot(b, slot);
    if (!s || !snap) {
        batch_set_err(err, errlen, "invalid batch slot snapshot save");
        return 1;
    }
    if (!ds4_batch_slot_can_save(b, slot)) {
        batch_set_err(err, errlen, "batch slot has no checkpoint-safe snapshot to save");
        return 1;
    }
    if (batch_shared_attach_slot(b, slot, s, err, errlen) != 0) return 1;
    int rc = ds4_session_save_snapshot(s, snap, err, errlen);
    batch_shared_detach_slot(b, slot, s, false);
    return rc;
}

int ds4_batch_load_slot_snapshot(ds4_batch *b, int slot,
                                 const ds4_session_snapshot *snap,
                                 char *err, size_t errlen) {
    ds4_session *s = NULL;
    ds4_batch_slot_state *st = NULL;
    if (batch_require_slot(b, slot, &s, &st, err, errlen) != 0) return 1;
    if (!snap || !snap->ptr || snap->len == 0) {
        batch_set_err(err, errlen, "invalid batch slot snapshot load");
        return 1;
    }
    st->eval_in_flight = true;
    if (batch_shared_attach_slot(b, slot, s, err, errlen) != 0) {
        batch_mark_slot_invalid(b, slot, true);
        return 1;
    }
    int rc = ds4_session_load_snapshot(s, snap, err, errlen);
    batch_shared_detach_slot(b, slot, s, rc == 0);
    st->eval_in_flight = false;
    if (rc == 0) {
        st->logits_valid = true;
        st->payload_valid = true;
        st->error = false;
        st->needs_rebuild = false;
    } else {
        batch_mark_slot_invalid(b, slot, true);
    }
    return rc;
}
