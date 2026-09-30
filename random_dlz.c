/*
 * random_dlz.c - self-contained BIND 9 dlopen DLZ module
 *
 * All records are loaded from a text file. Random text can be generated in
 * RDATA with placeholders; for example, "%X#5%.%ZONE%" expands to a different
 * 5-letter DNS-safe label under the configured zone on each lookup.
 *
 * Build on Linux:
 *
 *   cc -O2 -Wall -Wextra -fPIC -pthread -shared \
 *       -o random_dlz.so random_dlz.c
 *
 * Build with musl on Linux:
 *
 *   musl-gcc -O2 -Wall -Wextra -fPIC -pthread -shared \
 *       -o random_dlz.so random_dlz.c
 *
 * Configure named:
 *
 *   dlz "random-dlz" {
 *       database "dlopen /usr/local/lib/bind/random_dlz.so %ZONE% /etc/bind/random/ZONE";
 *       search yes;
 *   };
 *
 * Arguments after the shared object are: zone and records-file. If zone is
 * "%ZONE%", the records-file path is a fixed template file shared by every
 * matched zone. For example, /etc/bind/random/ZONE is read as that literal
 * file path; %ZONE% inside record RDATA expands to the concrete zone selected
 * for the current lookup. Dynamic mode only claims two-label zones by default,
 * so a query under xy1.abc.com can still be matched at abc.com. To support
 * public suffixes, pass a fourth argument such as suffix=com,net,org,co.uk.
 * Add the optional debug argument to log zone discovery, owner normalization,
 * each returned RR, and the final lookup result.
 *
 * Each records-file line contains an owner selector, TTL, type, and textual
 * RDATA:
 *
 *   @       300 SOA  ns1.%ZONE% hostmaster.%ZONE% 1 300 60 86400 0
 *   @       300 NS   ns1.%ZONE%
 *   @       30  A    1.2.4.8
 *   *       30  A    1.2.3.4
 *   *       0   MX   10 %XN#2,5%.%ZONE%
 *   *       60  TXT  "digits=%D#5% letters=%X#2,5% alnum=%NX#0%"
 *   *       60  AAAA 2001:db8::1
 *   @      300 CAA  0 issue "letsencrypt.org"
 *   _smtp._tcp 60 SRV 10 5 25 mail.example.net.
 *
 * The owner can be "@" for the zone apex, "*" for every non-apex name, or
 * an exact relative owner. CNAME is rejected to avoid publishing invalid
 * mixed CNAME/non-CNAME owner data from wildcard templates.
 *
 * Random placeholders are expanded in RDATA only. %D#5% means 5 random digits,
 * %X#5% means 5 random lowercase letters, %XN#5% and %NX#5% mean 5 random
 * lowercase letters/digits, #0 chooses a random length from 1 through 63,
 * #2,5 chooses a random length from 2 through 5, and %ZONE% expands to the
 * current zone name with a trailing dot. Supported placeholder lengths are
 * 1 through 63.
 *
 * DLZ_DLOPEN_VERSION defaults to 3 for current BIND releases. For an older
 * BIND build that exposes DLZ API v2, add -DDLZ_DLOPEN_VERSION=2.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif

#ifndef DLZ_DLOPEN_VERSION
#define DLZ_DLOPEN_VERSION 3
#endif

#define DNS_SDLZFLAG_THREADSAFE 0x00000001U

#define ISC_R_SUCCESS        0U
#define ISC_R_NOMEMORY       1U
#define ISC_R_NOTFOUND      23U
#define ISC_R_FAILURE       25U
#define ISC_R_NOTIMPLEMENTED 27U

#define ISC_LOG_INFO    (-1)
#define ISC_LOG_ERROR   (-4)

#define MAX_RANDOM_PLACEHOLDER_LENGTH 63U
#define RECORD_DATA_BUFFER_SIZE 4096U
#define DEFAULT_DYNAMIC_ZONE_LABELS 2U
#define EXACT_RECORD_BUCKETS 64U

#define UNUSED(value) (void)(value)

typedef unsigned int isc_result_t;
typedef uint32_t dns_ttl_t;
typedef void *dns_sdlzlookup_t;

/*
 * These structures are opaque to this module. named owns them and only passes
 * pointers through the ABI.
 */
typedef struct dns_clientinfomethods dns_clientinfomethods_t;
typedef struct dns_clientinfo dns_clientinfo_t;

typedef void log_t(int level, const char *format, ...);
typedef isc_result_t dns_sdlz_putrr_t(dns_sdlzlookup_t *lookup,
                                      const char *type,
                                      dns_ttl_t ttl,
                                      const char *data);

struct records_stamp {
    dev_t device;
    ino_t inode;
    off_t size;
    time_t modified_seconds;
    long modified_nanoseconds;
};

struct random_dlz_state {
    char *zone;
    char *records_path;
    struct suffix_rule *suffixes;
    struct static_record *exact_records;
    struct static_record *exact_buckets[EXACT_RECORD_BUCKETS];
    struct static_record *wildcard_records;
    struct records_stamp records_stamp;
    struct records_stamp checked_stamp;
    pthread_rwlock_t records_lock;
    log_t *log;
    log_t *debug_log;
    dns_sdlz_putrr_t *putrr;
    size_t zone_length;
    int random_fd;
    bool records_lock_initialized;
    bool dynamic_zone;
};

struct suffix_rule {
    struct suffix_rule *next;
    char *name;
    size_t length;
    size_t labels;
};

struct static_record {
    struct static_record *next;
    struct static_record *hash_next;
    char *owner;
    char *type;
    char *data;
    size_t owner_length;
    uint32_t owner_hash;
    dns_ttl_t ttl;
    bool template_data;
};

static uint32_t
dns_name_hash(const char *text, size_t length) {
    uint32_t hash = 2166136261U;
    size_t index;

    for (index = 0U; index < length; ++index) {
        unsigned char c = (unsigned char)text[index];

        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c + ('a' - 'A'));
        }
        hash ^= (uint32_t)c;
        hash *= 16777619U;
    }
    return hash;
}

static bool
fill_random(int fd, unsigned char *buffer, size_t length) {
#ifdef __linux__
    UNUSED(fd);

    while (length > 0U) {
        ssize_t count = getrandom(buffer, length, 0U);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }

        buffer += (size_t)count;
        length -= (size_t)count;
    }

    return true;
#else
    if (fd < 0) {
        return false;
    }

    while (length > 0U) {
        ssize_t count = read(fd, buffer, length);

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }

        buffer += (size_t)count;
        length -= (size_t)count;
    }

    return true;
#endif
}

enum random_kind {
    RANDOM_ALNUM,
    RANDOM_DIGIT,
    RANDOM_ALPHA,
};

static bool
make_random_text(int random_fd, char *buffer, size_t length,
                 enum random_kind kind) {
    static const char alnum[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static const char digit[] = "0123456789";
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyz";
    const char *alphabet = alnum;
    unsigned char bytes[MAX_RANDOM_PLACEHOLDER_LENGTH];
    size_t alphabet_length = sizeof(alnum) - 1U;
    size_t index;

    if (length == 0U || length > MAX_RANDOM_PLACEHOLDER_LENGTH ||
        !fill_random(random_fd, bytes, length)) {
        return false;
    }

    if (kind == RANDOM_DIGIT) {
        alphabet = digit;
        alphabet_length = sizeof(digit) - 1U;
    } else if (kind == RANDOM_ALPHA) {
        alphabet = alpha;
        alphabet_length = sizeof(alpha) - 1U;
    }

    for (index = 0U; index < length; ++index) {
        buffer[index] = alphabet[bytes[index] % alphabet_length];
    }
    return true;
}

static bool
choose_random_length(int random_fd, size_t minimum, size_t maximum,
                     size_t *length) {
    unsigned char bytes[sizeof(uint64_t)];
    uint64_t value = 0U;
    size_t index;

    if (minimum == 0U || maximum > MAX_RANDOM_PLACEHOLDER_LENGTH ||
        minimum > maximum) {
        return false;
    }

    if (minimum == maximum) {
        *length = minimum;
        return true;
    }

    if (!fill_random(random_fd, bytes, sizeof(bytes))) {
        return false;
    }

    for (index = 0U; index < sizeof(bytes); ++index) {
        value = (value << 8U) | (uint64_t)bytes[index];
    }
    *length = minimum + (size_t)(value % (maximum - minimum + 1U));
    return true;
}

static bool
parse_length_number(const char *input, size_t *index, size_t *value) {
    size_t parsed = 0U;

    if (input[*index] < '0' || input[*index] > '9') {
        return false;
    }

    while (input[*index] >= '0' && input[*index] <= '9') {
        parsed = parsed * 10U + (size_t)(input[*index] - '0');
        if (parsed > MAX_RANDOM_PLACEHOLDER_LENGTH) {
            return false;
        }
        ++*index;
    }

    *value = parsed;
    return true;
}

static bool
parse_random_length(const char *input, size_t *index, int random_fd,
                    size_t *length) {
    size_t minimum;
    size_t maximum;

    if (!parse_length_number(input, index, &minimum)) {
        return false;
    }

    if (input[*index] != ',') {
        maximum = minimum == 0U ? MAX_RANDOM_PLACEHOLDER_LENGTH : minimum;
        minimum = minimum == 0U ? 1U : minimum;
        return choose_random_length(random_fd, minimum, maximum, length);
    }

    ++*index;
    if (!parse_length_number(input, index, &maximum)) {
        return false;
    }
    return choose_random_length(random_fd, minimum, maximum, length);
}

static bool
parse_random_placeholder(const char *input, size_t *index,
                         int random_fd, enum random_kind *kind,
                         size_t *length) {
    switch (input[(*index)++]) {
    case 'D':
        *kind = RANDOM_DIGIT;
        break;
    case 'X':
        if (input[*index] == 'N') {
            *kind = RANDOM_ALNUM;
            ++*index;
        } else {
            *kind = RANDOM_ALPHA;
        }
        break;
    case 'N':
        if (input[*index] != 'X') {
            return false;
        }
        *kind = RANDOM_ALNUM;
        ++*index;
        break;
    default:
        return false;
    }

    if (input[*index] != '#') {
        return false;
    }
    ++*index;

    if (!parse_random_length(input, index, random_fd, length) ||
        input[*index] != '%') {
        return false;
    }
    ++*index;
    return true;
}

static bool
expand_template(const char *input, const char *zone, size_t zone_length,
                int random_fd, char *output, size_t output_size) {
    size_t in_index = 0U;
    size_t out_index = 0U;

    if (output_size == 0U) {
        return false;
    }

    while (input[in_index] != '\0') {
        if (input[in_index] != '%') {
            if (out_index + 1U >= output_size) {
                return false;
            }
            output[out_index++] = input[in_index++];
            continue;
        }

        if (strncmp(input + in_index, "%ZONE%", 6U) == 0) {
            if (out_index >= output_size ||
                zone_length >= output_size - out_index) {
                return false;
            }
            memcpy(output + out_index, zone, zone_length);
            out_index += zone_length;
            in_index += 6U;
            continue;
        }

        enum random_kind random_kind = RANDOM_ALNUM;
        size_t random_length = 0U;

        ++in_index;
        if (!parse_random_placeholder(input, &in_index, random_fd, &random_kind,
                                      &random_length) ||
            out_index + random_length >= output_size ||
            !make_random_text(random_fd, output + out_index, random_length,
                              random_kind)) {
            return false;
        }
        out_index += random_length;
    }

    output[out_index] = '\0';
    return true;
}

static size_t
name_length_without_dot(const char *name) {
    size_t length = strlen(name);

    while (length > 0U && name[length - 1U] == '.') {
        --length;
    }
    return length;
}

static size_t
count_dns_labels(const char *name, size_t length) {
    size_t index;
    size_t labels = 1U;
    bool previous_dot = false;

    if (length == 0U || name[0] == '.') {
        return 0U;
    }

    for (index = 0U; index < length; ++index) {
        if (name[index] != '.') {
            previous_dot = false;
            continue;
        }
        if (previous_dot || index + 1U == length) {
            return 0U;
        }
        previous_dot = true;
        ++labels;
    }
    return labels;
}

static bool
add_suffix_rule(struct random_dlz_state *state, const char *suffix,
                size_t length) {
    struct suffix_rule *rule;
    char *name;
    size_t labels;

    while (length > 0U && suffix[length - 1U] == '.') {
        --length;
    }

    labels = count_dns_labels(suffix, length);
    if (labels == 0U) {
        return false;
    }

    rule = malloc(sizeof(*rule) + length + 1U);
    if (rule == NULL) {
        return false;
    }

    name = (char *)(rule + 1);
    memcpy(name, suffix, length);
    name[length] = '\0';

    rule->name = name;
    rule->length = length;
    rule->labels = labels;
    rule->next = state->suffixes;
    state->suffixes = rule;
    return true;
}

static bool
parse_suffix_rules(struct random_dlz_state *state, const char *text) {
    const char *start = text;
    const char *cursor = text;

    for (;;) {
        if (*cursor == ',' || *cursor == '\0') {
            if (cursor == start ||
                !add_suffix_rule(state, start, (size_t)(cursor - start))) {
                return false;
            }
            if (*cursor == '\0') {
                return true;
            }
            start = cursor + 1;
        }
        ++cursor;
    }
}

static bool
parse_option(struct random_dlz_state *state, const char *arg,
             bool *debug_requested) {
    if (strcmp(arg, "debug") == 0) {
        if (*debug_requested) {
            return false;
        }
        *debug_requested = true;
        return true;
    }

    if (state->dynamic_zone && state->suffixes == NULL &&
        strncmp(arg, "suffix=", 7U) == 0) {
        return parse_suffix_rules(state, arg + 7U);
    }

    return false;
}

static bool
name_matches_suffix_rule(const char *name, size_t name_length,
                         size_t name_labels,
                         const struct suffix_rule *rule) {
    size_t offset;

    if (name_labels != rule->labels + 1U || name_length <= rule->length) {
        return false;
    }

    offset = name_length - rule->length;
    return name[offset - 1U] == '.' &&
           strncasecmp(name + offset, rule->name, rule->length) == 0;
}

static bool
static_zone_allowed(const struct random_dlz_state *state, const char *name) {
    size_t length;

    if (name == NULL || state->zone_length == 0U) {
        return false;
    }

    length = name_length_without_dot(name);
    return length + 1U == state->zone_length &&
           strncasecmp(state->zone, name, length) == 0;
}

static bool
dynamic_zone_allowed(const struct random_dlz_state *state, const char *name,
                     size_t *lengthp) {
    const struct suffix_rule *rule;
    size_t length;
    size_t labels;

    if (name == NULL) {
        return false;
    }

    length = name_length_without_dot(name);
    labels = count_dns_labels(name, length);
    if (labels == 0U) {
        return false;
    }
    if (lengthp != NULL) {
        *lengthp = length;
    }

    if (state->suffixes == NULL) {
        return labels == DEFAULT_DYNAMIC_ZONE_LABELS;
    }

    for (rule = state->suffixes; rule != NULL; rule = rule->next) {
        if (name_matches_suffix_rule(name, length, labels, rule)) {
            return true;
        }
    }
    return false;
}

static char *
absolute_zone_name(const char *zone) {
    size_t length = strlen(zone);
    bool has_dot = length > 0U && zone[length - 1U] == '.';
    char *absolute = malloc(length + (has_dot ? 1U : 2U));

    if (absolute == NULL) {
        return NULL;
    }

    memcpy(absolute, zone, length);
    if (!has_dot) {
        absolute[length++] = '.';
    }
    absolute[length] = '\0';
    return absolute;
}

static bool
parse_ttl(const char *text, dns_ttl_t *ttl) {
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return false;
    }

    *ttl = (dns_ttl_t)value;
    return true;
}

static bool
get_records_stamp(const char *path, struct records_stamp *stamp) {
    struct stat attributes;

    if (stat(path, &attributes) != 0) {
        return false;
    }

    stamp->device = attributes.st_dev;
    stamp->inode = attributes.st_ino;
    stamp->size = attributes.st_size;
#ifdef __linux__
    stamp->modified_seconds = attributes.st_mtim.tv_sec;
    stamp->modified_nanoseconds = attributes.st_mtim.tv_nsec;
#else
    stamp->modified_seconds = attributes.st_mtime;
    stamp->modified_nanoseconds = 0L;
#endif
    return true;
}

static bool
records_stamp_equal(const struct records_stamp *left,
                    const struct records_stamp *right) {
    return left->device == right->device &&
           left->inode == right->inode &&
           left->size == right->size &&
           left->modified_seconds == right->modified_seconds &&
           left->modified_nanoseconds == right->modified_nanoseconds;
}

static void
free_records(struct static_record *record) {
    while (record != NULL) {
        struct static_record *next = record->next;

        free(record);
        record = next;
    }
}

static void
free_suffix_rules(struct suffix_rule *rule) {
    while (rule != NULL) {
        struct suffix_rule *next = rule->next;

        free(rule);
        rule = next;
    }
}

static void
free_state(struct random_dlz_state *state) {
    if (state == NULL) {
        return;
    }

    free(state->zone);
    free(state->records_path);
    free_suffix_rules(state->suffixes);
    if (state->random_fd >= 0) {
        (void)close(state->random_fd);
    }
    free_records(state->exact_records);
    free_records(state->wildcard_records);
    if (state->records_lock_initialized) {
        (void)pthread_rwlock_destroy(&state->records_lock);
    }
    free(state);
}

static char *
next_field(char **cursor) {
    char *field;

    while (**cursor == ' ' || **cursor == '\t') {
        ++*cursor;
    }
    if (**cursor == '\0') {
        return NULL;
    }

    field = *cursor;
    while (**cursor != '\0' && **cursor != ' ' && **cursor != '\t') {
        ++*cursor;
    }
    if (**cursor != '\0') {
        *(*cursor)++ = '\0';
    }
    return field;
}

static char *
remaining_field(char *cursor) {
    char *end;

    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    if (*cursor == '\0') {
        return NULL;
    }

    end = cursor + strlen(cursor);
    while (end > cursor &&
           (end[-1] == '\n' || end[-1] == '\r' ||
            end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
    *end = '\0';
    return *cursor == '\0' ? NULL : cursor;
}

static isc_result_t
make_record(const char *owner, const char *ttl, const char *type,
            const char *data, struct static_record **recordp) {
    struct static_record *record;
    char *strings;
    size_t owner_length;
    size_t type_length;
    size_t data_length;

    if (owner == NULL || ttl == NULL || type == NULL || data == NULL) {
        return ISC_R_FAILURE;
    }
    if (strcasecmp(type, "CNAME") == 0) {
        return ISC_R_FAILURE;
    }

    owner_length = strlen(owner);
    type_length = strlen(type);
    data_length = strlen(data);

    record = malloc(sizeof(*record) + owner_length + type_length +
                    data_length + 3U);
    if (record == NULL) {
        return ISC_R_NOMEMORY;
    }
    if (!parse_ttl(ttl, &record->ttl)) {
        free(record);
        return ISC_R_FAILURE;
    }

    record->next = NULL;
    record->hash_next = NULL;
    strings = (char *)(record + 1);
    record->owner = strings;
    memcpy(record->owner, owner, owner_length + 1U);
    strings += owner_length + 1U;

    record->type = strings;
    memcpy(record->type, type, type_length + 1U);
    strings += type_length + 1U;

    record->data = strings;
    memcpy(record->data, data, data_length + 1U);

    record->owner_length = owner_length;
    record->owner_hash = dns_name_hash(owner, owner_length);
    record->template_data = memchr(data, '%', data_length) != NULL;

    *recordp = record;
    return ISC_R_SUCCESS;
}

static isc_result_t
load_records(log_t *log, const char *path,
             struct static_record *exact_buckets[EXACT_RECORD_BUCKETS],
             struct static_record **exact_recordsp,
             struct static_record **wildcard_recordsp) {
    struct static_record **exact_tail = exact_recordsp;
    struct static_record **wildcard_tail = wildcard_recordsp;
    struct static_record **bucket_tails[EXACT_RECORD_BUCKETS];
    char buffer[4096];
    size_t index;
    unsigned int line_number = 0U;
    FILE *file;
    isc_result_t result = ISC_R_SUCCESS;

    for (index = 0U; index < EXACT_RECORD_BUCKETS; ++index) {
        bucket_tails[index] = &exact_buckets[index];
    }

    file = fopen(path, "r");
    if (file == NULL) {
        if (log != NULL) {
            log(ISC_LOG_ERROR, "random_dlz: cannot open records file %s: %s",
                path, strerror(errno));
        }
        return ISC_R_FAILURE;
    }

    while (fgets(buffer, sizeof(buffer), file) != NULL) {
        char *cursor = buffer;
        char *owner;
        char *ttl;
        char *type;
        char *data;
        struct static_record *record = NULL;

        ++line_number;
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '\0' || *cursor == '\n' || *cursor == '#') {
            continue;
        }

        owner = next_field(&cursor);
        ttl = next_field(&cursor);
        type = next_field(&cursor);
        data = remaining_field(cursor);

        result = make_record(owner, ttl, type, data, &record);
        if (result != ISC_R_SUCCESS) {
            break;
        }

        if (record->owner_length == 1U && record->owner[0] == '*') {
            *wildcard_tail = record;
            wildcard_tail = &record->next;
        } else {
            size_t bucket = record->owner_hash % EXACT_RECORD_BUCKETS;

            *exact_tail = record;
            exact_tail = &record->next;
            *bucket_tails[bucket] = record;
            bucket_tails[bucket] = &record->hash_next;
        }
    }

    if (result != ISC_R_SUCCESS) {
        if (log != NULL) {
            log(ISC_LOG_ERROR, "random_dlz: invalid records file %s line %u",
                path, line_number);
        }
    } else if (ferror(file) != 0) {
        if (log != NULL) {
            log(ISC_LOG_ERROR, "random_dlz: cannot read records file %s: %s",
                path, strerror(errno));
        }
        result = ISC_R_FAILURE;
    }

    if (fclose(file) != 0) {
        result = ISC_R_FAILURE;
    }
    return result;
}

static isc_result_t
load_records_snapshot(log_t *log, const char *path,
                      struct records_stamp *stamp,
                      struct static_record *exact_buckets[EXACT_RECORD_BUCKETS],
                      struct static_record **exact_recordsp,
                      struct static_record **wildcard_recordsp) {
    struct records_stamp before;
    struct records_stamp after;
    isc_result_t result;

    *exact_recordsp = NULL;
    *wildcard_recordsp = NULL;
    memset(exact_buckets, 0,
           sizeof(struct static_record *) * EXACT_RECORD_BUCKETS);

    if (!get_records_stamp(path, &before)) {
        if (log != NULL) {
            log(ISC_LOG_ERROR, "random_dlz: cannot stat records file %s: %s",
                path, strerror(errno));
        }
        return ISC_R_FAILURE;
    }

    result = load_records(log, path, exact_buckets, exact_recordsp,
                          wildcard_recordsp);
    if (result == ISC_R_SUCCESS && !get_records_stamp(path, &after)) {
        if (log != NULL) {
            log(ISC_LOG_ERROR, "random_dlz: cannot stat records file %s: %s",
                path, strerror(errno));
        }
        result = ISC_R_FAILURE;
    }
    if (result == ISC_R_SUCCESS && !records_stamp_equal(&before, &after)) {
        if (log != NULL) {
            log(ISC_LOG_ERROR,
                "random_dlz: records file changed while loading %s", path);
        }
        result = ISC_R_FAILURE;
    }
    if (result != ISC_R_SUCCESS) {
        free_records(*exact_recordsp);
        free_records(*wildcard_recordsp);
        *exact_recordsp = NULL;
        *wildcard_recordsp = NULL;
        memset(exact_buckets, 0,
               sizeof(struct static_record *) * EXACT_RECORD_BUCKETS);
        return result;
    }

    *stamp = after;
    return ISC_R_SUCCESS;
}

static void
lock_records(struct random_dlz_state *state) {
    struct records_stamp observed;
    struct records_stamp loaded_stamp;
    struct static_record *exact_records = NULL;
    struct static_record *exact_buckets[EXACT_RECORD_BUCKETS];
    struct static_record *wildcard_records = NULL;
    struct static_record *old_exact_records = NULL;
    struct static_record *old_wildcard_records = NULL;
    bool installed = false;

    if (!get_records_stamp(state->records_path, &observed)) {
        if (state->log != NULL) {
            state->log(ISC_LOG_ERROR,
                       "random_dlz: cannot stat records file %s: %s",
                       state->records_path, strerror(errno));
        }
        (void)pthread_rwlock_rdlock(&state->records_lock);
        return;
    }

    (void)pthread_rwlock_rdlock(&state->records_lock);
    if (records_stamp_equal(&state->checked_stamp, &observed)) {
        return;
    }
    (void)pthread_rwlock_unlock(&state->records_lock);

    if (load_records_snapshot(state->log, state->records_path, &loaded_stamp,
                              exact_buckets, &exact_records,
                              &wildcard_records) != ISC_R_SUCCESS) {
        (void)pthread_rwlock_wrlock(&state->records_lock);
        if (get_records_stamp(state->records_path, &loaded_stamp) &&
            records_stamp_equal(&observed, &loaded_stamp)) {
            state->checked_stamp = observed;
        }
        (void)pthread_rwlock_unlock(&state->records_lock);
        (void)pthread_rwlock_rdlock(&state->records_lock);
        return;
    }

    (void)pthread_rwlock_wrlock(&state->records_lock);
    if (get_records_stamp(state->records_path, &observed) &&
        records_stamp_equal(&observed, &loaded_stamp)) {
        state->checked_stamp = loaded_stamp;
        if (!records_stamp_equal(&state->records_stamp, &loaded_stamp)) {
            old_exact_records = state->exact_records;
            old_wildcard_records = state->wildcard_records;
            state->exact_records = exact_records;
            memcpy(state->exact_buckets, exact_buckets,
                   sizeof(state->exact_buckets));
            state->wildcard_records = wildcard_records;
            state->records_stamp = loaded_stamp;
            installed = true;
        }
    }
    (void)pthread_rwlock_unlock(&state->records_lock);

    free_records(installed ? old_exact_records : exact_records);
    free_records(installed ? old_wildcard_records : wildcard_records);
    if (installed && state->log != NULL) {
        state->log(ISC_LOG_INFO, "random_dlz: reloaded records from %s",
                   state->records_path);
    }

    (void)pthread_rwlock_rdlock(&state->records_lock);
}

static isc_result_t
put_record(const struct random_dlz_state *state,
           const struct static_record *record, const char *zone,
           size_t zone_length, const char *query_owner, const char *source,
           dns_sdlzlookup_t *lookup) {
    char expanded_data[RECORD_DATA_BUFFER_SIZE];
    const char *data = record->data;
    isc_result_t result;

    if (record->template_data) {
        if (!expand_template(record->data, zone, zone_length, state->random_fd,
                             expanded_data, sizeof(expanded_data))) {
            if (state->log != NULL) {
                state->log(ISC_LOG_ERROR,
                           "random_dlz: failed to expand record data for %s %s",
                           record->owner, record->type);
            }
            return ISC_R_FAILURE;
        }
        data = expanded_data;
    }

    result = state->putrr(lookup, record->type, record->ttl, data);
    if (state->debug_log != NULL) {
        state->debug_log(ISC_LOG_INFO,
                         "random_dlz: rr zone=%s query-owner=%s source=%s "
                         "record-owner=%s type=%s ttl=%u data=%s result=%u",
                         zone, query_owner, source, record->owner,
                         record->type, (unsigned int)record->ttl, data,
                         result);
    }
    return result;
}

static bool
lookup_owner_name(const char *name, const char *zone, size_t zone_length,
                  char *buffer, size_t buffer_size, const char **ownerp) {
    size_t raw_length;
    size_t name_length;
    size_t prefix_length;
    size_t zone_base_length = zone_length -
        (zone_length > 0U && zone[zone_length - 1U] == '.' ? 1U : 0U);

    if (name == NULL || ownerp == NULL) {
        return false;
    }
    if (strcmp(name, "@") == 0) {
        *ownerp = "@";
        return true;
    }

    raw_length = strlen(name);
    name_length = name_length_without_dot(name);
    if (name_length == 0U) {
        return false;
    }

    if (name_length >= zone_base_length &&
        strncasecmp(name + name_length - zone_base_length,
                    zone, zone_base_length) == 0 &&
        (name_length == zone_base_length ||
         name[name_length - zone_base_length - 1U] == '.')) {
        if (name_length == zone_base_length) {
            *ownerp = "@";
            return true;
        }

        prefix_length = name_length - zone_base_length - 1U;
        if (prefix_length + 1U > buffer_size) {
            return false;
        }
        memcpy(buffer, name, prefix_length);
        buffer[prefix_length] = '\0';
        *ownerp = buffer;
        return true;
    }

    if (name_length != raw_length) {
        if (name_length + 1U > buffer_size) {
            return false;
        }
        memcpy(buffer, name, name_length);
        buffer[name_length] = '\0';
        *ownerp = buffer;
        return true;
    }

    *ownerp = name;
    return true;
}

static isc_result_t
put_matching_records(struct random_dlz_state *state, const char *zone,
                     size_t zone_length, const char *name,
                     dns_sdlzlookup_t *lookup) {
    struct static_record *record;
    size_t name_length;
    uint32_t name_hash;
    size_t bucket;
    bool apex_lookup;
    bool found = false;

    if (name == NULL) {
        return ISC_R_NOTFOUND;
    }
    name_length = strlen(name);
    name_hash = dns_name_hash(name, name_length);
    bucket = (size_t)(name_hash % EXACT_RECORD_BUCKETS);
    apex_lookup = name_length == 1U && name[0] == '@';

    for (record = state->exact_buckets[bucket]; record != NULL;
         record = record->hash_next) {
        isc_result_t result;

        if (record->owner_length != name_length ||
            record->owner_hash != name_hash ||
            strncasecmp(record->owner, name, name_length) != 0) {
            continue;
        }

        result = put_record(state, record, zone, zone_length, name, "exact",
                            lookup);
        if (result != ISC_R_SUCCESS) {
            return result;
        }
        found = true;
    }

    if (found || apex_lookup) {
        return found ? ISC_R_SUCCESS : ISC_R_NOTFOUND;
    }

    for (record = state->wildcard_records; record != NULL;
         record = record->next) {
        isc_result_t result = put_record(state, record, zone, zone_length,
                                         name, "wildcard", lookup);

        if (result != ISC_R_SUCCESS) {
            return result;
        }
        found = true;
    }

    return found ? ISC_R_SUCCESS : ISC_R_NOTFOUND;
}

static void
add_helper(struct random_dlz_state *state, const char *name, void *pointer) {
    if (strcmp(name, "log") == 0) {
        state->log = (log_t *)pointer;
    } else if (strcmp(name, "putrr") == 0) {
        state->putrr = (dns_sdlz_putrr_t *)pointer;
    }
}

int
dlz_version(unsigned int *flags) {
    if (flags != NULL) {
        *flags = DNS_SDLZFLAG_THREADSAFE;
    }
    return DLZ_DLOPEN_VERSION;
}

isc_result_t
dlz_create(const char *dlzname, unsigned int argc, char *argv[], void **dbdata,
           ...) {
    struct random_dlz_state *state;
    const char *records_path;
    const char *helper_name;
    va_list arguments;
    unsigned int index;
    bool debug_requested = false;

    UNUSED(dlzname);

    state = calloc(1U, sizeof(*state));
    if (state == NULL) {
        return ISC_R_NOMEMORY;
    }
    state->random_fd = -1;
    if (pthread_rwlock_init(&state->records_lock, NULL) != 0) {
        free_state(state);
        return ISC_R_FAILURE;
    }
    state->records_lock_initialized = true;

    va_start(arguments, dbdata);
    while ((helper_name = va_arg(arguments, const char *)) != NULL) {
        add_helper(state, helper_name, va_arg(arguments, void *));
    }
    va_end(arguments);

    if (argc < 3U || argv[1][0] == '\0' || argv[2][0] == '\0') {
        if (state->log != NULL) {
            state->log(ISC_LOG_ERROR,
                       "random_dlz: expected: <zone> <records-file> "
                       "[suffix=...] [debug]");
        }
        free_state(state);
        return ISC_R_FAILURE;
    }

    records_path = argv[2];
    state->dynamic_zone = strcmp(argv[1], "%ZONE%") == 0;
    state->zone = state->dynamic_zone ? strdup("%ZONE%") :
        absolute_zone_name(argv[1]);
    state->records_path = strdup(records_path);

    for (index = 3U; index < argc; ++index) {
        if (!parse_option(state, argv[index], &debug_requested)) {
            if (state->log != NULL) {
                state->log(ISC_LOG_ERROR,
                           "random_dlz: invalid option: %s", argv[index]);
            }
            free_state(state);
            return ISC_R_FAILURE;
        }
    }
    state->debug_log = debug_requested ? state->log : NULL;

    if (state->zone == NULL || state->records_path == NULL) {
        if (state->log != NULL) {
            state->log(ISC_LOG_ERROR, "random_dlz: invalid arguments");
        }
        free_state(state);
        return ISC_R_FAILURE;
    }
    state->zone_length = strlen(state->zone);

    if (state->putrr == NULL) {
        if (state->log != NULL) {
            state->log(ISC_LOG_ERROR, "random_dlz: missing putrr helper");
        }
        free_state(state);
        return ISC_R_NOTIMPLEMENTED;
    }

#ifndef __linux__
    state->random_fd = open("/dev/urandom", O_RDONLY);
    if (state->random_fd < 0) {
        if (state->log != NULL) {
            state->log(ISC_LOG_ERROR, "random_dlz: cannot open /dev/urandom: %s",
                       strerror(errno));
        }
        free_state(state);
        return ISC_R_FAILURE;
    }
#endif

    if (load_records_snapshot(state->log, state->records_path,
                              &state->records_stamp, state->exact_buckets,
                              &state->exact_records,
                              &state->wildcard_records) != ISC_R_SUCCESS) {
        free_state(state);
        return ISC_R_FAILURE;
    }
    state->checked_stamp = state->records_stamp;

    if (state->log != NULL) {
        state->log(ISC_LOG_INFO, "random_dlz: loaded %s from %s",
                   state->dynamic_zone ? "dynamic zone template" : state->zone,
                   records_path);
    }
    if (state->debug_log != NULL) {
        state->debug_log(ISC_LOG_INFO, "random_dlz: debug logging enabled");
    }

    *dbdata = state;
    return ISC_R_SUCCESS;
}

void
dlz_destroy(void *dbdata) {
    struct random_dlz_state *state = dbdata;

    if (state == NULL) {
        return;
    }

    if (state->log != NULL) {
        state->log(ISC_LOG_INFO, "random_dlz: unloading zone %s",
                   state->zone);
    }

    free_state(state);
}

#if DLZ_DLOPEN_VERSION < 3
isc_result_t
dlz_findzonedb(void *dbdata, const char *name)
#else
isc_result_t
dlz_findzonedb(void *dbdata, const char *name,
               dns_clientinfomethods_t *methods,
               dns_clientinfo_t *clientinfo)
#endif
{
    struct random_dlz_state *state = dbdata;
    isc_result_t result;
    const char *mode;

#if DLZ_DLOPEN_VERSION >= 3
    UNUSED(methods);
    UNUSED(clientinfo);
#endif

    mode = state->dynamic_zone ? "dynamic" : "static";
    result = ISC_R_NOTFOUND;
    if (name != NULL) {
        result = state->dynamic_zone ?
            (dynamic_zone_allowed(state, name, NULL) ?
             ISC_R_SUCCESS : ISC_R_NOTFOUND) :
            (static_zone_allowed(state, name) ?
             ISC_R_SUCCESS : ISC_R_NOTFOUND);
    }
    if (state->debug_log != NULL) {
        state->debug_log(ISC_LOG_INFO,
                         "random_dlz: findzone name=%s mode=%s result=%u",
                         name == NULL ? "(null)" : name, mode, result);
    }
    return result;
}

#if DLZ_DLOPEN_VERSION == 1
isc_result_t
dlz_lookup(const char *zone, const char *name, void *dbdata,
           dns_sdlzlookup_t *lookup)
#else
isc_result_t
dlz_lookup(const char *zone, const char *name, void *dbdata,
           dns_sdlzlookup_t *lookup, dns_clientinfomethods_t *methods,
           dns_clientinfo_t *clientinfo)
#endif
{
    struct random_dlz_state *state = dbdata;
    char zone_buffer[RECORD_DATA_BUFFER_SIZE];
    char owner_buffer[RECORD_DATA_BUFFER_SIZE];
    const char *active_zone = state->zone;
    const char *owner_name;
    log_t *debug_log = state->debug_log;
    isc_result_t result;
    size_t zone_length = state->zone_length;

#if DLZ_DLOPEN_VERSION > 1
    UNUSED(methods);
    UNUSED(clientinfo);
#endif

    if (state->dynamic_zone) {
        size_t zone_base_length;

        if (!dynamic_zone_allowed(state, zone, &zone_base_length)) {
            if (debug_log != NULL) {
                debug_log(ISC_LOG_INFO,
                          "random_dlz: lookup rejected zone=%s name=%s "
                          "reason=zone-boundary",
                          zone == NULL ? "(null)" : zone,
                          name == NULL ? "(null)" : name);
            }
            return ISC_R_NOTFOUND;
        }
        if (zone_base_length + 2U > sizeof(zone_buffer)) {
            return ISC_R_FAILURE;
        }
        memcpy(zone_buffer, zone, zone_base_length);
        zone_length = zone_base_length;
        zone_buffer[zone_length++] = '.';
        zone_buffer[zone_length] = '\0';
        active_zone = zone_buffer;
    }

    if (!lookup_owner_name(name, active_zone, zone_length, owner_buffer,
                           sizeof(owner_buffer), &owner_name)) {
        if (debug_log != NULL) {
            debug_log(ISC_LOG_INFO,
                      "random_dlz: lookup rejected zone=%s name=%s "
                      "reason=owner-normalize",
                      active_zone, name == NULL ? "(null)" : name);
        }
        return ISC_R_NOTFOUND;
    }
    if (strcmp(owner_name, "*") == 0) {
        if (debug_log != NULL) {
            debug_log(ISC_LOG_INFO,
                      "random_dlz: lookup rejected zone=%s name=%s "
                      "owner=%s reason=literal-wildcard",
                      active_zone, name, owner_name);
        }
        return ISC_R_NOTFOUND;
    }
    if (debug_log != NULL) {
        debug_log(ISC_LOG_INFO,
                  "random_dlz: lookup zone=%s name=%s owner=%s",
                  active_zone, name == NULL ? "(null)" : name, owner_name);
    }

    lock_records(state);
    result = put_matching_records(state, active_zone, zone_length, owner_name,
                                  lookup);
    (void)pthread_rwlock_unlock(&state->records_lock);
    if (debug_log != NULL) {
        debug_log(ISC_LOG_INFO,
                  "random_dlz: lookup done zone=%s name=%s owner=%s result=%u",
                  active_zone, name == NULL ? "(null)" : name, owner_name,
                  result);
    }
    return result;
}
