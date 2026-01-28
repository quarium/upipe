/*
 * Copyright (C) 2012-2014 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short Upipe dictionary dumping for debug purposes
 */

#ifndef _UPIPE_UDICT_DUMP_H_
/** @hidden */
#define _UPIPE_UDICT_DUMP_H_
#ifdef __cplusplus
extern "C" {
#endif

#include "upipe/ubase.h"
#include "upipe/udict.h"
#include "upipe/uprobe.h"

#include <stdint.h>
#include <inttypes.h>

static inline const char *udict_type_str(enum udict_type type)
{
    switch (type) {
        case UDICT_TYPE_OPAQUE:         return "opaque";
        case UDICT_TYPE_STRING:         return "string";
        case UDICT_TYPE_VOID:           return "void";
        case UDICT_TYPE_BOOL:           return "bool";
        case UDICT_TYPE_RATIONAL:       return "rational";
        case UDICT_TYPE_UNSIGNED:       return "unsigned";
        case UDICT_TYPE_SMALL_UNSIGNED: return "small_unsigned";
        case UDICT_TYPE_INT:            return "int";
        case UDICT_TYPE_SMALL_INT:      return "small_int";
        case UDICT_TYPE_FLOAT:          return "float";
        default:                        return "unknown";
    }
}

/** @internal @This dumps the content of a udict entry for debug purposes.
 *
 * @param udict pointer to the udict
 * @param iname name of the entry to dump
 * @param itype type of the entry to dump
 * @param buf buffer to print the udict value to
 * @param size buffer size
 * @return the printed len
 */
static inline int udict_snprint_value(struct udict *udict, const char *iname,
                                      enum udict_type itype, char *buf,
                                      size_t size)
{
    const char *name;
    enum udict_type type;

    if (!ubase_check(udict_name(udict, itype, &name, &type))) {
        name = iname;
        type = itype;
    }

    switch (type) {
        default:
            return snprintf(buf, size, "%s", "");

        case UDICT_TYPE_OPAQUE: {
            struct udict_opaque val;
            int err = udict_get_opaque(udict, &val, itype, iname);
            if (likely(ubase_check(err)))
                return snprintf(buf, size, "%zu octets", val.size);
            return snprintf(buf, size, "[invalid]");
        }

        case UDICT_TYPE_STRING: {
            const char *val = "";
            int err = udict_get_string(udict, &val, itype, iname);
            if (likely(ubase_check(err)))
                return snprintf(buf, size, "\"%s\"", val);
            return snprintf(buf, size, "[invalid]");
        }

        case UDICT_TYPE_VOID:
            return snprintf(buf, size, "%s", "");

        case UDICT_TYPE_BOOL: {
            bool val = false; /* to keep gcc happy */
            int err = udict_get_bool(udict, &val, itype, iname);
            if (likely(ubase_check(err)))
                return snprintf(buf, size, "%s", val ? "true" : "false");
            return snprintf(buf, size, "[invalid]");
        }

        case UDICT_TYPE_RATIONAL: {
            struct urational val = { .num = 0, .den = 0 };
            int err = udict_get_rational(udict, &val, itype, iname);
            if (likely(ubase_check(err)))
                return snprintf(buf, size, "%" PRId64 "/%" PRIu64, val.num, val.den);
            return snprintf(buf, size, "[invalid]");
        }

#define UDICT_DUMP_TEMPLATE(TYPE, utype, ctype, ftype)          \
    case UDICT_TYPE_##TYPE: {                                   \
        ctype val = 0; /* to keep gcc happy */                  \
        int err = udict_get_##utype(udict, &val, itype, iname); \
        if (likely(ubase_check(err)))                           \
            return snprintf(buf, size, ftype, val);             \
        return snprintf(buf, size, "[invalid]");                \
    }

        UDICT_DUMP_TEMPLATE(SMALL_UNSIGNED, small_unsigned, uint8_t, "%" PRIu8)
        UDICT_DUMP_TEMPLATE(SMALL_INT, small_int, int8_t, "%" PRId8)
        UDICT_DUMP_TEMPLATE(UNSIGNED, unsigned, uint64_t, "%" PRIu64)
        UDICT_DUMP_TEMPLATE(INT, int, int64_t, "%" PRId64)
        UDICT_DUMP_TEMPLATE(FLOAT, float, double, "%f")
#undef UDICT_DUMP_TEMPLATE
    }

    return -1;
}

/** @internal @This dumps the content of a udict entry for debug purposes.
 *
 * @param udict pointer to the udict
 * @param iname name of the entry to dump
 * @param itype type of the entry to dump
 * @param buf buffer to print to
 * @param size buffer size
 * @return the printed len
 */
static inline int udict_snprint(struct udict *udict, const char *iname,
                                enum udict_type itype, char *buf, size_t size)
{
    const char *name;
    enum udict_type type;

    if (!ubase_check(udict_name(udict, itype, &name, &type))) {
        name = iname;
        type = itype;
    }

    int ret = udict_snprint_value(udict, iname, itype, NULL, 0);
    if (unlikely(ret < 0))
        return ret;

    char value[ret + 1];
    udict_snprint_value(udict, iname, itype, value, ret + 1);

    return snprintf(buf, size, "\"%s\" [%s]%s%s", name, udict_type_str(type),
                    ret ? ": " : "", value);
}

/** @internal @This dumps the content of a udict entry for debug purposes.
 *
 * @param udict pointer to the udict
 * @param iname name of the entry to dump
 * @param itype type of the entry to dump
 * @param uprobe pipe module printing the messages
 * @param level uprobe log level
 * @param prefix optional prefix
 */
static inline void udict_print(struct udict *udict, const char *iname,
                               enum udict_type itype, struct uprobe *uprobe,
                               enum uprobe_log_level level, const char *prefix)
{
    int len = udict_snprint(udict, iname, itype, NULL, 0);
    if (len > 0) {
        char buf[len + 1];
        udict_snprint(udict, iname, itype, buf, len + 1);
        uprobe_log_va(uprobe, NULL, level, "%s%s", prefix, buf);
    }
}

/** @internal @This dumps the content of a udict entry for debug purposes.
 *
 * @param udict pointer to the udict
 * @param iname name of the entry to dump
 * @param itype type of the entry to dump
 * @param uprobe pipe module printing the messages
 * @param level uprobe log level
 * @param prefix optional prefix
 */
static inline void udict_print_diff(struct udict *u1, struct udict *u2,
                                    const char *iname, enum udict_type itype,
                                    struct uprobe *uprobe,
                                    enum uprobe_log_level level)
{
    size_t s1 = 0, s2 = 0;
    const uint8_t *p1 = NULL, *p2 = NULL;
    ubase_assert(udict_get(u1, iname, itype, &s1, &p1));
    int ret = udict_get(u2, iname, itype, &s2, &p2);
    if (ubase_check(ret) && s1 == s2 && memcmp(p1, p2, s1) == 0)
        return;

    if (p1 && !p2)
        udict_print(u1, iname, itype, uprobe, level, " - ");
    else {
        int l1 = udict_snprint(u1, iname, itype, NULL, 0);
        int l2 = udict_snprint_value(u2, iname, itype, NULL, 0);
        if (l1 >= 0 && l2 >= 0) {
            char buf[l1 + 5 + l2 + 1];
            udict_snprint(u1, iname, itype, buf, l1 + 1);
            sprintf(buf + l1, " → ");
            udict_snprint_value(u2, iname, itype, buf + l1 + 5, l2 + 1);
            uprobe_log_va(uprobe, NULL, level, "   %s", buf);
        }
    }
}

/** @internal @This dumps the content of a udict for debug purposes.
 *
 * @param udict pointer to the udict
 * @param uprobe pipe module printing the messages
 * @param level uprobe log level
 */
static inline void udict_dump_lvl(struct udict *udict, struct uprobe *uprobe,
                                  enum uprobe_log_level level)
{
    const char *iname;
    enum udict_type itype;

    uprobe_log_va(uprobe, NULL, level, "dumping udict %p", udict);
    udict_foreach(udict, iname, itype)
        udict_print(udict, iname, itype, uprobe, level, " - ");
    uprobe_log_va(uprobe, NULL, level, "end of attributes for udict %p", udict);
}

/** @hidden */
#define UDICT_DUMP(Name, Level)                                             \
/** @internal @This dumps the content of a uref for debug purposes.         \
 *                                                                          \
 * @param udict pointer to the udict                                        \
 * @param uprobe pipe module printing the messages                          \
 */                                                                         \
static inline void udict_dump##Name(struct udict *udict,                    \
                                    struct uprobe *uprobe)                  \
{                                                                           \
    return udict_dump_lvl(udict, uprobe, UPROBE_LOG_##Level);               \
}
UDICT_DUMP(, DEBUG);
UDICT_DUMP(_verbose, VERBOSE);
UDICT_DUMP(_dbg, DEBUG);
UDICT_DUMP(_info, INFO);
UDICT_DUMP(_notice, NOTICE);
UDICT_DUMP(_warn, WARNING);
UDICT_DUMP(_err, ERROR);
#undef UDICT_DUMP

/** @internal @This dumps the difference of two udicts for debug purposes.
 *
 * @param udict1 pointer to the first udict to compare
 * @param udict2 pointer to the second udict to compare
 * @param uprobe pipe module printing the messages
 * @param level uprobe log level
 */
static inline void udict_diff_lvl(struct udict *udict1, struct udict *udict2,
                                  struct uprobe *uprobe,
                                  enum uprobe_log_level level)
{
    uprobe_log_va(uprobe, NULL, level, "dumping udict diff %p - %p",
                  udict1, udict2);

    const char *iname;
    enum udict_type itype;
    udict_foreach(udict1, iname, itype) {
        size_t s1 = 0, s2 = 0;
        const uint8_t *p1 = NULL, *p2 = NULL;
        ubase_assert(udict_get(udict1, iname, itype, &s1, &p1));
        int ret = udict_get(udict2, iname, itype, &s2, &p2);
        if (ubase_check(ret) && s1 == s2 && memcmp(p1, p2, s1) == 0)
            continue;

        udict_print_diff(udict1, udict2, iname, itype, uprobe, level);
    }

    udict_foreach(udict2, iname, itype) {
        size_t size;
        const uint8_t *p = NULL;
        int ret = udict_get(udict1, iname, itype, &size, &p);
        if (ubase_check(ret))
            continue;

        udict_print(udict2, iname, itype, uprobe, level, " + ");
    }

    uprobe_log_va(uprobe, NULL, level, "end of diff for udict %p - %p",
                  udict1, udict2);
}

/** @hidden */
#define UDICT_DIFF(Name, Level)                                             \
/** @internal @This dumps the difference of two udicts for debug purposes.  \
 *                                                                          \
 * @param udict1 pointer to the first udict to compare
 * @param udict2 pointer to the second udict to compare
 * @param uprobe pipe module printing the messages                          \
 */                                                                         \
static inline void udict_diff##Name(struct udict *udict1,                   \
                                    struct udict *udict2,                   \
                                    struct uprobe *uprobe)                  \
{                                                                           \
    return udict_diff_lvl(udict1, udict2, uprobe, UPROBE_LOG_##Level);      \
}
UDICT_DIFF(, DEBUG);
UDICT_DIFF(_verbose, VERBOSE);
UDICT_DIFF(_dbg, DEBUG);
UDICT_DIFF(_info, INFO);
UDICT_DIFF(_notice, NOTICE);
UDICT_DIFF(_warn, WARNING);
UDICT_DIFF(_err, ERROR);
#undef UDICT_DIFF

#ifdef __cplusplus
}
#endif
#endif
