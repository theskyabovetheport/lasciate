/*
 * lasciate -- rewrite what xsecurelock's auth dialog says.
 *
 * An LD_PRELOAD shim, loaded only by the `lasciate` wrapper. It intercepts
 * four functions. Three are display-only:
 *
 *   dcgettext            the password prompt, which comes from libpam
 *   XftTextExtentsUtf8   how auth_x11 measures text before drawing it
 *   XftDrawStringUtf8    how auth_x11 draws it
 *
 * The fourth, pam_authenticate, is observed rather than altered: it is called,
 * its result is noted so the inscription can report that somebody tried, and
 * that result is handed back unchanged. No path here can turn a failed unlock into
 * a successful one, and nothing here sees the password.
 *
 * Constraints the code is shaped by:
 *
 *   - The prompt belongs to libpam, not to xsecurelock. auth_x11 draws
 *     <hostname> " - " <prompt>, with the separator compiled in.
 *
 *   - A Linux-PAM message catalogue is never consulted: authproto_pam calls
 *     setlocale(LC_CTYPE, "") only, LC_MESSAGES stays "C", and glibc's gettext
 *     returns every msgid untranslated under a C locale. Substituting inside
 *     dcgettext sidesteps the locale entirely.
 *
 *   - Both Xft calls must carry the same substitution. auth_x11 measures a
 *     string to centre it, then draws it, and clears only the area it is about
 *     to draw. The substitution is therefore a pure function of its input.
 *
 *   - auth_x11 links no shaping engine, so combining marks cannot stack: every
 *     mark past the first lands at the same offset and overprints. Marks are
 *     applied one per base glyph.
 *
 *   - Feedback symbols hash the time_hex string and nothing else, so they stay
 *     derived from public information and carry no state between keystrokes.
 *     The glyph count is fixed, so it cannot hint at how much has been typed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* libpam's default prompt. Stable across versions; it is the string
 * pam_get_authtok passes to the conversation function. */
#define PAM_DOMAIN "Linux-PAM"
#define PAM_PROMPT "Password: "

#define TEXT_MAX 512
/* Codepoints per glyph set. utf8_decode drops anything past it, silently. */
#define SET_MAX 128
#define GLYPHS_MAX 64

/* Defaults. Every one is overridable from the environment; see README. */
#define DEF_PROCESSING_SRC "Processing..."
#define DEF_PROCESSING "EVOCATIO"
/* temptatum est -- "it has been attempted". Shown in place of the ordinary
 * inscription once somebody has failed. */
/* Split so the \xb7 escape ends where it should: C consumes as many hex digits
 * as it can, and "\xb7EST" would be read as one out-of-range escape. */
#define DEF_PROMPT_FAILED "TEMPTATVM\xc2\xb7" "EST"
#define DEF_BASES "◆●▲▼■◇○▓█"
/* Leading spaces are "no mark", so a proportion of glyphs stay bare and the
 * marked ones read as deliberate rather than as a rendering fault. */
#define DEF_MARKS "   \xcc\x80\xcc\x81\xcc\x82\xcc\x83\xcc\x88\xcc\x8a\xcc\xa7\xcc\xb6"
#define DEF_GLYPHS 12

static char prompt_buf[TEXT_MAX];
static char processing_buf[TEXT_MAX];

/* ------------------------------------------------------------------- utf-8 */

static int utf8_put(char *out, uint32_t cp) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* Characters, not bytes: it is columns on screen that have to line up, and the
 * glyphs in play are all single-width. */
static int utf8_len(const char *s) {
    int n = 0;
    for (; *s != '\0'; s++) {
        if ((*s & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

/* Decodes a UTF-8 string into codepoints. Malformed input is not diagnosed --
 * it comes from the user's own configuration, and the worst case is an odd
 * glyph rather than anything unsafe. */
static int utf8_decode(const char *s, uint32_t *out, int max) {
    int n = 0;
    while (*s != '\0' && n < max) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) {
            out[n++] = c;
            s += 1;
        } else if ((c & 0xE0) == 0xC0 && s[1]) {
            out[n++] = (uint32_t)(c & 0x1F) << 6 | (uint32_t)(s[1] & 0x3F);
            s += 2;
        } else if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
            out[n++] = (uint32_t)(c & 0x0F) << 12 |
                       (uint32_t)(s[1] & 0x3F) << 6 | (uint32_t)(s[2] & 0x3F);
            s += 3;
        } else {
            s += 1;   /* skip anything we do not handle */
        }
    }
    return n;
}

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v != NULL && v[0] != '\0') ? v : fallback;
}

/* ------------------------------------------------------------- tamper trace
 *
 * A failed unlock replaces the inscription for the rest of the lock, so
 * changed text on your return means somebody tried while you were away.
 *
 * Whether it happened, not how often: a file that exists or does not.
 *
 * It has to outlive the process: auth_x11 is spawned per attempt and exits
 * again, so a flag in memory would reset every time. The file lives under
 * XDG_RUNTIME_DIR (tmpfs, mode 700, gone at reboot), and the `lasciate`
 * wrapper removes it as the lock starts, so each session begins clean. */

static const char *fails_path(void) {
    static char path[TEXT_MAX];
    if (path[0] == '\0') {
        const char *dir = getenv("XDG_RUNTIME_DIR");
        if (dir != NULL && dir[0] != '\0') {
            snprintf(path, sizeof(path), "%s/lasciate-fails", dir);
        } else {
            snprintf(path, sizeof(path), "/tmp/lasciate-fails-%u",
                     (unsigned)getuid());
        }
    }
    return path;
}

/* The /tmp fallback is a directory anyone can write to, so what is at the path
 * is checked rather than assumed: no symlink, nothing that is not an ordinary
 * file, and no blocking on the open. A named pipe left there would otherwise
 * stop the first draw of the auth dialog for as long as it sat unopened at the
 * other end, which for a lock screen means waiting at one you cannot answer.
 *
 * Losing the file is not worth handling beyond saying "no failure" -- it costs
 * the tamper trace one lock session and nothing else. */
static int open_fails(int flags, mode_t mode) {
    int fd = open(fails_path(), flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, mode);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Nothing is read out of the file and nothing is written into it: its
 * existence is the whole of the message. */
static int any_failure(void) {
    int fd = open_fails(O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

static void note_failure(void) {
    int fd = open_fails(O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        close(fd);
    }
}

/* The failure signal is pam_authenticate's return value. Nothing on screen
 * carries one: auth_x11 has no failure text of its own beyond a generic
 * "Error", and authproto_pam's protocol with it is single-character message
 * types.
 *
 * A pure observer: call the real function, look at what it returned, hand that
 * value back unaltered. No path here can turn a failure into a success. If the
 * real symbol cannot be resolved it returns non-zero, failing closed. */
int pam_authenticate(void *pamh, int flags) {
    static int (*real)(void *, int);
    if (real == NULL) {
        real = dlsym(RTLD_NEXT, "pam_authenticate");
        if (real == NULL) {
            /* Never fabricate PAM_SUCCESS (0). Any non-zero value denies. */
            return 1;
        }
    }

    int rc = real(pamh, flags);
    if (rc != 0) {   /* PAM_SUCCESS is 0 in every implementation */
        note_failure();
    }
    return rc;
}

/* ------------------------------------------------------------------ prompt */

/* Resolved once. LASCIATE_PROMPT wins if set to anything non-empty; empty or
 * unset falls back to the hostname, so the shim on its own still says something
 * useful. Empty rather than unset is how a user asks for that in practice --
 * a theme supplies a prompt for anything left unset, and only an explicit
 * empty value reaches here.
 *
 * After a failed unlock the text is replaced outright, and that is the whole
 * of the signal: somebody tried. auth_x11 is spawned per attempt, so each new
 * process asks on its first draw and nothing needs to update in place. */
static const char *prompt_text(void) {
    if (prompt_buf[0] == '\0') {
        int failed = any_failure();
        const char *v = getenv(failed ? "LASCIATE_PROMPT_FAILED"
                                      : "LASCIATE_PROMPT");
        if (failed && (v == NULL || v[0] == '\0')) {
            v = DEF_PROMPT_FAILED;
        }

        if (v != NULL && v[0] != '\0') {
            strncpy(prompt_buf, v, sizeof(prompt_buf) - 1);
        } else if (gethostname(prompt_buf, sizeof(prompt_buf) - 1) != 0) {
            strncpy(prompt_buf, PAM_PROMPT, sizeof(prompt_buf) - 1);
        }
        prompt_buf[sizeof(prompt_buf) - 1] = '\0';
    }
    return prompt_buf;
}

char *dcgettext(const char *domainname, const char *msgid, int category) {
    static char *(*real)(const char *, const char *, int);
    if (real == NULL) {
        real = dlsym(RTLD_NEXT, "dcgettext");
        if (real == NULL) {
            /* What gettext itself returns when it has no translation. */
            return (char *)msgid;
        }
    }

    /* msgid is declared nonnull by libintl.h, so it is not checked -- doing so
     * only earns a -Wnonnull-compare warning. domainname may legally be NULL,
     * meaning the current default domain. */
    if (domainname != NULL &&
        strcmp(domainname, PAM_DOMAIN) == 0 &&
        strcmp(msgid, PAM_PROMPT) == 0) {
        return (char *)prompt_text();
    }

    return real(domainname, msgid, category);
}

/* ------------------------------------------------------------------ status */

/* Padded with spaces to the prompt's width, symmetrically. auth_x11 clears
 * only what it is about to draw, so a status narrower than the prompt repaints
 * a strip and leaves the prompt's ends visible around it.
 *
 * A value too long to pad loses its padding; one too long for the buffer at
 * all falls back to the default. */
static const char *status_text(int *out_len) {
    if (processing_buf[0] == '\0') {
        const char *r = env_or("LASCIATE_PROCESSING", DEF_PROCESSING);
        if (strlen(r) >= sizeof(processing_buf)) {
            r = DEF_PROCESSING;
        }

        int want = utf8_len(prompt_text());
        int have = utf8_len(r);
        int pad = (want > have) ? want - have : 0;
        int left = pad / 2;
        int right = pad - left;

        if ((size_t)left + strlen(r) + (size_t)right >= sizeof(processing_buf)) {
            left = right = 0;   /* uncentred beats truncated */
        }

        char *p = processing_buf;
        memset(p, ' ', (size_t)left);
        p += left;
        strncpy(p, r, sizeof(processing_buf) - (size_t)left - 1);
        p += strlen(r);
        memset(p, ' ', (size_t)right);
        p[right] = '\0';
    }

    *out_len = (int)strlen(processing_buf);
    return processing_buf;
}

/* ---------------------------------------------------------------- feedback */

/* Only time_hex output is rewritten. The filter has to be tight: this hook
 * sees every string auth_x11 draws, the substituted prompt included. */
static int is_time_hex(const unsigned char *s, int len) {
    if (len < 3 || s[0] != '0' || s[1] != 'x') {
        return 0;
    }
    for (int i = 2; i < len; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'a' && s[i] <= 'f') ||
              (s[i] >= 'A' && s[i] <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

/* FNV-1a. Not for security -- it only has to be stable, so the measuring call
 * and the drawing call agree on what they are looking at. */
static uint32_t fnv1a(const unsigned char *s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h = (h ^ s[i]) * 16777619u;
    }
    return h;
}

static uint32_t bases[SET_MAX], marks[SET_MAX];
static int n_bases, n_marks, n_glyphs, sets_ready;

static void load_sets(void) {
    if (sets_ready) {
        return;
    }
    n_bases = utf8_decode(env_or("LASCIATE_BASES", DEF_BASES), bases, SET_MAX);
    n_marks = utf8_decode(env_or("LASCIATE_MARKS", DEF_MARKS), marks, SET_MAX);

    const char *g = getenv("LASCIATE_GLYPHS");
    n_glyphs = (g != NULL) ? atoi(g) : DEF_GLYPHS;
    if (n_glyphs < 1 || n_glyphs > GLYPHS_MAX) {
        n_glyphs = DEF_GLYPHS;
    }
    sets_ready = 1;
}

/* Returns the string to hand the real Xft call: a rewritten one in `buf`, or
 * the original untouched. */
static const unsigned char *substitute(const unsigned char *s, int len,
                                       char *buf, size_t bufsz, int *out_len) {
    if (s == NULL || len <= 0) {
        *out_len = len;
        return s;
    }

    const char *src = env_or("LASCIATE_PROCESSING_SRC", DEF_PROCESSING_SRC);
    if (len == (int)strlen(src) && memcmp(s, src, (size_t)len) == 0) {
        return (const unsigned char *)status_text(out_len);
    }

    if (getenv("LASCIATE_NO_GLYPHS") != NULL || !is_time_hex(s, len)) {
        *out_len = len;
        return s;
    }

    load_sets();
    if (n_bases == 0) {
        *out_len = len;
        return s;
    }

    uint32_t h = fnv1a(s, len);
    int n = 0;
    for (int i = 0; i < n_glyphs; i++) {
        /* Re-stir between glyphs so neighbours do not correlate visibly. */
        h = h * 1664525u + 1013904223u;
        if ((size_t)n + 8 >= bufsz) {
            break;
        }
        n += utf8_put(buf + n, bases[(h >> 8) % (uint32_t)n_bases]);
        if (n_marks > 0) {
            uint32_t m = marks[(h >> 20) % (uint32_t)n_marks];
            if (m != ' ') {
                n += utf8_put(buf + n, m);
            }
        }
    }
    buf[n] = '\0';
    *out_len = n;
    return (const unsigned char *)buf;
}

/* Opaque pointers on purpose: none are dereferenced here, they are passed
 * straight through, and declaring them this way means no Xft headers are
 * needed to build. The ABI only requires the pointer and int arguments to line
 * up, which they do. */
void XftDrawStringUtf8(void *draw, const void *color, void *pub,
                       int x, int y, const unsigned char *string, int len) {
    static void (*real)(void *, const void *, void *, int, int,
                        const unsigned char *, int);
    if (real == NULL) {
        real = dlsym(RTLD_NEXT, "XftDrawStringUtf8");
        if (real == NULL) {
            return;
        }
    }

    char buf[GLYPHS_MAX * 8 + 1];
    int out_len;
    const unsigned char *out = substitute(string, len, buf, sizeof(buf), &out_len);
    real(draw, color, pub, x, y, out, out_len);
}

void XftTextExtentsUtf8(void *dpy, void *pub, const unsigned char *string,
                        int len, void *extents) {
    static void (*real)(void *, void *, const unsigned char *, int, void *);
    if (real == NULL) {
        real = dlsym(RTLD_NEXT, "XftTextExtentsUtf8");
        if (real == NULL) {
            return;
        }
    }

    char buf[GLYPHS_MAX * 8 + 1];
    int out_len;
    const unsigned char *out = substitute(string, len, buf, sizeof(buf), &out_len);
    real(dpy, pub, out, out_len, extents);
}

#ifdef LASCIATE_SELFTEST

/* Both the prompt and the padded status resolve once and cache. The tests need
 * to re-resolve them after a failure is noted, which nothing at
 * runtime ever does -- auth_x11 is a fresh process per attempt. */
static void reset_caches(void) {
    prompt_buf[0] = '\0';
    processing_buf[0] = '\0';
}

/* How many codepoints of `s` are base glyphs. Marks have no advance width, so
 * this is the rendered column count. */
static int count_bases(const char *s) {
    uint32_t cps[GLYPHS_MAX * 2];
    int n = utf8_decode(s, cps, GLYPHS_MAX * 2), c = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n_bases; j++) {
            if (cps[i] == bases[j]) {
                c++;
                break;
            }
        }
    }
    return c;
}

/* Covers everything that does not need an X server: which strings are
 * rewritten, and the two invariants the rest depends on -- determinism, and a
 * status as wide as the prompt. */
int main(void) {
    setenv("LASCIATE_PROMPT", "RELINQVITE\xc2\xb7OMNEM\xc2\xb7SPEM", 1);

    struct { const char *in; int rewritten; } cases[] = {
        {"0x58a7f92bd7359", 1},
        {"0xDEADBEEF", 1},
        {"Processing...", 1},
        {"Processing", 0},
        {"RELINQVITE\xc2\xb7OMNEM\xc2\xb7SPEM", 0},
        {"Password: ", 0},
        {"0x", 0},
        {"0xnothex", 0},
        {"", 0},
    };
    int failures = 0;
    char buf[GLYPHS_MAX * 8 + 1];

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int len = (int)strlen(cases[i].in), out_len;
        const unsigned char *out = substitute((const unsigned char *)cases[i].in,
                                              len, buf, sizeof(buf), &out_len);
        int rewritten = (out != (const unsigned char *)cases[i].in);
        if (rewritten != cases[i].rewritten) {
            printf("  FAIL  %-24s expected %s\n", cases[i].in,
                   cases[i].rewritten ? "rewritten" : "untouched");
            failures++;
        } else {
            printf("  ok    %-24s -> %s\n", cases[i].in,
                   rewritten ? (const char *)out : "(untouched)");
        }
    }

    int slen;
    const char *status = status_text(&slen);
    if (utf8_len(status) != utf8_len(prompt_text())) {
        printf("  FAIL  status %d cols, prompt %d\n",
               utf8_len(status), utf8_len(prompt_text()));
        failures++;
    } else {
        printf("  ok    status padded to prompt width (%d cols)\n",
               utf8_len(status));
    }

    char a[sizeof(buf)], b[sizeof(buf)];
    int la, lb;
    const char *t = "0x58a7f92bd7359";
    substitute((const unsigned char *)t, (int)strlen(t), a, sizeof(a), &la);
    substitute((const unsigned char *)t, (int)strlen(t), b, sizeof(b), &lb);
    if (la != lb || memcmp(a, b, (size_t)la) != 0) {
        printf("  FAIL  same input produced different output\n");
        failures++;
    } else {
        printf("  ok    deterministic across calls\n");
    }

    /* The tamper trace: whether somebody tried, which is all it says. */
    setenv("XDG_RUNTIME_DIR", "/tmp", 1);
    unlink(fails_path());
    reset_caches();
    if (any_failure() || strstr(prompt_text(), "TEMPT") != NULL) {
        printf("  FAIL  clean state still shows the failed inscription\n");
        failures++;
    } else {
        printf("  ok    no failures -> ordinary inscription\n");
    }

    note_failure();
    reset_caches();
    if (!any_failure()) {
        printf("  FAIL  a failure did not outlive the process\n");
        failures++;
    } else if (strcmp(prompt_text(), DEF_PROMPT_FAILED) != 0) {
        printf("  FAIL  failed inscription was [%s], want [%s]\n",
               prompt_text(), DEF_PROMPT_FAILED);
        failures++;
    } else {
        printf("  ok    a failure -> [%s]\n", prompt_text());
    }

    /* Repeating it says the same thing, and says it the same way. */
    note_failure();
    note_failure();
    reset_caches();
    if (strcmp(prompt_text(), DEF_PROMPT_FAILED) != 0) {
        printf("  FAIL  three failures read differently: [%s]\n", prompt_text());
        failures++;
    } else {
        printf("  ok    three of them still -> [%s]\n", prompt_text());
    }

    /* The status is padded to whichever inscription is showing, so it still
     * repaints the whole line once the text has changed. */
    int flen;
    const char *fstatus = status_text(&flen);
    if (utf8_len(fstatus) != utf8_len(prompt_text())) {
        printf("  FAIL  status %d cols vs failed inscription %d\n",
               utf8_len(fstatus), utf8_len(prompt_text()));
        failures++;
    } else {
        printf("  ok    status still matches width after the text changes\n");
    }

    unlink(fails_path());
    reset_caches();

    /* Base glyphs, not bytes: marks are two bytes each and how many land is
     * hash-dependent, so byte length varies while rendered width does not.
     * Counting bases is what actually pins the width. */
    const char *shorter = "0xAB";
    substitute((const unsigned char *)shorter, (int)strlen(shorter), b, sizeof(b), &lb);
    load_sets();
    int ba = count_bases(a), bb = count_bases(b);
    if (ba != n_glyphs || bb != n_glyphs) {
        printf("  FAIL  base glyphs %d and %d, want %d\n", ba, bb, n_glyphs);
        failures++;
    } else {
        printf("  ok    %d base glyphs regardless of input (%d vs %d bytes)\n",
               n_glyphs, la, lb);
    }

    /* Configuration is not required to be sensible. Anything that does not fit
     * has to come back inside the buffer rather than through it -- checked at
     * the exact byte where the padding stops helping. */
    static char huge[sizeof(processing_buf) * 2];
    memset(huge, 'A', sizeof(huge) - 1);
    setenv("LASCIATE_PROCESSING", huge, 1);
    reset_caches();
    int hlen;
    const char *hstatus = status_text(&hlen);
    if ((size_t)hlen >= sizeof(processing_buf) || strcmp(hstatus, huge) == 0) {
        printf("  FAIL  oversized status kept %d bytes of %zu\n",
               hlen, sizeof(huge) - 1);
        failures++;
    } else {
        printf("  ok    oversized status falls back (%d bytes, buffer %zu)\n",
               hlen, sizeof(processing_buf));
    }

    /* One byte under the limit still belongs to the user. */
    static char snug[sizeof(processing_buf) - 1];
    memset(snug, 'B', sizeof(snug) - 1);
    setenv("LASCIATE_PROCESSING", snug, 1);
    reset_caches();
    const char *sstatus = status_text(&hlen);
    if (strcmp(sstatus, snug) != 0) {
        printf("  FAIL  status of %zu bytes was not kept intact\n",
               sizeof(snug) - 1);
        failures++;
    } else {
        printf("  ok    status of %zu bytes kept intact\n", sizeof(snug) - 1);
    }
    unsetenv("LASCIATE_PROCESSING");

    return failures != 0;
}
#endif
