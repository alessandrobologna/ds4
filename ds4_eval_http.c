#include "ds4.h"
#include "ds4_eval_common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char *v;
    size_t len;
    size_t cap;
} byte_buf;

static void buf_append(byte_buf *b, const char *p, size_t n) {
    if (n == 0) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *v = realloc(b->v, cap);
        if (!v) {
            fprintf(stderr, "ds4-eval-http: out of memory\n");
            exit(1);
        }
        b->v = v;
        b->cap = cap;
    }
    memcpy(b->v + b->len, p, n);
    b->len += n;
    b->v[b->len] = '\0';
}

static void buf_appendf(byte_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        va_end(ap);
        return;
    }
    char *tmp = malloc((size_t)n + 1);
    if (!tmp) {
        fprintf(stderr, "ds4-eval-http: out of memory\n");
        exit(1);
    }
    vsnprintf(tmp, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_append(b, tmp, (size_t)n);
    free(tmp);
}

static void buf_free(byte_buf *b) {
    free(b->v);
    memset(b, 0, sizeof(*b));
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void trace_write_block(FILE *trace, const char *label, const char *text) {
    if (!trace) return;
    size_t len = text ? strlen(text) : 0;
    fprintf(trace, "%s_BEGIN bytes=%zu\n", label, len);
    if (len) {
        fwrite(text, 1, len, trace);
        if (text[len - 1] != '\n') fputc('\n', trace);
    }
    fprintf(trace, "%s_END\n", label);
}

typedef struct {
    char host[256];
    char path[1024];
    int port;
} http_eval_url;

typedef struct {
    const char *url;
    const char *model;
    const char *trace_path;
    int max_tokens;
    int question_limit;
    int parallel;
    int timeout_ms;
    float temperature;
    float top_p;
    float min_p;
    uint64_t seed;
    bool think;
    bool stream;
} http_eval_config;

typedef struct {
    int http_status;
    int generated_bytes;
    int reasoning_bytes;
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
    int cached_tokens;
    int cache_write_tokens;
    double elapsed_sec;
    double ttft_sec;
    bool usage_present;
    bool passed;
    bool errored;
    char got[EVAL_ANSWER_MAX];
    char *output;
    char *reasoning;
    char *error;
} http_eval_result;

typedef struct {
    const http_eval_config *cfg;
    const http_eval_url *url;
    FILE *trace;
    int ncases;
    int next_case;
    int passed;
    int failed;
    int errors;
    int usage_cases;
    int ttft_cases;
    long long prompt_tokens;
    long long completion_tokens;
    long long total_tokens;
    long long cached_tokens;
    long long cache_write_tokens;
    double ttft_sum;
    double ttft_min;
    double ttft_max;
    pthread_mutex_t mu;
} http_eval_state;

static char *http_eval_strdup(const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *p = malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *http_eval_strndup(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void http_eval_set_error(char **dst, const char *fmt, ...) {
    if (!dst) return;
    free(*dst);
    *dst = NULL;

    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        va_end(ap);
        return;
    }
    char *s = malloc((size_t)n + 1);
    if (!s) {
        va_end(ap);
        return;
    }
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    *dst = s;
}

static int http_eval_parse_int_arg(const char *s, const char *opt) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s[0] || *end || v <= 0 || v > INT_MAX) {
        fprintf(stderr, "ds4-eval-http: invalid value for %s: %s\n", opt, s);
        exit(2);
    }
    return (int)v;
}

static uint64_t http_eval_parse_u64_arg(const char *s, const char *opt) {
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!s[0] || *end || v == 0) {
        fprintf(stderr, "ds4-eval-http: invalid value for %s: %s\n", opt, s);
        exit(2);
    }
    return (uint64_t)v;
}

static float http_eval_parse_float_arg(const char *s, const char *opt,
                                       float minv, float maxv) {
    char *end = NULL;
    float v = strtof(s, &end);
    if (!s[0] || *end || !isfinite(v) || v < minv || v > maxv) {
        fprintf(stderr, "ds4-eval-http: invalid value for %s: %s\n", opt, s);
        exit(2);
    }
    return v;
}

static const char *http_eval_need_arg(int *i, int argc, char **argv,
                                      const char *opt) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "ds4-eval-http: %s requires an argument\n", opt);
        exit(2);
    }
    return argv[++*i];
}

static void http_eval_usage(FILE *fp) {
    fprintf(fp,
        "Usage: ds4-eval-http [options]\n"
        "\n"
        "Runs the embedded ds4-eval question set through an OpenAI-compatible\n"
        "HTTP server. This is a client-side integration harness for exercising\n"
        "server batching with independent requests.\n"
        "\n"
        "HTTP:\n"
        "  --url URL              Chat completions URL. Default: http://127.0.0.1:8000/v1/chat/completions\n"
        "  --model NAME           Request model name. Default: deepseek-v4-flash\n"
        "  --parallel N           Concurrent HTTP requests. Default: 1\n"
        "  --timeout-ms N         Socket timeout per request. Default: 600000\n"
        "  --stream               Use SSE streaming and measure client-visible TTFT.\n"
        "\n"
        "Evaluation:\n"
        "  -n, --tokens N         Max generated tokens per question. Default: 16000\n"
        "  --questions N          Run only the first N embedded questions.\n"
        "  --temp F               Sampling temperature. Default: 0\n"
        "  --top-p F              Nucleus sampling probability. Default: 1\n"
        "  --min-p F              Min-p sampling threshold. Default: 0.05\n"
        "  --seed N               Base sampling seed. Default: time-based\n"
        "  --trace FILE           Write prompts, outputs, and grading decisions.\n"
        "  --think                Enable server thinking mode. Default\n"
        "  --nothink              Disable server thinking mode.\n"
        "  -h, --help             Show this help.\n");
}

static http_eval_config http_eval_parse_options(int argc, char **argv) {
    http_eval_config c = {
        .url = "http://127.0.0.1:8000/v1/chat/completions",
        .model = "deepseek-v4-flash",
        .max_tokens = 16000,
        .parallel = 1,
        .timeout_ms = 600000,
        .top_p = DS4_DEFAULT_TOP_P,
        .min_p = DS4_DEFAULT_MIN_P,
        .think = true,
    };

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            http_eval_usage(stdout);
            exit(0);
        } else if (!strcmp(arg, "--url")) {
            c.url = http_eval_need_arg(&i, argc, argv, arg);
        } else if (!strcmp(arg, "--model")) {
            c.model = http_eval_need_arg(&i, argc, argv, arg);
        } else if (!strcmp(arg, "--parallel")) {
            c.parallel = http_eval_parse_int_arg(http_eval_need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--timeout-ms")) {
            c.timeout_ms = http_eval_parse_int_arg(http_eval_need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--stream")) {
            c.stream = true;
        } else if (!strcmp(arg, "-n") || !strcmp(arg, "--tokens")) {
            c.max_tokens = http_eval_parse_int_arg(http_eval_need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--questions")) {
            c.question_limit = http_eval_parse_int_arg(http_eval_need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--temp")) {
            c.temperature = http_eval_parse_float_arg(http_eval_need_arg(&i, argc, argv, arg),
                                                      arg, 0.0f, 100.0f);
        } else if (!strcmp(arg, "--top-p")) {
            c.top_p = http_eval_parse_float_arg(http_eval_need_arg(&i, argc, argv, arg),
                                                arg, 0.0f, 1.0f);
        } else if (!strcmp(arg, "--min-p")) {
            c.min_p = http_eval_parse_float_arg(http_eval_need_arg(&i, argc, argv, arg),
                                                arg, 0.0f, 1.0f);
        } else if (!strcmp(arg, "--seed")) {
            c.seed = http_eval_parse_u64_arg(http_eval_need_arg(&i, argc, argv, arg), arg);
        } else if (!strcmp(arg, "--trace")) {
            c.trace_path = http_eval_need_arg(&i, argc, argv, arg);
        } else if (!strcmp(arg, "--think")) {
            c.think = true;
        } else if (!strcmp(arg, "--nothink")) {
            c.think = false;
        } else {
            fprintf(stderr, "ds4-eval-http: unknown option: %s\n", arg);
            http_eval_usage(stderr);
            exit(2);
        }
    }
    if (c.max_tokens > EVAL_MAX_CONTEXT) {
        fprintf(stderr,
                "ds4-eval-http: --tokens (%d) exceeds the %d token context cap\n",
                c.max_tokens, EVAL_MAX_CONTEXT);
        exit(2);
    }
    if (c.parallel > 1024) {
        fprintf(stderr, "ds4-eval-http: --parallel must be <= 1024\n");
        exit(2);
    }
    return c;
}

static bool http_eval_parse_url(const char *url, http_eval_url *out,
                                char **err) {
    memset(out, 0, sizeof(*out));
    out->port = 80;

    const char *p = url;
    const char prefix[] = "http://";
    if (strncmp(p, prefix, sizeof(prefix) - 1) != 0) {
        http_eval_set_error(err, "only http:// URLs are supported: %s", url);
        return false;
    }
    p += sizeof(prefix) - 1;

    const char *slash = strchr(p, '/');
    const char *authority_end = slash ? slash : p + strlen(p);
    if (authority_end == p) {
        http_eval_set_error(err, "URL is missing a host: %s", url);
        return false;
    }

    const char *colon = NULL;
    for (const char *q = p; q < authority_end; q++) {
        if (*q == ':') colon = q;
    }
    const char *host_end = colon ? colon : authority_end;
    size_t host_len = (size_t)(host_end - p);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        http_eval_set_error(err, "URL host is invalid or too long: %s", url);
        return false;
    }
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';

    if (colon) {
        char port_buf[16];
        size_t port_len = (size_t)(authority_end - colon - 1);
        if (port_len == 0 || port_len >= sizeof(port_buf)) {
            http_eval_set_error(err, "URL port is invalid: %s", url);
            return false;
        }
        memcpy(port_buf, colon + 1, port_len);
        port_buf[port_len] = '\0';
        out->port = http_eval_parse_int_arg(port_buf, "--url port");
        if (out->port > 65535) {
            http_eval_set_error(err, "URL port is out of range: %d", out->port);
            return false;
        }
    }

    const char *path = slash ? slash : "/";
    if (strlen(path) >= sizeof(out->path)) {
        http_eval_set_error(err, "URL path is too long: %s", url);
        return false;
    }
    strcpy(out->path, path);
    return true;
}

static void http_eval_append_json_string(byte_buf *b, const char *s) {
    buf_append(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"':  buf_append(b, "\\\"", 2); break;
        case '\\': buf_append(b, "\\\\", 2); break;
        case '\b': buf_append(b, "\\b", 2); break;
        case '\f': buf_append(b, "\\f", 2); break;
        case '\n': buf_append(b, "\\n", 2); break;
        case '\r': buf_append(b, "\\r", 2); break;
        case '\t': buf_append(b, "\\t", 2); break;
        default:
            if (*p < 0x20) {
                buf_appendf(b, "\\u%04x", *p);
            } else {
                buf_append(b, (const char *)p, 1);
            }
            break;
        }
    }
    buf_append(b, "\"", 1);
}

static char *http_eval_build_request_body(const http_eval_config *cfg,
                                          const eval_case *tc,
                                          int idx) {
    char *question = build_question_prompt(tc);
    if (!question) return NULL;

    uint64_t seed = cfg->seed ? cfg->seed + (uint64_t)idx : 0;
    byte_buf b = {0};
    buf_append(&b, "{", 1);
    buf_append(&b, "\"model\":", strlen("\"model\":"));
    http_eval_append_json_string(&b, cfg->model);
    buf_append(&b, ",\"messages\":[{\"role\":\"system\",\"content\":",
               strlen(",\"messages\":[{\"role\":\"system\",\"content\":"));
    http_eval_append_json_string(&b, eval_system_prompt());
    buf_append(&b, "},{\"role\":\"user\",\"content\":",
               strlen("},{\"role\":\"user\",\"content\":"));
    http_eval_append_json_string(&b, question);
    buf_appendf(&b,
                "}],\"max_tokens\":%d,\"temperature\":%.8g,"
                "\"top_p\":%.8g,\"min_p\":%.8g,\"stream\":%s,"
                "\"think\":%s",
                cfg->max_tokens,
                cfg->temperature,
                cfg->top_p,
                cfg->min_p,
                cfg->stream ? "true" : "false",
                cfg->think ? "true" : "false");
    if (cfg->stream) {
        buf_append(&b, ",\"stream_options\":{\"include_usage\":true}",
                   strlen(",\"stream_options\":{\"include_usage\":true}"));
    }
    if (seed) {
        buf_appendf(&b, ",\"seed\":%llu", (unsigned long long)seed);
    }
    buf_append(&b, "}", 1);
    free(question);
    return b.v;
}

static bool http_eval_send_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t got = send(fd, p, n, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) return false;
        p += got;
        n -= (size_t)got;
    }
    return true;
}

static void http_eval_set_socket_timeout(int fd, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int http_eval_connect(const http_eval_url *url, int timeout_ms,
                             char **err) {
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", url->port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(url->host, port_buf, &hints, &res);
    if (gai != 0) {
        http_eval_set_error(err, "getaddrinfo(%s:%s): %s",
                            url->host, port_buf, gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    int last_errno = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }
        http_eval_set_socket_timeout(fd, timeout_ms);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        last_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        http_eval_set_error(err, "connect(%s:%d): %s",
                            url->host, url->port, strerror(last_errno));
    }
    return fd;
}

static bool http_eval_post_json(const http_eval_url *url,
                                const http_eval_config *cfg,
                                const char *body,
                                double start_sec,
                                int *status_out,
                                char **body_out,
                                double *ttft_out,
                                char **err) {
    *status_out = 0;
    *body_out = NULL;
    if (ttft_out) *ttft_out = -1.0;

    int fd = http_eval_connect(url, cfg->timeout_ms, err);
    if (fd < 0) return false;

    byte_buf req = {0};
    buf_appendf(&req,
                "POST %s HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Content-Type: application/json\r\n"
                "Accept: %s\r\n"
                "Connection: close\r\n"
                "Content-Length: %zu\r\n"
                "\r\n",
                url->path,
                url->host,
                url->port,
                cfg->stream ? "text/event-stream" : "application/json",
                strlen(body));
    buf_append(&req, body, strlen(body));

    bool ok = http_eval_send_all(fd, req.v, req.len);
    buf_free(&req);
    if (!ok) {
        http_eval_set_error(err, "failed to write HTTP request: %s", strerror(errno));
        close(fd);
        return false;
    }

    byte_buf resp = {0};
    char tmp[8192];
    for (;;) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            http_eval_set_error(err, "failed to read HTTP response: %s", strerror(errno));
            close(fd);
            buf_free(&resp);
            return false;
        }
        if (n == 0) break;
        buf_append(&resp, tmp, (size_t)n);
        if (cfg->stream && ttft_out && *ttft_out < 0.0 && resp.v) {
            char *payload = strstr(resp.v, "\r\n\r\n");
            if (payload &&
                (strstr(payload + 4, "\"content\":") ||
                 strstr(payload + 4, "\"reasoning_content\":"))) {
                *ttft_out = now_sec() - start_sec;
            }
        }
    }
    close(fd);

    if (!resp.v) {
        http_eval_set_error(err, "empty HTTP response");
        return false;
    }

    char *header_end = strstr(resp.v, "\r\n\r\n");
    if (!header_end) {
        http_eval_set_error(err, "malformed HTTP response");
        buf_free(&resp);
        return false;
    }

    int code = 0;
    if (sscanf(resp.v, "HTTP/%*s %d", &code) != 1) {
        http_eval_set_error(err, "malformed HTTP status line");
        buf_free(&resp);
        return false;
    }
    *status_out = code;

    char *payload = header_end + 4;
    size_t payload_len = resp.len - (size_t)(payload - resp.v);
    *body_out = http_eval_strndup(payload, payload_len);
    buf_free(&resp);
    if (!*body_out) {
        http_eval_set_error(err, "out of memory copying HTTP response");
        return false;
    }
    return true;
}

static const char *http_eval_skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int http_eval_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool http_eval_decode_json_string(const char **pp, char **out) {
    const char *p = *pp;
    if (*p != '"') return false;
    p++;

    byte_buf b = {0};
    while (*p && *p != '"') {
        if (*p != '\\') {
            buf_append(&b, p, 1);
            p++;
            continue;
        }
        p++;
        switch (*p) {
        case '"':  buf_append(&b, "\"", 1); p++; break;
        case '\\': buf_append(&b, "\\", 1); p++; break;
        case '/':  buf_append(&b, "/", 1); p++; break;
        case 'b':  buf_append(&b, "\b", 1); p++; break;
        case 'f':  buf_append(&b, "\f", 1); p++; break;
        case 'n':  buf_append(&b, "\n", 1); p++; break;
        case 'r':  buf_append(&b, "\r", 1); p++; break;
        case 't':  buf_append(&b, "\t", 1); p++; break;
        case 'u': {
            int cp = 0;
            bool valid = true;
            for (int i = 0; i < 4; i++) {
                int h = http_eval_hex_digit(p[1 + i]);
                if (h < 0) valid = false;
                cp = (cp << 4) | (h < 0 ? 0 : h);
            }
            if (!valid) {
                buf_free(&b);
                return false;
            }
            if (cp < 0x80) {
                char ch = (char)cp;
                buf_append(&b, &ch, 1);
            } else if (cp < 0x800) {
                char ch[2] = {
                    (char)(0xc0 | (cp >> 6)),
                    (char)(0x80 | (cp & 0x3f)),
                };
                buf_append(&b, ch, sizeof(ch));
            } else {
                char ch[3] = {
                    (char)(0xe0 | (cp >> 12)),
                    (char)(0x80 | ((cp >> 6) & 0x3f)),
                    (char)(0x80 | (cp & 0x3f)),
                };
                buf_append(&b, ch, sizeof(ch));
            }
            p += 5;
            break;
        }
        default:
            buf_free(&b);
            return false;
        }
    }
    if (*p != '"') {
        buf_free(&b);
        return false;
    }
    p++;
    *pp = p;
    if (!b.v) return (*out = http_eval_strdup("")) != NULL;
    *out = b.v;
    return true;
}

static bool http_eval_json_field_string(const char *start, const char *key,
                                        char **out) {
    char pattern[128];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern))
        return false;

    const char *p = start;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *q = http_eval_skip_ws(p + strlen(pattern));
        if (*q++ != ':') {
            p += strlen(pattern);
            continue;
        }
        q = http_eval_skip_ws(q);
        if (!strncmp(q, "null", 4)) {
            *out = http_eval_strdup("");
            return *out != NULL;
        }
        return http_eval_decode_json_string(&q, out);
    }
    return false;
}

static bool http_eval_json_field_int(const char *start, const char *key,
                                     int *out) {
    char pattern[128];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern))
        return false;

    const char *p = start;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *q = http_eval_skip_ws(p + strlen(pattern));
        if (*q++ != ':') {
            p += strlen(pattern);
            continue;
        }
        q = http_eval_skip_ws(q);
        char *end = NULL;
        long v = strtol(q, &end, 10);
        if (end == q || v < 0 || v > INT_MAX) return false;
        *out = (int)v;
        return true;
    }
    return false;
}

static void http_eval_parse_usage(const char *json, http_eval_result *r) {
    const char *usage = strstr(json, "\"usage\"");
    if (!usage) return;

    int prompt = 0;
    int completion = 0;
    int total = 0;
    bool got_prompt = http_eval_json_field_int(usage, "prompt_tokens", &prompt);
    bool got_completion = http_eval_json_field_int(usage, "completion_tokens", &completion);
    bool got_total = http_eval_json_field_int(usage, "total_tokens", &total);
    if (!got_prompt && !got_completion && !got_total) return;

    r->usage_present = true;
    r->prompt_tokens = got_prompt ? prompt : 0;
    r->completion_tokens = got_completion ? completion : 0;
    r->total_tokens = got_total ? total : r->prompt_tokens + r->completion_tokens;
    (void)http_eval_json_field_int(usage, "cached_tokens", &r->cached_tokens);
    (void)http_eval_json_field_int(usage, "cache_write_tokens", &r->cache_write_tokens);
}

static bool http_eval_extract_assistant_content(const char *json, char **out) {
    const char *choices = strstr(json, "\"choices\"");
    const char *message = choices ? strstr(choices, "\"message\"") : NULL;
    const char *start = message ? message : (choices ? choices : json);
    if (http_eval_json_field_string(start, "content", out)) return true;
    if (http_eval_json_field_string(start, "text", out)) return true;
    return false;
}

static void http_eval_extract_optional_reasoning(const char *json, char **out) {
    char *reasoning = NULL;
    if (http_eval_json_field_string(json, "reasoning_content", &reasoning)) {
        *out = reasoning;
    }
}

static void http_eval_parse_sse_body(const char *sse, http_eval_result *r) {
    byte_buf content = {0};
    byte_buf reasoning = {0};
    const char *p = sse;

    while ((p = strstr(p, "data:")) != NULL) {
        p += 5;
        while (*p == ' ' || *p == '\t') p++;
        const char *line_end = strchr(p, '\n');
        if (!line_end) line_end = p + strlen(p);
        const char *data_end = line_end;
        if (data_end > p && data_end[-1] == '\r') data_end--;

        if ((size_t)(data_end - p) == strlen("[DONE]") &&
            !strncmp(p, "[DONE]", strlen("[DONE]"))) {
            p = line_end;
            continue;
        }

        char *event = http_eval_strndup(p, (size_t)(data_end - p));
        if (!event) {
            r->errored = true;
            http_eval_set_error(&r->error, "out of memory parsing SSE event");
            break;
        }

        http_eval_parse_usage(event, r);

        char *piece = NULL;
        if (http_eval_json_field_string(event, "reasoning_content", &piece)) {
            buf_append(&reasoning, piece, strlen(piece));
            free(piece);
        }
        piece = NULL;
        if (http_eval_json_field_string(event, "content", &piece)) {
            buf_append(&content, piece, strlen(piece));
            free(piece);
        }

        free(event);
        p = line_end;
    }

    r->output = content.v ? content.v : http_eval_strdup("");
    r->reasoning = reasoning.v;
    r->generated_bytes = r->output ? (int)strlen(r->output) : 0;
    r->reasoning_bytes = r->reasoning ? (int)strlen(r->reasoning) : 0;
}

static void http_eval_trace_case(FILE *trace,
                                 const http_eval_config *cfg,
                                 const eval_case *tc,
                                 int idx,
                                 int ncases,
                                 const http_eval_result *r,
                                 const char *question_prompt,
                                 const char *response_body) {
    if (!trace) return;
    int nchoices = eval_case_nchoices(tc);
    fprintf(trace,
            "===== HTTP CASE %d/%d %s/%s =====\n"
            "timestamp_unix: %lld\n"
            "source: %s\n"
            "id: %s\n"
            "domain: %s\n"
            "title: %s\n"
            "status: %s\n"
            "picked: %s\n"
            "expected: %s\n"
            "http_status: %d\n"
            "generated_bytes: %d\n"
            "reasoning_bytes: %d\n"
            "elapsed_sec: %.3f\n"
            "ttft_sec: %.3f\n"
            "prompt_tokens: %d\n"
            "completion_tokens: %d\n"
            "total_tokens: %d\n"
            "cached_tokens: %d\n"
            "cache_write_tokens: %d\n"
            "completion_tokens_per_sec: %.3f\n"
            "total_tokens_per_sec: %.3f\n"
            "model: %s\n"
            "max_tokens: %d\n"
            "stream: %s\n"
            "temperature: %.6g\n"
            "top_p: %.6g\n"
            "min_p: %.6g\n"
            "think: %s\n",
            idx + 1,
            ncases,
            tc->source,
            tc->id,
            (long long)time(NULL),
            tc->source,
            tc->id,
            tc->domain,
            tc->title,
            r->errored ? "ERROR" : (r->passed ? "PASS" : "FAIL"),
            r->got[0] ? r->got : "?",
            tc->answer,
            r->http_status,
            r->generated_bytes,
            r->reasoning_bytes,
            r->elapsed_sec,
            r->ttft_sec,
            r->prompt_tokens,
            r->completion_tokens,
            r->total_tokens,
            r->cached_tokens,
            r->cache_write_tokens,
            r->elapsed_sec > 0.0 ? (double)r->completion_tokens / r->elapsed_sec : 0.0,
            r->elapsed_sec > 0.0 ? (double)r->total_tokens / r->elapsed_sec : 0.0,
            cfg->model,
            cfg->max_tokens,
            cfg->stream ? "enabled" : "disabled",
            cfg->temperature,
            cfg->top_p,
            cfg->min_p,
            cfg->think ? "enabled" : "disabled");
    if (r->error && *r->error) fprintf(trace, "error: %s\n", r->error);
    if (nchoices > 0) {
        fprintf(trace, "choices:\n");
        for (int i = 0; i < nchoices; i++) {
            fprintf(trace, "  %c. %s\n", 'A' + i, tc->choice[i]);
        }
    } else if (eval_case_is_compsec(tc)) {
        fprintf(trace, "answer_kind: compsec_line_spec\n");
    } else {
        fprintf(trace, "answer_kind: exact_integer\n");
    }
    trace_write_block(trace, "SYSTEM_PROMPT", eval_system_prompt());
    trace_write_block(trace, "QUESTION_PROMPT", question_prompt);
    trace_write_block(trace, "REASONING_OUTPUT", r->reasoning ? r->reasoning : "");
    trace_write_block(trace, "MODEL_OUTPUT", r->output ? r->output : "");
    if (response_body) trace_write_block(trace, "HTTP_RESPONSE", response_body);
    fputc('\n', trace);
    fflush(trace);
}

static void http_eval_run_one(const http_eval_state *st,
                              int idx,
                              http_eval_result *r,
                              char **response_body_out) {
    memset(r, 0, sizeof(*r));
    r->http_status = 0;
    r->ttft_sec = -1.0;
    snprintf(r->got, sizeof(r->got), "?");
    *response_body_out = NULL;

    const eval_case *tc = &eval_cases[idx];
    char *body = http_eval_build_request_body(st->cfg, tc, idx);
    if (!body) {
        r->errored = true;
        http_eval_set_error(&r->error, "out of memory building request");
        return;
    }

    double start = now_sec();
    char *err = NULL;
    bool ok = http_eval_post_json(st->url, st->cfg, body,
                                  start, &r->http_status, response_body_out,
                                  &r->ttft_sec, &err);
    r->elapsed_sec = now_sec() - start;
    free(body);

    if (!ok) {
        r->errored = true;
        r->error = err ? err : http_eval_strdup("HTTP request failed");
        return;
    }
    if (r->http_status != 200) {
        r->errored = true;
        http_eval_set_error(&r->error, "HTTP status %d", r->http_status);
        if (*response_body_out) {
            r->output = http_eval_strdup(*response_body_out);
            r->generated_bytes = r->output ? (int)strlen(r->output) : 0;
        }
        return;
    }

    if (st->cfg->stream) {
        http_eval_parse_sse_body(*response_body_out, r);
        if (r->errored) return;
    } else {
        http_eval_parse_usage(*response_body_out, r);
        http_eval_extract_optional_reasoning(*response_body_out, &r->reasoning);
        r->reasoning_bytes = r->reasoning ? (int)strlen(r->reasoning) : 0;

        char *content = NULL;
        if (!http_eval_extract_assistant_content(*response_body_out, &content)) {
            r->errored = true;
            http_eval_set_error(&r->error, "could not find assistant content in response");
            r->output = http_eval_strdup(*response_body_out ? *response_body_out : "");
            r->generated_bytes = r->output ? (int)strlen(r->output) : 0;
            return;
        }
        r->output = content;
        r->generated_bytes = (int)strlen(r->output);
    }

    find_case_answer(tc, r->output, r->got, sizeof(r->got));
    r->passed = answer_matches(tc, r->got);
}

static void *http_eval_worker_main(void *arg) {
    http_eval_state *st = arg;

    for (;;) {
        pthread_mutex_lock(&st->mu);
        int idx = st->next_case++;
        pthread_mutex_unlock(&st->mu);
        if (idx >= st->ncases) break;

        http_eval_result r;
        char *response_body = NULL;
        char *question = build_question_prompt(&eval_cases[idx]);
        http_eval_run_one(st, idx, &r, &response_body);

        pthread_mutex_lock(&st->mu);
        if (r.errored) st->errors++;
        else if (r.passed) st->passed++;
        else st->failed++;
        if (r.usage_present) {
            st->usage_cases++;
            st->prompt_tokens += r.prompt_tokens;
            st->completion_tokens += r.completion_tokens;
            st->total_tokens += r.total_tokens;
            st->cached_tokens += r.cached_tokens;
            st->cache_write_tokens += r.cache_write_tokens;
        }
        if (r.ttft_sec >= 0.0) {
            if (st->ttft_cases == 0 || r.ttft_sec < st->ttft_min) st->ttft_min = r.ttft_sec;
            if (st->ttft_cases == 0 || r.ttft_sec > st->ttft_max) st->ttft_max = r.ttft_sec;
            st->ttft_sum += r.ttft_sec;
            st->ttft_cases++;
        }
        const char *status = r.errored ? "ERROR" : (r.passed ? "PASS" : "FAIL");
        printf("[%3d/%3d] %-5s %-14s %-12s got=%s expected=%s http=%d %.2fs",
               idx + 1,
               st->ncases,
               status,
               eval_cases[idx].source,
               eval_cases[idx].id,
               r.got[0] ? r.got : "?",
               eval_cases[idx].answer,
               r.http_status,
               r.elapsed_sec);
        if (r.ttft_sec >= 0.0) printf(" ttft=%.2fs", r.ttft_sec);
        if (r.usage_present) {
            printf(" tok=%d+%d/%d ctps=%.2f",
                   r.prompt_tokens,
                   r.completion_tokens,
                   r.total_tokens,
                   r.elapsed_sec > 0.0 ? (double)r.completion_tokens / r.elapsed_sec : 0.0);
        }
        printf(" bytes=%d", r.generated_bytes);
        if (r.reasoning_bytes > 0) printf(" reasoning_bytes=%d", r.reasoning_bytes);
        fputc('\n', stdout);
        fflush(stdout);
        http_eval_trace_case(st->trace, st->cfg, &eval_cases[idx], idx, st->ncases,
                             &r, question ? question : "", response_body);
        pthread_mutex_unlock(&st->mu);

        free(question);
        free(response_body);
        free(r.output);
        free(r.reasoning);
        free(r.error);
    }
    return NULL;
}

int main(int argc, char **argv) {
    http_eval_config cfg = http_eval_parse_options(argc, argv);
    int embedded = eval_case_count;
    int ncases = embedded;
    if (cfg.question_limit > 0 && cfg.question_limit < ncases) ncases = cfg.question_limit;
    if (cfg.question_limit > embedded) {
        fprintf(stderr, "ds4-eval-http: only %d questions are embedded\n", embedded);
        return 2;
    }
    if (!cfg.seed) {
        cfg.seed = (uint64_t)time(NULL) ^
                   ((uint64_t)getpid() << 32) ^
                   (uint64_t)clock();
    }
    if (cfg.parallel > ncases) cfg.parallel = ncases;

    char *err = NULL;
    http_eval_url url;
    if (!http_eval_parse_url(cfg.url, &url, &err)) {
        fprintf(stderr, "ds4-eval-http: %s\n", err ? err : "invalid URL");
        free(err);
        return 2;
    }

    FILE *trace = NULL;
    if (cfg.trace_path) {
        trace = fopen(cfg.trace_path, "w");
        if (!trace) {
            fprintf(stderr, "ds4-eval-http: cannot open trace '%s': %s\n",
                    cfg.trace_path, strerror(errno));
            return 2;
        }
        fprintf(trace,
                "# ds4-eval-http trace\n"
                "started_unix: %lld\n"
                "url: %s\n"
                "model: %s\n"
                "questions: %d\n"
                "parallel: %d\n"
                "max_tokens: %d\n"
                "temperature: %.6g\n"
                "top_p: %.6g\n"
                "min_p: %.6g\n"
                "seed: %llu\n"
                "think: %s\n"
                "stream: %s\n"
                "\n",
                (long long)time(NULL),
                cfg.url,
                cfg.model,
                ncases,
                cfg.parallel,
                cfg.max_tokens,
                cfg.temperature,
                cfg.top_p,
                cfg.min_p,
                (unsigned long long)cfg.seed,
                cfg.think ? "enabled" : "disabled",
                cfg.stream ? "enabled" : "disabled");
        fflush(trace);
    }

    fprintf(stderr,
            "ds4-eval-http: url=%s model=%s questions=%d parallel=%d tokens=%d think=%s stream=%s\n",
            cfg.url,
            cfg.model,
            ncases,
            cfg.parallel,
            cfg.max_tokens,
            cfg.think ? "enabled" : "disabled",
            cfg.stream ? "enabled" : "disabled");

    http_eval_state st = {
        .cfg = &cfg,
        .url = &url,
        .trace = trace,
        .ncases = ncases,
    };
    pthread_mutex_init(&st.mu, NULL);

    double start = now_sec();
    pthread_t *threads = calloc((size_t)cfg.parallel, sizeof(*threads));
    if (!threads) {
        fprintf(stderr, "ds4-eval-http: out of memory\n");
        pthread_mutex_destroy(&st.mu);
        if (trace) fclose(trace);
        return 1;
    }
    for (int i = 0; i < cfg.parallel; i++) {
        if (pthread_create(&threads[i], NULL, http_eval_worker_main, &st) != 0) {
            fprintf(stderr, "ds4-eval-http: failed to create worker thread\n");
            cfg.parallel = i;
            break;
        }
    }
    for (int i = 0; i < cfg.parallel; i++) {
        pthread_join(threads[i], NULL);
    }
    double elapsed = now_sec() - start;
    free(threads);

    printf("\nSUMMARY passed=%d failed=%d errors=%d total=%d elapsed=%.2fs cases_per_sec=%.3f parallel=%d",
           st.passed,
           st.failed,
           st.errors,
           ncases,
           elapsed,
           elapsed > 0.0 ? (double)ncases / elapsed : 0.0,
           cfg.parallel);
    if (st.usage_cases > 0) {
        printf(" usage_cases=%d prompt_tokens=%lld completion_tokens=%lld total_tokens=%lld cached_tokens=%lld cache_write_tokens=%lld completion_tps=%.3f total_tps=%.3f",
               st.usage_cases,
               st.prompt_tokens,
               st.completion_tokens,
               st.total_tokens,
               st.cached_tokens,
               st.cache_write_tokens,
               elapsed > 0.0 ? (double)st.completion_tokens / elapsed : 0.0,
               elapsed > 0.0 ? (double)st.total_tokens / elapsed : 0.0);
    }
    if (st.ttft_cases > 0) {
        printf(" ttft_avg=%.3fs ttft_min=%.3fs ttft_max=%.3fs",
               st.ttft_sum / (double)st.ttft_cases,
               st.ttft_min,
               st.ttft_max);
    }
    fputc('\n', stdout);
    if (trace) {
        fprintf(trace,
                "===== SUMMARY =====\n"
                "passed: %d\n"
                "failed: %d\n"
                "errors: %d\n"
                "total: %d\n"
                "elapsed_sec: %.3f\n"
                "cases_per_sec: %.6f\n"
                "usage_cases: %d\n"
                "prompt_tokens: %lld\n"
                "completion_tokens: %lld\n"
                "total_tokens: %lld\n"
                "cached_tokens: %lld\n"
                "cache_write_tokens: %lld\n"
                "completion_tokens_per_sec: %.6f\n"
                "total_tokens_per_sec: %.6f\n"
                "ttft_cases: %d\n"
                "ttft_avg_sec: %.6f\n"
                "ttft_min_sec: %.6f\n"
                "ttft_max_sec: %.6f\n",
                st.passed,
                st.failed,
                st.errors,
                ncases,
                elapsed,
                elapsed > 0.0 ? (double)ncases / elapsed : 0.0,
                st.usage_cases,
                st.prompt_tokens,
                st.completion_tokens,
                st.total_tokens,
                st.cached_tokens,
                st.cache_write_tokens,
                elapsed > 0.0 ? (double)st.completion_tokens / elapsed : 0.0,
                elapsed > 0.0 ? (double)st.total_tokens / elapsed : 0.0,
                st.ttft_cases,
                st.ttft_cases > 0 ? st.ttft_sum / (double)st.ttft_cases : 0.0,
                st.ttft_cases > 0 ? st.ttft_min : 0.0,
                st.ttft_cases > 0 ? st.ttft_max : 0.0);
        fclose(trace);
    }
    pthread_mutex_destroy(&st.mu);
    return st.failed || st.errors ? 1 : 0;
}
