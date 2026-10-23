/**
 * PHP Meminfo
 *
 * @author    Benoit Jacquemont <benoit.jacquemont@gmail.com>
 * @author    Vlad Shashkov <v.shashkov@pos-credit.ru>
 * @copyright Copyright (c) 2010-2021, Benoit Jacquemont
 * @copyright Copyright (c) 2026, The Vanta
 */

/*
 * Writer: output buffering, JSON string escaping, integer and pointer formatting.
 *
 * The dump is composed by hand rather than built as a structure and serialised:
 * it has to be written from a process that may already be short of memory, and
 * for millions of items. Buffered output and a single-pass escaper keep it fast
 * and keep a dump valid JSON whatever bytes the application's strings hold.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_meminfo.h"
#include "internal.h"

/* ------------------------------------------------------------------------- *
 * Writer
 *
 * Everything the dump emits goes through these. There is deliberately no
 * printf-style entry point: the old writer spent roughly 60% of its time in a
 * 34-pass escaper and issued about nine stream writes per item.
 * ------------------------------------------------------------------------- */

/**
 * Hand a buffer over to the sink, retrying partial writes.
 */
static void meminfo_write_through(meminfo_writer *w, const char *ptr, size_t len)
{
    if (w->stream) {
        errno = 0;
        if ((size_t) php_stream_write(w->stream, ptr, len) != len) {
            // The stream layer does not promise errno; EIO when it says nothing.
            w->err = errno ? errno : EIO;
        }
        return;
    }

#ifndef PHP_WIN32
    if (w->fd >= 0) {
        size_t offset = 0;

        while (offset < len) {
            ssize_t written = write(w->fd, ptr + offset, len - offset);

            if (written < 0) {
                if (errno == EINTR) {
                    // A signal arrived mid-write; the dump itself may well be
                    // what triggered another one.
                    continue;
                }
                w->err = errno ? errno : EIO;
                return;
            }
            if (written == 0) {
                // No progress and no error: retrying would loop forever.
                w->err = EIO;
                return;
            }

            offset += (size_t) written;
        }
        return;
    }
#endif

    w->err = EBADF;
}

MEMINFO_HIDDEN void meminfo_writer_flush(meminfo_writer *w)
{
    if (w->err || w->len == 0) {
        w->len = 0;
        return;
    }

    meminfo_write_through(w, w->buf, w->len);
    w->len = 0;
}

/**
 * Append len bytes: the part of meminfo_write_raw() that does not fit the
 * inline fast path in internal.h (buffer full, oversized chunk, failed writer).
 */
MEMINFO_HIDDEN zend_never_inline void meminfo_write_raw_slow(meminfo_writer *w, const char *ptr, size_t len)
{
    if (w->err || len == 0) {
        return;
    }

    if (len >= sizeof(w->buf)) {
        // Larger than the buffer: flush what is staged, then pass it straight
        // through rather than copying it in pieces.
        meminfo_writer_flush(w);
        if (!w->err) {
            meminfo_write_through(w, ptr, len);
        }
        return;
    }

    if (w->len + len > sizeof(w->buf)) {
        meminfo_writer_flush(w);
        if (w->err) {
            return;
        }
    }

    memcpy(w->buf + w->len, ptr, len);
    w->len += len;
}

/**
 * Length of the well-formed UTF-8 sequence starting at p, or 0 when the bytes
 * there are not one (stray continuation byte, overlong form, surrogate, value
 * past U+10FFFF, or a sequence cut short by the end of the input).
 *
 * Only called for a lead byte of 0x80 or more; ASCII never gets here.
 */
static size_t meminfo_utf8_sequence_length(const unsigned char *p, const unsigned char *end)
{
    unsigned char c = p[0];
    unsigned char min = 0x80, max = 0xbf;
    size_t len, i;

    if (c >= 0xc2 && c <= 0xdf) {
        len = 2;
    } else if (c >= 0xe0 && c <= 0xef) {
        len = 3;
        if (c == 0xe0) {
            min = 0xa0;
        } else if (c == 0xed) {
            max = 0x9f;
        }
    } else if (c >= 0xf0 && c <= 0xf4) {
        len = 4;
        if (c == 0xf0) {
            min = 0x90;
        } else if (c == 0xf4) {
            max = 0x8f;
        }
    } else {
        return 0;
    }

    if ((size_t) (end - p) < len) {
        return 0;
    }

    // Only the second byte has a narrowed range; the rest are any continuation.
    if (p[1] < min || p[1] > max) {
        return 0;
    }
    for (i = 2; i < len; i++) {
        if (p[i] < 0x80 || p[i] > 0xbf) {
            return 0;
        }
    }

    return len;
}

/* 1 for the bytes the escaper copies as they are: printable ASCII except '"' and '\'. */
static const unsigned char meminfo_json_plain[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,1,0,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
};

/**
 * Append len bytes with JSON string escaping, so that the result is always
 * valid JSON whatever the bytes are.
 *
 * '"', '\' and the C0 controls (NUL included) are escaped as the format
 * requires. Well-formed UTF-8 is passed through untouched; every byte that is
 * not part of a well-formed sequence becomes U+FFFD, as json_encode() does with
 * JSON_INVALID_UTF8_SUBSTITUTE. Without that, one stray byte in one array key
 * made the whole dump unreadable for json_decode() (#124).
 *
 * Runs of unescaped bytes are copied in one go, which is what makes this a
 * single pass over the input with no allocation at all.
 */
MEMINFO_HIDDEN void meminfo_write_json_escaped_len(meminfo_writer *w, const char *s, size_t len)
{
    static const char hex_digits[] = "0123456789abcdef";

    const unsigned char *start = (const unsigned char *) s;
    const unsigned char *end = start + len;
    const unsigned char *run = start;
    const unsigned char *p = start;

    while (p < end) {
        unsigned char c;
        const char *escape;
        size_t escape_len;
        char unicode[6];

        while (p < end && meminfo_json_plain[*p]) {
            p++;
        }
        if (p >= end) {
            break;
        }
        c = *p;

        if (c >= 0x80) {
            size_t sequence = meminfo_utf8_sequence_length(p, end);

            if (sequence > 0) {
                p += sequence;
                continue;
            }

            escape = "\\ufffd";
            escape_len = 6;
        } else if (c == '"') {
            escape = "\\\"";
            escape_len = 2;
        } else if (c == '\\') {
            escape = "\\\\";
            escape_len = 2;
        } else if (c <= 0x1f) {
            unicode[0] = '\\';
            unicode[1] = 'u';
            unicode[2] = '0';
            unicode[3] = '0';
            unicode[4] = hex_digits[(c >> 4) & 0xf];
            unicode[5] = hex_digits[c & 0xf];

            escape = unicode;
            escape_len = 6;
        } else {
            p++;
            continue;
        }

        if (p > run) {
            meminfo_write_raw(w, (const char *) run, (size_t) (p - run));
        }
        meminfo_write_raw(w, escape, escape_len);

        p++;
        run = p;
    }

    if (p > run) {
        meminfo_write_raw(w, (const char *) run, (size_t) (p - run));
    }
}

/**
 * Append a NUL-terminated string with JSON string escaping.
 *
 * For class names, frame labels and type names, which deliberately end at the
 * first NUL: an anonymous class is named "class@anonymous\0<file>:<line>$<n>",
 * and cutting it there is what groups its instances under one class.
 */
MEMINFO_HIDDEN void meminfo_write_json_escaped(meminfo_writer *w, const char *s)
{
    meminfo_write_json_escaped_len(w, s, strlen(s));
}

/**
 * Append the name of an array key or a property, unmangled ("\0Class\0name"
 * becomes "name"), keeping any NUL that is part of the name itself.
 */
MEMINFO_HIDDEN void meminfo_write_json_escaped_key(meminfo_writer *w, zend_string *key, zend_bool is_property)
{
    if (is_property) {
        const char *class_name, *property_name;
        size_t property_len;

        zend_unmangle_property_name_ex(key, &class_name, &property_name, &property_len);
        meminfo_write_json_escaped_len(w, property_name, property_len);
        return;
    }

    meminfo_write_json_escaped_len(w, ZSTR_VAL(key), ZSTR_LEN(key));
}

/**
 * Append a signed decimal integer.
 *
 * Every number the dump emits used to go through a "%ld"/"%zd"/"%d" format,
 * all of which are signed, so values are formatted as signed here too — a
 * negative array key must keep printing as a negative number.
 */
MEMINFO_HIDDEN void meminfo_write_int(meminfo_writer *w, int64_t value)
{
    char digits[21];
    char *q = digits + sizeof(digits);
    uint64_t magnitude;
    int negative = value < 0;

    // Negated as unsigned: -INT64_MIN does not fit in an int64_t.
    magnitude = negative ? (~(uint64_t) value) + 1 : (uint64_t) value;

    // Right to left, then appended in one go.
    do {
        *--q = (char) ('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0);

    if (negative) {
        *--q = '-';
    }

    meminfo_write_raw(w, q, (size_t) (digits + sizeof(digits) - q));
}

/* "00" to "ff": two hex digits per byte of an address. */
static const char meminfo_hex_pairs[513] =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
    "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
    "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f"
    "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
    "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
    "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
    "c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
    "e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

/**
 * Append an address exactly as "%p" renders it: "0x" and lowercase hex with no
 * leading zeros.
 *
 * Written out by hand both for speed and because php.h redirects snprintf to
 * ap_php_snprintf, whose "%p" is not unique on PHP 7.0 — item identifiers must
 * be, since the whole dump is keyed by them.
 */
MEMINFO_HIDDEN void meminfo_write_ptr(meminfo_writer *w, const void *ptr)
{
    static const char hex_digits[] = "0123456789abcdef";

    char out[2 + sizeof(uintptr_t) * 2];
    char *q = out + sizeof(out);
    uintptr_t value = (uintptr_t) ptr;

    // Right to left, so the digit count need not be known up front: two digits
    // per step while a whole byte is left, then the last one if any. No clz:
    // its operand width differs between platforms.
    while (value > 0xf) {
        q -= 2;
        memcpy(q, meminfo_hex_pairs + (value & 0xff) * 2, 2);
        value >>= 8;
    }
    if (value != 0 || q == out + sizeof(out)) {
        *--q = hex_digits[value];
    }

    *--q = 'x';
    *--q = '0';

    meminfo_write_raw(w, q, (size_t) (out + sizeof(out) - q));
}
