/* Android display monitoring, based on wm size and dumpsys input formats.
 * Platform subprocess code is isolated below; never blocks the Deskflow loop.
 * SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "adb_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#define OUTPUT_MAX (256 * 1024)
#define QUERY_TIMEOUT_MS 3000
#define QUERY_INTERVAL_MS 1000

static bool dimensions(const char *p, int *w, int *h)
{
    char *end;
    while (isspace((unsigned char)*p)) ++p;
    if (!isdigit((unsigned char)*p)) return false;
    long x = strtol(p, &end, 10);
    if (*end++ != 'x' || !isdigit((unsigned char)*end)) return false;
    long y = strtol(end, &end, 10);
    if (x < 1 || x > 32767 || y < 1 || y > 32767 ||
        (*end && !isspace((unsigned char)*end))) return false;
    *w = (int)x; *h = (int)y;
    return true;
}

/* Read a numeric field only within one line (external displays cannot supply
 * the primary display's orientation). Reject partial/invalid numeric tokens. */
static int field(const char *line, const char *name)
{
    const char *p = strstr(line, name), *eol = strchr(line, '\n');
    if (!p || (eol && p >= eol)) return -1;
    p += strlen(name);
    while (*p == ' ' || *p == '=' || *p == ':') ++p;
    if (*p < '0' || *p > '9') return -1;
    char *end;
    long value = strtol(p, &end, 10);
    return value >= 0 && value <= 32767 && !isalnum((unsigned char)*end) ? (int)value : -1;
}

bool otg_display_parse(const char *text, struct otg_display_sample *sample)
{
    int w = 0, h = 0, ow = 0, oh = 0, rotation = -1;
    for (const char *line = text; *line;) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!strncmp(p, "Physical size:", 14)) (void)dimensions(p + 14, &w, &h);
        if (!strncmp(p, "Override size:", 14)) (void)dimensions(p + 14, &ow, &oh);
        if (!strncmp(p, "SurfaceOrientation:", 19)) rotation = field(p, "SurfaceOrientation");
        if (!strncmp(p, "Viewport", 8) && field(p, "displayId") == 0) {
            int r = field(p, "orientation");
            if (r >= 0 && r <= 3) rotation = r;
        }
        line = strchr(line, '\n');
        if (!line) break;
        ++line;
    }
    if (ow && oh) { w = ow; h = oh; }
    if (!w || !h || rotation < 0 || rotation > 3) return false;
    *sample = (struct otg_display_sample){w, h, rotation};
    return true;
}

struct otg_display_monitor {
    char *adb, *serial;
    int initial_w, initial_h, base_w, base_h, reference_w, reference_h;
    struct otg_display_sample candidate;
    struct otg_display_size published;
    bool have_candidate, have_published, overflow;
    uint64_t deadline, next_query;
    size_t used;
    char output[OUTPUT_MAX + 1];
#ifdef _WIN32
    HANDLE process, reader;
#else
    pid_t pid;
    int reader;
#endif
};

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

static bool running(struct otg_display_monitor *m)
{
#ifdef _WIN32
    return m->process != NULL;
#else
    return m->pid > 0;
#endif
}

static void cancel_query(struct otg_display_monitor *m)
{
#ifdef _WIN32
    if (m->process) {
        if (WaitForSingleObject(m->process, 0) == WAIT_TIMEOUT) TerminateProcess(m->process, 1);
        CloseHandle(m->process); m->process = NULL;
    }
    if (m->reader) { CloseHandle(m->reader); m->reader = NULL; }
#else
    if (m->pid > 0) {
        (void)kill(m->pid, SIGKILL);
        while (waitpid(m->pid, NULL, 0) < 0 && errno == EINTR) {}
        m->pid = 0;
    }
    if (m->reader >= 0) { close(m->reader); m->reader = -1; }
#endif
}

#ifdef _WIN32
/* Quote UTF-8 argv using the Windows CRT backslash/quote rules. */
static char *quote_arg(char *out, const char *arg)
{
    *out++ = '"';
    while (*arg) {
        size_t slashes = 0;
        while (*arg == '\\') { ++slashes; ++arg; }
        size_t count = (*arg == '"' || !*arg) ? slashes * 2 : slashes;
        while (count--) *out++ = '\\';
        if (*arg == '"') *out++ = '\\';
        if (*arg) *out++ = *arg++;
    }
    *out++ = '"'; *out = 0;
    return out;
}
#endif

static bool start_query(struct otg_display_monitor *m)
{
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE writer = NULL, null_file = INVALID_HANDLE_VALUE;
    STARTUPINFOW si = {0}; PROCESS_INFORMATION pi = {0};
    bool ok = false;
    size_t capacity = 2 * (strlen(m->adb) + strlen(m->serial)) + 128;
    char *command = malloc(capacity);
    wchar_t *wide = NULL;
    if (!command) return false;
    char *p = quote_arg(command, m->adb);
    memcpy(p, " -s ", 4); p = quote_arg(p + 4, m->serial);
    strcpy(p, " shell \"wm size; dumpsys input\"");
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command, -1, NULL, 0);
    if (!count) goto done;
    wide = malloc((size_t)count * sizeof(*wide));
    if (!wide || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command, -1, wide, count)) goto done;
    if (!CreatePipe(&m->reader, &writer, &sa, 0) ||
        !SetHandleInformation(m->reader, HANDLE_FLAG_INHERIT, 0)) goto done;
    null_file = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (null_file == INVALID_HANDLE_VALUE) goto done;
    si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = si.hStdError = null_file; si.hStdOutput = writer;
    if (!CreateProcessW(NULL, wide, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) goto done;
    m->process = pi.hProcess; CloseHandle(pi.hThread); ok = true;
done:
    if (writer) CloseHandle(writer);
    if (null_file != INVALID_HANDLE_VALUE) CloseHandle(null_file);
    free(command); free(wide);
    if (!ok) cancel_query(m);
    return ok;
#else
    int output[2];
    if (pipe(output)) return false;
    if (fcntl(output[0], F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(output[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(output[1], F_SETFD, FD_CLOEXEC) < 0) {
        close(output[0]); close(output[1]); return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        int null_file = open("/dev/null", O_RDWR);
        if (null_file < 0 || dup2(output[1], STDOUT_FILENO) < 0 ||
            dup2(null_file, STDIN_FILENO) < 0 || dup2(null_file, STDERR_FILENO) < 0) _exit(127);
        close(output[0]); close(output[1]);
        if (null_file > STDERR_FILENO) close(null_file);
        execlp(m->adb, m->adb, "-s", m->serial, "shell", "wm size; dumpsys input", (char *)NULL);
        _exit(127);
    }
    close(output[1]);
    if (pid < 0) { close(output[0]); return false; }
    m->pid = pid; m->reader = output[0];
    return true;
#endif
}

/* Drain while the child runs: dumpsys output can exceed a pipe's capacity.
 * The total read per tick is bounded even for a broken/malicious command. */
static void drain_query(struct otg_display_monitor *m)
{
    size_t budget = OUTPUT_MAX;
    char data[4096];
    while (budget) {
        size_t capacity = budget < sizeof(data) ? budget : sizeof(data);
#ifdef _WIN32
        DWORD available = 0, size = 0;
        if (!PeekNamedPipe(m->reader, NULL, 0, NULL, &available, NULL) || !available) break;
        if (!ReadFile(m->reader, data, available < capacity ? available : (DWORD)capacity, &size, NULL) || !size) break;
#else
        ssize_t size = read(m->reader, data, capacity);
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) break;
#endif
        if ((size_t)size > OUTPUT_MAX - m->used) m->overflow = true;
        else { memcpy(m->output + m->used, data, (size_t)size); m->used += (size_t)size; }
        budget -= (size_t)size;
    }
    m->output[m->used] = 0;
}

/* Two consecutive successful snapshots suppress intermediate rotation states.
 * Infer natural virtual axes from the saved aspect, not the first rotation:
 * a phone that rotated before connection must not become the wrong baseline. */
static bool observe(struct otg_display_monitor *m, const struct otg_display_sample *s,
                    struct otg_display_size *out)
{
    if (!m->have_candidate || s->width != m->candidate.width || s->height != m->candidate.height ||
        s->rotation != m->candidate.rotation) {
        m->candidate = *s; m->have_candidate = true;
        return false;
    }
    if (!m->reference_w) {
        m->reference_w = s->width; m->reference_h = s->height;
        bool transpose = (m->initial_w > m->initial_h) != (s->width > s->height);
        m->base_w = transpose ? m->initial_h : m->initial_w;
        m->base_h = transpose ? m->initial_w : m->initial_h;
    }
    int vw = (int)(((int64_t)s->width * m->base_w + m->reference_w / 2) / m->reference_w);
    int vh = (int)(((int64_t)s->height * m->base_h + m->reference_h / 2) / m->reference_h);
    if (vw < 1) vw = 1;
    if (vh < 1) vh = 1;
    if (vw > 32767 || vh > 32767) return false;
    struct otg_display_size size = s->rotation % 2
        ? (struct otg_display_size){s->height, s->width, vh, vw}
        : (struct otg_display_size){s->width, s->height, vw, vh};
    if (m->have_published && size.width == m->published.width && size.height == m->published.height &&
        size.virtual_width == m->published.virtual_width && size.virtual_height == m->published.virtual_height) return false;
    m->published = size; m->have_published = true; *out = size;
    return true;
}

struct otg_display_monitor *otg_display_monitor_create(const char *adb, const char *serial, int w, int h)
{
    if (!adb || !*adb || !serial || !*serial || w < 1 || w > 32767 || h < 1 || h > 32767) return NULL;
    struct otg_display_monitor *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
#ifndef _WIN32
    m->reader = -1;
#endif
    m->adb = malloc(strlen(adb) + 1); m->serial = malloc(strlen(serial) + 1);
    if (!m->adb || !m->serial) { otg_display_monitor_destroy(m); return NULL; }
    strcpy(m->adb, adb); strcpy(m->serial, serial);
    m->initial_w = w; m->initial_h = h;
    return m;
}

bool otg_display_monitor_poll(struct otg_display_monitor *m, struct otg_display_size *out)
{
    if (!m) return false;
    uint64_t now = now_ms();
    if (!running(m)) {
        if (now < m->next_query) return false;
        m->next_query = now + QUERY_INTERVAL_MS;
        m->deadline = now + QUERY_TIMEOUT_MS; m->used = 0; m->overflow = false;
        if (!start_query(m)) m->have_candidate = false;
        return false;
    }
    drain_query(m);
    bool done, ok = false;
#ifdef _WIN32
    done = WaitForSingleObject(m->process, 0) == WAIT_OBJECT_0;
    if (done) { DWORD code; ok = GetExitCodeProcess(m->process, &code) && code == 0; }
#else
    int status = 0;
    pid_t result = waitpid(m->pid, &status, WNOHANG);
    done = result == m->pid || (result < 0 && errno != EINTR);
    if (done) { ok = result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0; m->pid = 0; }
#endif
    if (!done && now < m->deadline && !m->overflow) return false;
    if (done) drain_query(m);
    cancel_query(m);
    /* Failed/slow ADB must not disconnect HID or create a tight retry loop. */
    m->next_query = now + QUERY_INTERVAL_MS;
    struct otg_display_sample sample;
    if (!done || !ok || m->overflow || !otg_display_parse(m->output, &sample)) {
        m->have_candidate = false;
        return false;
    }
    return observe(m, &sample, out);
}

void otg_display_monitor_destroy(struct otg_display_monitor *m)
{
    if (!m) return;
    cancel_query(m); free(m->adb); free(m->serial); free(m);
}
