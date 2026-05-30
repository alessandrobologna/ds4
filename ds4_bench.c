#include "ds4.h"
#include "ds4_distributed.h"

/* Purpose-built throughput benchmark.
 *
 * The benchmark walks one fixed token sequence to configurable context
 * frontiers, measuring only the newest prefill interval at each frontier.  It
 * then snapshots the live session in memory, performs a fixed greedy decode
 * probe, restores the snapshot, and continues to the next frontier. Snapshot
 * save/restore time is intentionally outside both timing windows but is
 * reported separately.
 */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *model_path;
    const char *prompt_path;
    const char *chat_prompt_path;
    const char *system;
    const char *csv_path;
    ds4_backend backend;
    int threads;
    int ctx_start;
    int ctx_max;
    int ctx_alloc;
    int step_incr;
    int gen_tokens;
    int power_percent;
    int batch_sessions;
    int batch_prefill_rows;
    double step_mul;
    const char *dump_frontier_logits_dir;
    ds4_dist_options dist;
    const char *batch_backend;
    bool batch_common_prefix;
    bool decode_top;
    bool warm_weights;
    bool quality;
} bench_config;

static double bench_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void usage(FILE *fp) {
    fprintf(fp,
        "Usage: ds4-bench --prompt-file FILE [options]\n"
        "\n"
        "Benchmarks instantaneous prefill and generation throughput at context\n"
        "frontiers such as 2048, 4096, 6144, ... . Generation is always greedy,\n"
        "runs for exactly --gen-tokens tokens. The default full-logits path\n"
        "skips EOS so every row is comparable.\n"
        "\n"
        "Input:\n"
        "  --prompt-file FILE\n"
        "      Raw benchmark text. The fixed token sequence is sliced at each frontier.\n"
        "  --chat-prompt-file FILE\n"
        "      Render FILE as one no-thinking chat user message, then slice that sequence.\n"
        "  -sys, --system TEXT\n"
        "      System prompt used only with --chat-prompt-file.\n"
        "\n"
        "Model and backend:\n"
        "  -m, --model FILE       GGUF model path. Default: ds4flash.gguf\n"
        "  --metal | --cuda | --cpu | --backend NAME\n"
        "      Select backend explicitly. Defaults to Metal on macOS, CUDA elsewhere.\n"
        "  -t, --threads N        CPU helper threads.\n"
        "  --quality              Prefer exact kernels where applicable.\n"
        "  --warm-weights         Touch mapped tensor pages before benchmarking.\n"
        "  --power N              Target GPU duty cycle percentage, 1..100. Default: 100\n"
        "\n"
        "Distributed:\n");
    ds4_dist_usage(fp);
    fprintf(fp,
        "\n"
        "\n"
        "Sweep:\n"
        "  --ctx-start N          First measured frontier. Default: 2048\n"
        "  --ctx-max N            Last measured frontier. Default: 32768\n"
        "  --ctx-alloc N          Allocated context. Default: ctx-max + gen-tokens + 1\n"
        "  --step-mul F           Multiplicative step. Default: 1\n"
        "  --step-incr N          Linear step when --step-mul is 1. Default: 2048\n"
        "  --gen-tokens N         Greedy decode tokens per frontier. Use 0 for pure prefill. Default: 128\n"
        "  --decode-top\n"
        "      Use the top-token decode API during generation. The first token\n"
        "      still skips EOS; subsequent tokens come from the backend top-1 path.\n"
        "\n"
        "Batch sweep:\n"
        "  --batch-sessions N     Run a multi-session sweep instead of the single-session sweep.\n"
        "  --batch-backend NAME   Batch backend for --batch-sessions. Default: shared-decode\n"
        "      Valid names are session-slots and shared-decode.\n"
        "  --batch-prefill-rows N\n"
        "      Shared-decode prefill scratch rows. Default: auto/DS4_BATCH_PREFILL_ROW_CAP.\n"
        "  --batch-common-prefix\n"
        "      Use the same prompt prefix for every batch slot instead of distinct slices.\n"
        "\n"
        "Output:\n"
        "  --csv FILE             Write CSV there instead of stdout.\n"
        "  --dump-frontier-logits-dir DIR\n"
        "      Write one full-logit JSON file per measured frontier. DIR must exist.\n"
        "  -h, --help             Show this help.\n");
}

static int parse_int(const char *s, const char *opt) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (s[0] == '\0' || *end != '\0' || v <= 0 || v > INT_MAX) {
        fprintf(stderr, "ds4-bench: invalid value for %s: %s\n", opt, s);
        exit(2);
    }
    return (int)v;
}

static int parse_nonnegative_int(const char *s, const char *opt) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (s[0] == '\0' || *end != '\0' || v < 0 || v > INT_MAX) {
        fprintf(stderr, "ds4-bench: invalid value for %s: %s\n", opt, s);
        exit(2);
    }
    return (int)v;
}

static double parse_double_arg(const char *s, const char *opt) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (s[0] == '\0' || *end != '\0' || !isfinite(v)) {
        fprintf(stderr, "ds4-bench: invalid value for %s: %s\n", opt, s);
        exit(2);
    }
    return v;
}

static const char *need_arg(int *i, int argc, char **argv, const char *opt) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "ds4-bench: %s requires an argument\n", opt);
        exit(2);
    }
    return argv[++*i];
}

static ds4_backend parse_backend(const char *s, const char *opt) {
    if (!strcmp(s, "metal")) return DS4_BACKEND_METAL;
    if (!strcmp(s, "cuda")) return DS4_BACKEND_CUDA;
    if (!strcmp(s, "cpu")) return DS4_BACKEND_CPU;
    fprintf(stderr, "ds4-bench: invalid value for %s: %s\n", opt, s);
    fprintf(stderr, "ds4-bench: valid backends are: metal, cuda, cpu\n");
    exit(2);
}

static ds4_backend default_backend(void) {
#ifdef DS4_NO_GPU
    return DS4_BACKEND_CPU;
#elif defined(__APPLE__)
    return DS4_BACKEND_METAL;
#else
    return DS4_BACKEND_CUDA;
#endif
}

static char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "ds4-bench: failed to open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "ds4-bench: failed to seek %s\n", path);
        fclose(fp);
        exit(1);
    }
    long n = ftell(fp);
    if (n < 0) {
        fprintf(stderr, "ds4-bench: failed to tell %s\n", path);
        fclose(fp);
        exit(1);
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "ds4-bench: failed to rewind %s\n", path);
        fclose(fp);
        exit(1);
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fprintf(stderr, "ds4-bench: out of memory reading %s\n", path);
        fclose(fp);
        exit(1);
    }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        fprintf(stderr, "ds4-bench: failed to read %s\n", path);
        free(buf);
        fclose(fp);
        exit(1);
    }
    fclose(fp);
    buf[n] = '\0';
    return buf;
}

static bench_config parse_options(int argc, char **argv) {
    bench_config c = {
        .model_path = "ds4flash.gguf",
        .system = "You are a helpful assistant.",
        .backend = default_backend(),
        .ctx_start = 2048,
        .ctx_max = 32768,
        .step_incr = 2048,
        .gen_tokens = 128,
        .batch_sessions = 1,
        .step_mul = 1.0,
        .batch_backend = "shared-decode",
    };

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(stdout);
            exit(0);
        }
        char dist_parse_err[256] = {0};
        ds4_dist_cli_parse_result dist_parse =
            ds4_dist_parse_cli_arg(arg,
                                   &i,
                                   argc,
                                   argv,
                                   &c.dist,
                                   dist_parse_err,
                                   sizeof(dist_parse_err));
        if (dist_parse == DS4_DIST_CLI_ERROR) {
            fprintf(stderr,
                    "ds4-bench: %s\n",
                    dist_parse_err[0] ? dist_parse_err : "invalid distributed option");
            exit(2);
        }
        if (dist_parse == DS4_DIST_CLI_MATCHED) continue;

        if (!strcmp(arg, "-m") || !strcmp(arg, "--model")) {
            c.model_path = need_arg(&i, argc, argv, arg);
        } else if (!strcmp(arg, "--prompt-file")) {
            c.prompt_path = need_arg(&i, argc, argv, arg);
        } else if (!strcmp(arg, "--chat-prompt-file")) {
            c.chat_prompt_path = need_arg(&i, argc, argv, arg);
        } else if (!strcmp(arg, "-sys") || !strcmp(arg, "--system")) {
            c.system = need_arg(&i, argc, argv, arg);
        } else if (!strcmp(arg, "--ctx-start")) {
            c.ctx_start = parse_int(need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--ctx-max")) {
            c.ctx_max = parse_int(need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--ctx-alloc")) {
            c.ctx_alloc = parse_int(need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--step-incr")) {
            c.step_incr = parse_int(need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--step-mul")) {
            c.step_mul = parse_double_arg(need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--gen-tokens") || !strcmp(arg, "--tokens") || !strcmp(arg, "-n")) {
            c.gen_tokens = parse_nonnegative_int(need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--batch-sessions")) {
            c.batch_sessions = parse_int(need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--batch-backend")) {
            c.batch_backend = need_arg(&i, argc, argv, arg);
        } else if (!strcmp(arg, "--batch-prefill-rows")) {
            c.batch_prefill_rows = parse_int(need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--batch-common-prefix")) {
            c.batch_common_prefix = true;
        } else if (!strcmp(arg, "--decode-top")) {
            c.decode_top = true;
        } else if (!strcmp(arg, "--csv")) {
            c.csv_path = need_arg(&i, argc, argv, arg);
        } else if (!strcmp(arg, "--dump-frontier-logits-dir")) {
            c.dump_frontier_logits_dir = need_arg(&i, argc, argv, arg);
        } else if (!strcmp(arg, "-t") || !strcmp(arg, "--threads")) {
            c.threads = parse_int(need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--backend")) {
            c.backend = parse_backend(need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--metal")) {
            c.backend = DS4_BACKEND_METAL;
        } else if (!strcmp(arg, "--cuda")) {
            c.backend = DS4_BACKEND_CUDA;
        } else if (!strcmp(arg, "--cpu")) {
            c.backend = DS4_BACKEND_CPU;
        } else if (!strcmp(arg, "--quality")) {
            c.quality = true;
        } else if (!strcmp(arg, "--power")) {
            c.power_percent = parse_int(need_arg(&i, argc, argv, arg), arg);
            if (c.power_percent < 1 || c.power_percent > 100) {
                fprintf(stderr, "ds4-bench: --power must be between 1 and 100\n");
                exit(2);
            }
        } else if (!strcmp(arg, "--warm-weights")) {
            c.warm_weights = true;
        } else {
            fprintf(stderr, "ds4-bench: unknown option: %s\n", arg);
            usage(stderr);
            exit(2);
        }
    }

    if (!!c.prompt_path == !!c.chat_prompt_path) {
        fprintf(stderr, "ds4-bench: specify exactly one of --prompt-file or --chat-prompt-file\n");
        exit(2);
    }
    if (c.ctx_start > c.ctx_max) {
        fprintf(stderr, "ds4-bench: --ctx-start must be <= --ctx-max\n");
        exit(2);
    }
    if (c.step_mul < 1.0) {
        fprintf(stderr, "ds4-bench: --step-mul must be >= 1\n");
        exit(2);
    }
    if (c.step_mul == 1.0 && c.step_incr <= 0) {
        fprintf(stderr, "ds4-bench: --step-incr must be positive when --step-mul is 1\n");
        exit(2);
    }
    if (c.ctx_max > INT_MAX - c.gen_tokens - 1) {
        fprintf(stderr, "ds4-bench: requested context is too large\n");
        exit(2);
    }
    if (c.ctx_alloc == 0) c.ctx_alloc = c.ctx_max + c.gen_tokens + 1;
    if (c.ctx_alloc <= c.ctx_max + c.gen_tokens) {
        fprintf(stderr, "ds4-bench: --ctx-alloc must be greater than ctx-max + gen-tokens\n");
        exit(2);
    }
    if (c.batch_sessions < 1 || c.batch_sessions > DS4_BATCH_MAX_SLOTS) {
        fprintf(stderr, "ds4-bench: --batch-sessions must be between 1 and %d\n",
                DS4_BATCH_MAX_SLOTS);
        exit(2);
    }
    if (c.batch_sessions > 1 && c.chat_prompt_path) {
        fprintf(stderr, "ds4-bench: --batch-sessions currently requires --prompt-file\n");
        exit(2);
    }
    ds4_batch_backend batch_backend = DS4_BATCH_BACKEND_SESSION_SLOTS;
    if (!ds4_batch_backend_from_name(c.batch_backend, &batch_backend)) {
        fprintf(stderr, "ds4-bench: invalid --batch-backend value: %s\n", c.batch_backend);
        fprintf(stderr, "ds4-bench: valid batch backends are: session-slots, shared-decode\n");
        exit(2);
    }
    char dist_err[256];
    if (ds4_dist_prepare_engine_options(&c.dist, NULL, dist_err, sizeof(dist_err)) != 0) {
        fprintf(stderr, "ds4-bench: %s\n", dist_err);
        exit(2);
    }
    if (c.dist.role == DS4_DISTRIBUTED_WORKER) {
        fprintf(stderr, "ds4-bench: --role worker is a serving mode; start workers with ./ds4\n");
        exit(2);
    }
    return c;
}

static void json_write_string(FILE *fp, const char *s) {
    fputc('"', fp);
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\b': fputs("\\b", fp); break;
            case '\f': fputs("\\f", fp); break;
            case '\n': fputs("\\n", fp); break;
            case '\r': fputs("\\r", fp); break;
            case '\t': fputs("\\t", fp); break;
            default:
                if (*p < 0x20) fprintf(fp, "\\u%04x", (unsigned)*p);
                else fputc((char)*p, fp);
                break;
            }
        }
    }
    fputc('"', fp);
}

static int write_frontier_logits_json(
        const bench_config *cfg,
        ds4_engine         *engine,
        ds4_session        *session,
        int                 frontier,
        int                 previous) {
    if (!cfg->dump_frontier_logits_dir) return 0;

    const int vocab = ds4_engine_vocab_size(engine);
    float *logits = malloc((size_t)vocab * sizeof(logits[0]));
    if (!logits) {
        fprintf(stderr, "ds4-bench: out of memory copying frontier logits\n");
        return 1;
    }
    if (ds4_session_copy_logits(session, logits, vocab) != vocab) {
        fprintf(stderr, "ds4-bench: failed to copy frontier logits at %d\n", frontier);
        free(logits);
        return 1;
    }

    char path[PATH_MAX];
    const int n = snprintf(path,
                           sizeof(path),
                           "%s/frontier_%06d.logits.json",
                           cfg->dump_frontier_logits_dir,
                           frontier);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        fprintf(stderr, "ds4-bench: frontier logits path is too long\n");
        free(logits);
        return 1;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "ds4-bench: failed to open %s: %s\n", path, strerror(errno));
        free(logits);
        return 1;
    }

    const int argmax = ds4_session_argmax(session);
    fprintf(fp, "{\n  \"source\":\"ds4-bench\",\n  \"model\":");
    json_write_string(fp, cfg->model_path);
    fprintf(fp,
            ",\n  \"backend\":\"%s\",\n  \"quality\":%s,\n"
            "  \"quant_bits\":%d,\n  \"prompt_tokens\":%d,\n"
            "  \"frontier_tokens\":%d,\n  \"prefill_tokens\":%d,\n"
            "  \"ctx\":%d,\n  \"vocab\":%d,\n"
            "  \"argmax_id\":%d,\n  \"argmax_logit\":%.9g,\n  \"logits\":[",
            ds4_backend_name(cfg->backend),
            cfg->quality ? "true" : "false",
            ds4_engine_routed_quant_bits(engine),
            frontier,
            frontier,
            frontier - previous,
            cfg->ctx_alloc,
            vocab,
            argmax,
            logits[argmax]);
    for (int i = 0; i < vocab; i++) {
        if (i) fputc(',', fp);
        if ((i % 8) == 0) fputs("\n    ", fp);
        if (isfinite(logits[i])) fprintf(fp, "%.9g", logits[i]);
        else fputs("null", fp);
    }
    fputs("\n  ]\n}\n", fp);
    if (fclose(fp) != 0) {
        fprintf(stderr, "ds4-bench: failed to close %s\n", path);
        free(logits);
        return 1;
    }
    free(logits);
    return 0;
}

static int next_frontier(const bench_config *c, int cur) {
    if (cur >= c->ctx_max) return c->ctx_max;
    int next;
    if (c->step_mul == 1.0) {
        if (cur > INT_MAX - c->step_incr) next = c->ctx_max;
        else next = cur + c->step_incr;
    } else {
        const double v = ceil((double)cur * c->step_mul);
        next = v > (double)INT_MAX ? c->ctx_max : (int)v;
        if (next <= cur) next = cur + 1;
    }
    if (next > c->ctx_max) next = c->ctx_max;
    return next;
}

static void log_context_memory(ds4_backend backend, int ctx_size) {
    ds4_context_memory m = ds4_context_memory_estimate(backend, ctx_size);
    fprintf(stderr,
            "ds4-bench: context buffers %.2f MiB (ctx=%d, backend=%s, prefill_chunk=%u, raw_kv_rows=%u, compressed_kv_rows=%u)\n",
            (double)m.total_bytes / (1024.0 * 1024.0),
            ctx_size,
            ds4_backend_name(backend),
            m.prefill_cap,
            m.raw_cap,
            m.comp_cap);
}

static int wait_distributed_route(ds4_session *session) {
    char err[256] = {0};
    char last[256] = {0};
    unsigned ticks = 0;
    const struct timespec delay = {0, 250000000L};

    for (;;) {
        int ready = ds4_session_distributed_route_ready(session, err, sizeof(err));
        if (ready > 0) {
            if (ticks) fprintf(stderr, "ds4-bench: distributed route ready\n");
            return 0;
        }
        if (ready < 0) {
            fprintf(stderr,
                    "ds4-bench: distributed route readiness failed: %s\n",
                    err[0] ? err : "unknown error");
            return 1;
        }
        const char *why = err[0] ? err : "route incomplete";
        if (strcmp(last, why) != 0 || (ticks % 20u) == 0) {
            fprintf(stderr, "ds4-bench: waiting for distributed route: %s\n", why);
            snprintf(last, sizeof(last), "%s", why);
        }
        nanosleep(&delay, NULL);
        ticks++;
    }
}

static void maybe_warn_distributed_step_shape(const bench_config *cfg, ds4_session *session) {
    if (!cfg || !session || cfg->dist.role != DS4_DISTRIBUTED_COORDINATOR) return;
    uint32_t chunk = cfg->dist.prefill_chunk;
    if (chunk == 0) {
        const int cap = ds4_session_prefill_cap(session);
        if (cap > 0) chunk = (uint32_t)cap;
    }
    if (chunk == 0) return;
    if (cfg->step_mul == 1.0 &&
        cfg->step_incr > 0 &&
        (uint32_t)cfg->step_incr < chunk &&
        cfg->ctx_start < cfg->ctx_max)
    {
        fprintf(stderr,
                "ds4-bench: note: --step-incr=%d is smaller than distributed prefill chunk %u; "
                "suffix rows will not show multi-chunk pipeline overlap\n",
                cfg->step_incr,
                chunk);
    }
}

static void bench_close_sessions(ds4_session **sessions, int n_sessions) {
    if (!sessions) return;
    for (int i = 0; i < n_sessions; i++) ds4_session_free(sessions[i]);
    free(sessions);
}

static void bench_free_snapshots(ds4_session_snapshot *snap, int n_snap) {
    if (!snap) return;
    for (int i = 0; i < n_snap; i++) ds4_session_snapshot_free(&snap[i]);
    free(snap);
}

static int bench_open_output(const bench_config *cfg, FILE **out) {
    *out = stdout;
    if (!cfg->csv_path) return 0;
    *out = fopen(cfg->csv_path, "wb");
    if (!*out) {
        fprintf(stderr, "ds4-bench: failed to open %s: %s\n",
                cfg->csv_path, strerror(errno));
        return 1;
    }
    return 0;
}

static int bench_batch_slice_starts(
        const bench_config *cfg,
        const ds4_tokens   *prompt,
        int                *starts) {
    if (cfg->batch_common_prefix) {
        for (int i = 0; i < cfg->batch_sessions; i++) starts[i] = 0;
        return 0;
    }

    const int max_start = prompt->len - cfg->ctx_max;
    if (max_start < cfg->batch_sessions - 1) {
        fprintf(stderr,
                "ds4-bench: prompt has %d tokens, need enough room for %d distinct "
                "slices of ctx=%d\n",
                prompt->len,
                cfg->batch_sessions,
                cfg->ctx_max);
        return 1;
    }
    for (int i = 0; i < cfg->batch_sessions; i++) {
        starts[i] = cfg->batch_sessions == 1 ? 0 :
            (int)(((int64_t)max_start * i) / (cfg->batch_sessions - 1));
    }
    return 0;
}

static const char *bench_batch_slice_label(const bench_config *cfg) {
    return cfg->batch_common_prefix ? "common-prefix" : "distinct-slices";
}

static const char *bench_decode_label(const bench_config *cfg) {
    return cfg->decode_top ? "top-token" : "full-logits";
}

static int bench_serialized_multisession(
        ds4_engine         *engine,
        const bench_config *cfg,
        const ds4_tokens   *prompt,
        const int          *starts,
        FILE               *out,
        char               *err,
        size_t              errlen) {
    ds4_session **sessions = calloc((size_t)cfg->batch_sessions, sizeof(*sessions));
    ds4_session_snapshot *snap =
        calloc((size_t)cfg->batch_sessions, sizeof(*snap));
    if (!sessions || !snap) {
        fprintf(stderr, "ds4-bench: out of memory creating serialized sessions\n");
        free(sessions);
        bench_free_snapshots(snap, cfg->batch_sessions);
        return 1;
    }
    for (int i = 0; i < cfg->batch_sessions; i++) {
        if (ds4_session_create(&sessions[i], engine, cfg->ctx_alloc) != 0) {
            fprintf(stderr, "ds4-bench: failed to create serialized session %d\n", i);
            bench_close_sessions(sessions, cfg->batch_sessions);
            bench_free_snapshots(snap, cfg->batch_sessions);
            return 1;
        }
    }

    const int eos = ds4_token_eos(engine);
    int previous = 0;
    int rc = 0;
    for (int frontier = cfg->ctx_start; ; frontier = next_frontier(cfg, frontier)) {
        const double prefill_t0 = bench_now_sec();
        for (int i = 0; i < cfg->batch_sessions; i++) {
            ds4_tokens prefix = {
                .v = prompt->v + starts[i],
                .len = frontier,
                .cap = frontier,
            };
            if (ds4_session_sync(sessions[i], &prefix, err, errlen) != 0) {
                fprintf(stderr, "ds4-bench: serialized prefill slot %d to %d failed: %s\n",
                        i, frontier, err);
                rc = 1;
                break;
            }
        }
        const double prefill_t1 = bench_now_sec();
        if (rc != 0) break;

        uint64_t snapshot_bytes = 0;
        double snapshot_s = 0.0;
        const double snapshot_save_t0 = bench_now_sec();
        for (int i = 0; i < cfg->batch_sessions; i++) {
            if (ds4_session_save_snapshot(sessions[i], &snap[i], err, errlen) != 0) {
                fprintf(stderr, "ds4-bench: serialized snapshot slot %d at %d failed: %s\n",
                        i, frontier, err);
                rc = 1;
                break;
            }
            snapshot_bytes += snap[i].len;
        }
        snapshot_s += bench_now_sec() - snapshot_save_t0;
        if (rc != 0) break;

        const double gen_t0 = bench_now_sec();
        for (int i = 0; i < cfg->batch_sessions && rc == 0; i++) {
            int next_token = -1;
            for (int j = 0; j < cfg->gen_tokens; j++) {
                if (ds4_session_pos(sessions[i]) + 1 >= ds4_session_ctx(sessions[i])) {
                    fprintf(stderr, "ds4-bench: serialized generation would exceed context at frontier %d\n",
                            frontier);
                    rc = 1;
                    break;
                }
                const int token = cfg->decode_top && next_token >= 0 ?
                    next_token : ds4_session_argmax_excluding(sessions[i], eos);
                if (token < 0) {
                    fprintf(stderr, "ds4-bench: serialized argmax failed at frontier %d slot %d\n",
                            frontier, i);
                    rc = 1;
                    break;
                }
                if (cfg->decode_top) {
                    if (ds4_session_eval_top(sessions[i], token, &next_token,
                                             err, errlen) != 0) {
                        fprintf(stderr, "ds4-bench: serialized top-token decode failed at frontier %d slot %d: %s\n",
                                frontier, i, err);
                        rc = 1;
                        break;
                    }
                } else if (ds4_session_eval(sessions[i], token, err, errlen) != 0) {
                    fprintf(stderr, "ds4-bench: serialized decode failed at frontier %d slot %d: %s\n",
                            frontier, i, err);
                    rc = 1;
                    break;
                }
            }
        }
        const double gen_t1 = bench_now_sec();
        if (rc != 0) break;

        const double snapshot_load_t0 = bench_now_sec();
        for (int i = 0; i < cfg->batch_sessions; i++) {
            if (ds4_session_load_snapshot(sessions[i], &snap[i], err, errlen) != 0) {
                fprintf(stderr, "ds4-bench: serialized restore slot %d at %d failed: %s\n",
                        i, frontier, err);
                rc = 1;
                break;
            }
        }
        snapshot_s += bench_now_sec() - snapshot_load_t0;
        if (rc != 0) break;

        const int prefill_tokens = (frontier - previous) * cfg->batch_sessions;
        const int gen_tokens = cfg->gen_tokens * cfg->batch_sessions;
        const double prefill_sec = prefill_t1 - prefill_t0;
        const double gen_sec = gen_t1 - gen_t0;
        fprintf(out,
                "serialized,%s,%d,%d,%d,%d,%.2f,%d,%.2f,%llu,%s,%.6f\n",
                bench_batch_slice_label(cfg),
                cfg->batch_sessions,
                frontier,
                frontier - previous,
                prefill_tokens,
                prefill_sec > 0.0 ? (double)prefill_tokens / prefill_sec : 0.0,
                gen_tokens,
                gen_sec > 0.0 ? (double)gen_tokens / gen_sec : 0.0,
                (unsigned long long)snapshot_bytes,
                bench_decode_label(cfg),
                snapshot_s);
        fflush(out);

        previous = frontier;
        if (frontier >= cfg->ctx_max) break;
    }

    bench_close_sessions(sessions, cfg->batch_sessions);
    bench_free_snapshots(snap, cfg->batch_sessions);
    return rc;
}

static int bench_batch_multisession(
        ds4_engine         *engine,
        const bench_config *cfg,
        const ds4_tokens   *prompt,
        const int          *starts,
        FILE               *out,
        char               *err,
        size_t              errlen) {
    ds4_batch_backend backend = DS4_BATCH_BACKEND_SESSION_SLOTS;
    (void)ds4_batch_backend_from_name(cfg->batch_backend, &backend);
    ds4_batch_options opt = {
        .ctx_size = cfg->ctx_alloc,
        .max_slots = cfg->batch_sessions,
        .backend = backend,
        .prefill_rows = cfg->batch_prefill_rows,
    };
    ds4_batch *batch = NULL;
    if (ds4_batch_create_with_options(&batch, engine, &opt, err, errlen) != 0) {
        fprintf(stderr, "ds4-bench: failed to create batch runtime: %s\n", err);
        return 1;
    }
    ds4_session_snapshot *snap =
        calloc((size_t)cfg->batch_sessions, sizeof(*snap));
    ds4_batch_prefill_segment *segments =
        calloc((size_t)cfg->batch_sessions, sizeof(*segments));
    ds4_batch_step *steps =
        calloc((size_t)cfg->batch_sessions, sizeof(*steps));
    int *top_tokens = calloc((size_t)cfg->batch_sessions, sizeof(*top_tokens));
    if (!snap || !segments || !steps || !top_tokens) {
        fprintf(stderr, "ds4-bench: out of memory creating batch sweep buffers\n");
        free(segments);
        free(steps);
        free(top_tokens);
        bench_free_snapshots(snap, cfg->batch_sessions);
        ds4_batch_free(batch);
        return 1;
    }
    for (int i = 0; i < cfg->batch_sessions; i++) ds4_batch_claim_slot(batch, i);

    const int eos = ds4_token_eos(engine);
    int previous = 0;
    int rc = 0;
    for (int frontier = cfg->ctx_start; ; frontier = next_frontier(cfg, frontier)) {
        const int delta = frontier - previous;
        for (int i = 0; i < cfg->batch_sessions; i++) {
            segments[i] = (ds4_batch_prefill_segment){
                .slot = i,
                .tokens = prompt->v + starts[i] + previous,
                .n_tokens = delta,
                .refresh_logits = 1,
            };
        }
        const double prefill_t0 = bench_now_sec();
        if (ds4_batch_prefill_segments(batch, segments, cfg->batch_sessions,
                                       err, errlen) != 0) {
            fprintf(stderr, "ds4-bench: batch prefill to %d failed: %s\n",
                    frontier, err);
            rc = 1;
            break;
        }
        const double prefill_t1 = bench_now_sec();

        uint64_t snapshot_bytes = 0;
        double snapshot_s = 0.0;
        const double snapshot_save_t0 = bench_now_sec();
        for (int i = 0; i < cfg->batch_sessions; i++) {
            if (ds4_batch_save_slot_snapshot(batch, i, &snap[i],
                                             err, errlen) != 0) {
                fprintf(stderr, "ds4-bench: batch snapshot slot %d at %d failed: %s\n",
                        i, frontier, err);
                rc = 1;
                break;
            }
            snapshot_bytes += snap[i].len;
        }
        snapshot_s += bench_now_sec() - snapshot_save_t0;
        if (rc != 0) break;

        for (int i = 0; i < cfg->batch_sessions; i++) top_tokens[i] = -1;
        const double gen_t0 = bench_now_sec();
        for (int j = 0; j < cfg->gen_tokens; j++) {
            for (int i = 0; i < cfg->batch_sessions; i++) {
                const int token = cfg->decode_top && top_tokens[i] >= 0 ?
                    top_tokens[i] : ds4_batch_argmax_excluding(batch, i, eos);
                if (token < 0) {
                    fprintf(stderr, "ds4-bench: batch argmax failed at frontier %d slot %d\n",
                            frontier, i);
                    rc = 1;
                    break;
                }
                steps[i] = (ds4_batch_step){ .slot = i, .token = token };
            }
            if (rc != 0) break;
            if (cfg->decode_top) {
                if (ds4_batch_eval_top(batch, steps, cfg->batch_sessions,
                                       top_tokens, err, errlen) != 0) {
                    fprintf(stderr, "ds4-bench: batch top-token decode failed at frontier %d: %s\n",
                            frontier, err);
                    rc = 1;
                    break;
                }
            } else if (ds4_batch_eval(batch, steps, cfg->batch_sessions, err, errlen) != 0) {
                fprintf(stderr, "ds4-bench: batch decode failed at frontier %d: %s\n",
                        frontier, err);
                rc = 1;
                break;
            }
        }
        const double gen_t1 = bench_now_sec();
        if (rc != 0) break;

        const double snapshot_load_t0 = bench_now_sec();
        for (int i = 0; i < cfg->batch_sessions; i++) {
            if (ds4_batch_load_slot_snapshot(batch, i, &snap[i],
                                             err, errlen) != 0) {
                fprintf(stderr, "ds4-bench: batch restore slot %d at %d failed: %s\n",
                        i, frontier, err);
                rc = 1;
                break;
            }
        }
        snapshot_s += bench_now_sec() - snapshot_load_t0;
        if (rc != 0) break;

        const int prefill_tokens = delta * cfg->batch_sessions;
        const int gen_tokens = cfg->gen_tokens * cfg->batch_sessions;
        const double prefill_sec = prefill_t1 - prefill_t0;
        const double gen_sec = gen_t1 - gen_t0;
        fprintf(out,
                "batch,%s:%s,%d,%d,%d,%d,%.2f,%d,%.2f,%llu,%s,%.6f\n",
                ds4_batch_backend_name(batch),
                bench_batch_slice_label(cfg),
                cfg->batch_sessions,
                frontier,
                delta,
                prefill_tokens,
                prefill_sec > 0.0 ? (double)prefill_tokens / prefill_sec : 0.0,
                gen_tokens,
                gen_sec > 0.0 ? (double)gen_tokens / gen_sec : 0.0,
                (unsigned long long)snapshot_bytes,
                bench_decode_label(cfg),
                snapshot_s);
        fflush(out);

        previous = frontier;
        if (frontier >= cfg->ctx_max) break;
    }

    for (int i = 0; i < cfg->batch_sessions; i++) ds4_batch_release_slot(batch, i);
    free(segments);
    free(steps);
    free(top_tokens);
    bench_free_snapshots(snap, cfg->batch_sessions);
    ds4_batch_free(batch);
    return rc;
}

static int run_batch_bench(
        ds4_engine         *engine,
        const bench_config *cfg,
        const ds4_tokens   *prompt) {
    int starts[DS4_BATCH_MAX_SLOTS] = {0};
    if (bench_batch_slice_starts(cfg, prompt, starts) != 0) return 1;

    FILE *out = NULL;
    if (bench_open_output(cfg, &out) != 0) return 1;
    fprintf(out,
            "mode,label,batch_sessions,ctx_tokens,prefill_tokens,total_prefill_tokens,prefill_tps,total_gen_tokens,gen_tps,kvcache_bytes,decode_mode,snapshot_sec\n");
    fflush(out);

    char err[256];
    int rc = bench_serialized_multisession(engine, cfg, prompt, starts,
                                           out, err, sizeof(err));
    if (rc == 0) {
        rc = bench_batch_multisession(engine, cfg, prompt, starts,
                                      out, err, sizeof(err));
    }
    if (out != stdout && fclose(out) != 0 && rc == 0) {
        fprintf(stderr, "ds4-bench: failed to close %s: %s\n",
                cfg->csv_path, strerror(errno));
        rc = 1;
    }
    return rc;
}

int main(int argc, char **argv) {
    bench_config cfg = parse_options(argc, argv);

    ds4_engine_options opt = {
        .model_path = cfg.model_path,
        .backend = cfg.backend,
        .n_threads = cfg.threads,
        .power_percent = cfg.power_percent,
        .warm_weights = cfg.warm_weights,
        .quality = cfg.quality,
        .distributed = cfg.dist,
    };
    char dist_err[256];
    if (ds4_dist_prepare_engine_options(&cfg.dist, &opt, dist_err, sizeof(dist_err)) != 0) {
        fprintf(stderr, "ds4-bench: %s\n", dist_err);
        return 2;
    }
    ds4_engine *engine = NULL;
    if (ds4_engine_open(&engine, &opt) != 0) return 1;
    log_context_memory(cfg.backend, cfg.ctx_alloc);

    char *text = read_file(cfg.prompt_path ? cfg.prompt_path : cfg.chat_prompt_path);
    ds4_tokens prompt = {0};
    if (cfg.chat_prompt_path) {
        ds4_encode_chat_prompt(engine, cfg.system, text, DS4_THINK_NONE, &prompt);
    } else {
        ds4_tokenize_text(engine, text, &prompt);
    }
    free(text);

    if (prompt.len < cfg.ctx_max) {
        fprintf(stderr,
                "ds4-bench: prompt has %d tokens, need at least --ctx-max=%d\n",
                prompt.len,
                cfg.ctx_max);
        ds4_tokens_free(&prompt);
        ds4_engine_close(engine);
        return 1;
    }

    if (cfg.batch_sessions > 1) {
        int rc = run_batch_bench(engine, &cfg, &prompt);
        ds4_tokens_free(&prompt);
        ds4_engine_close(engine);
        return rc;
    }

    ds4_session *session = NULL;
    if (ds4_session_create(&session, engine, cfg.ctx_alloc) != 0) {
        fprintf(stderr, "ds4-bench: failed to create session\n");
        ds4_tokens_free(&prompt);
        ds4_engine_close(engine);
        return 1;
    }
    if (cfg.dist.role == DS4_DISTRIBUTED_COORDINATOR &&
        wait_distributed_route(session) != 0)
    {
        ds4_session_free(session);
        ds4_tokens_free(&prompt);
        ds4_engine_close(engine);
        return 1;
    }
    maybe_warn_distributed_step_shape(&cfg, session);

    FILE *out = stdout;
    if (cfg.csv_path) {
        out = fopen(cfg.csv_path, "wb");
        if (!out) {
            fprintf(stderr, "ds4-bench: failed to open %s: %s\n", cfg.csv_path, strerror(errno));
            ds4_session_free(session);
            ds4_tokens_free(&prompt);
            ds4_engine_close(engine);
            return 1;
        }
    }
    fprintf(out, "ctx_tokens,prefill_tokens,prefill_tps,gen_tokens,gen_tps,kvcache_bytes,decode_mode,snapshot_sec\n");
    fflush(out);

    const int eos = ds4_token_eos(engine);
    const bool distributed = cfg.dist.role == DS4_DISTRIBUTED_COORDINATOR;
    ds4_session_snapshot snap = {0};
    char err[256];
    int previous = 0;
    int rc = 0;

    for (int frontier = cfg.ctx_start; ; frontier = next_frontier(&cfg, frontier)) {
        ds4_tokens prefix = {
            .v = prompt.v,
            .len = frontier,
            .cap = frontier,
        };

        const double prefill_t0 = bench_now_sec();
        if (ds4_session_sync(session, &prefix, err, sizeof(err)) != 0) {
            fprintf(stderr, "ds4-bench: prefill to %d failed: %s\n", frontier, err);
            rc = 1;
            break;
        }
        const double prefill_t1 = bench_now_sec();
        const double prefill_sec = prefill_t1 - prefill_t0;
        const int prefill_tokens = frontier - previous;

        if (write_frontier_logits_json(&cfg, engine, session, frontier, previous) != 0) {
            rc = 1;
            break;
        }

        double snapshot_s = 0.0;
        bool have_snapshot = false;
        if (cfg.gen_tokens > 0 && !distributed) {
            const double snapshot_save_t0 = bench_now_sec();
            if (ds4_session_save_snapshot(session, &snap, err, sizeof(err)) != 0) {
                fprintf(stderr, "ds4-bench: snapshot at %d failed: %s\n", frontier, err);
                rc = 1;
                break;
            }
            snapshot_s += bench_now_sec() - snapshot_save_t0;
            have_snapshot = true;
        }

        const double gen_t0 = bench_now_sec();
        int next_token = -1;
        for (int i = 0; i < cfg.gen_tokens; i++) {
            if (ds4_session_pos(session) + 1 >= ds4_session_ctx(session)) {
                fprintf(stderr, "ds4-bench: generation would exceed allocated context at frontier %d\n", frontier);
                rc = 1;
                break;
            }
            const int token = cfg.decode_top && next_token >= 0 ?
                next_token : ds4_session_argmax_excluding(session, eos);
            if (token < 0) {
                fprintf(stderr, "ds4-bench: failed to choose non-EOS token at frontier %d\n", frontier);
                rc = 1;
                break;
            }
            if (cfg.decode_top) {
                if (ds4_session_eval_top(session, token, &next_token,
                                         err, sizeof(err)) != 0) {
                    fprintf(stderr, "ds4-bench: top-token decode at frontier %d failed: %s\n", frontier, err);
                    rc = 1;
                    break;
                }
            } else if (ds4_session_eval(session, token, err, sizeof(err)) != 0) {
                fprintf(stderr, "ds4-bench: decode at frontier %d failed: %s\n", frontier, err);
                rc = 1;
                break;
            }
        }
        const double gen_t1 = bench_now_sec();
        if (rc != 0) break;

        if (cfg.gen_tokens == 0) {
            /* Pure prefill benchmark: leave the live session at the frontier. */
        } else if (distributed) {
            if (ds4_session_sync(session, &prefix, err, sizeof(err)) != 0) {
                fprintf(stderr, "ds4-bench: distributed replay restore at %d failed: %s\n", frontier, err);
                rc = 1;
                break;
            }
        } else {
            const double snapshot_load_t0 = bench_now_sec();
            if (ds4_session_load_snapshot(session, &snap, err, sizeof(err)) != 0) {
                fprintf(stderr, "ds4-bench: restore at %d failed: %s\n", frontier, err);
                rc = 1;
                break;
            }
            snapshot_s += bench_now_sec() - snapshot_load_t0;
        }

        const double gen_sec = gen_t1 - gen_t0;
        fprintf(out,
                "%d,%d,%.2f,%d,%.2f,%llu,%s,%.6f\n",
                frontier,
                prefill_tokens,
                prefill_sec > 0.0 ? (double)prefill_tokens / prefill_sec : 0.0,
                cfg.gen_tokens,
                gen_sec > 0.0 ? (double)cfg.gen_tokens / gen_sec : 0.0,
                (unsigned long long)(have_snapshot ? snap.len : 0),
                bench_decode_label(&cfg),
                snapshot_s);
        fflush(out);

        previous = frontier;
        if (frontier >= cfg.ctx_max) break;
    }

    if (out != stdout) fclose(out);
    ds4_session_snapshot_free(&snap);
    ds4_session_free(session);
    ds4_tokens_free(&prompt);
    ds4_engine_close(engine);
    return rc;
}
