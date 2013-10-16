/*
 * Copyright (C) 2012-2013 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/** @file
 * @short Upipe uref attributes handling
 */

#ifndef _UPIPE_UREF_ATTR_H_
/** @hidden */
#define _UPIPE_UREF_ATTR_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <upipe/uref.h>
#include <upipe/udict.h>

/** @This imports all attributes from a uref into another uref (see also
 * @ref udict_import).
 *
 * @param uref overwritten uref
 * @param uref_attr uref containing attributes to fetch
 * @return false in case of error
 */
static inline bool uref_attr_import(struct uref *uref, struct uref *uref_attr)
{
    if (uref_attr->udict == NULL)
        return true;
    if (uref->udict == NULL) {
        uref->udict = udict_dup(uref_attr->udict);
        return uref->udict != NULL;
    }
    return udict_import(uref->udict, uref_attr->udict);
}

#define UREF_ATTR_TEMPLATE(utype, ctype)                                    \
/** @This returns the value of a utype attribute.                           \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @param type type of the attribute (potentially a shorthand)              \
 * @param name name of the attribute                                        \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_attr_get_##utype(struct uref *uref, ctype *p,       \
                                         enum udict_type type,              \
                                         const char *name)                  \
{                                                                           \
    if (uref->udict == NULL)                                                \
        return false;                                                       \
    return udict_get_##utype(uref->udict, p, type, name);                   \
}                                                                           \
/** @This returns the value of a utype attribute, with printf-style name    \
 * generation.                                                              \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @param type type of the attribute (potentially a shorthand)              \
 * @param format printf-style format of the attribute, followed by a        \
 * variable list of arguments                                               \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_attr_get_##utype##_va(struct uref *uref,            \
                                              ctype *p,                     \
                                              enum udict_type type,         \
                                              const char *format, ...)      \
{                                                                           \
    UBASE_VARARG(uref_attr_get_##utype(uref, p, type, string))              \
}                                                                           \
/** @This sets the value of a utype attribute, optionally creating it.      \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @param type type of the attribute (potentially a shorthand)              \
 * @param name name of the attribute                                        \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_attr_set_##utype(struct uref *uref, ctype v,        \
                                         enum udict_type type,              \
                                         const char *name)                  \
{                                                                           \
    if (uref->udict == NULL) {                                              \
        uref->udict = udict_alloc(uref->mgr->udict_mgr, 0);                 \
        if (unlikely(uref->udict == NULL))                                  \
            return false;                                                   \
    }                                                                       \
    return udict_set_##utype(uref->udict, v, type, name);                   \
}                                                                           \
/** @This sets the value of a utype attribute, optionally creating it, with \
 * printf-style name generation.                                            \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @param type type of the attribute (potentially a shorthand)              \
 * @param format printf-style format of the attribute, followed by a        \
 * variable list of arguments                                               \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_attr_set_##utype##_va(struct uref *uref,            \
                                              ctype v,                      \
                                              enum udict_type type,         \
                                              const char *format, ...)      \
{                                                                           \
    UBASE_VARARG(uref_attr_set_##utype(uref, v, type, string))              \
}

UREF_ATTR_TEMPLATE(opaque, struct udict_opaque)
UREF_ATTR_TEMPLATE(string, const char *)
UREF_ATTR_TEMPLATE(void, void *)
UREF_ATTR_TEMPLATE(bool, bool)
UREF_ATTR_TEMPLATE(small_unsigned, uint8_t)
UREF_ATTR_TEMPLATE(small_int, int8_t)
UREF_ATTR_TEMPLATE(unsigned, uint64_t)
UREF_ATTR_TEMPLATE(int, int64_t)
UREF_ATTR_TEMPLATE(float, double)
UREF_ATTR_TEMPLATE(rational, struct urational)
#undef UREF_ATTR_TEMPLATE

/** @This deletes an attribute.
 *
 * @param uref pointer to the uref
 * @param type type of the attribute (potentially a shorthand)
 * @param name name of the attribute
 * @return true if the attribute existed before
 */
static inline bool uref_attr_delete(struct uref *uref, enum udict_type type,
                                    const char *name)
{
    if (uref->udict == NULL)
        return false;
    return udict_delete(uref->udict, type, name);
}

/** @This deletes an attribute, with printf-style name generation.
 *
 * @param uref pointer to the uref
 * @param type type of the attribute (potentially a shorthand)
 * @param format printf-style format of the attribute, followed by a
 * variable list of arguments
 * @return true if the attribute existed before
 */
static inline bool uref_attr_delete_va(struct uref *uref, enum udict_type type,
                                       const char *format, ...)
                   __attribute__ ((format(printf, 3, 4)));
/** @hidden */
static inline bool uref_attr_delete_va(struct uref *uref, enum udict_type type,
                                       const char *format, ...)
{
    UBASE_VARARG(uref_attr_delete(uref, type, string))
}

/*
 * Opaque attributes
 */

/* @This allows to define accessors for a opaque attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param name opaque defining the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_OPAQUE(group, attr, name, desc)                           \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             const uint8_t **p,             \
                                             size_t *size_p)                \
{                                                                           \
    struct udict_opaque opaque;                                             \
    bool ret = uref_attr_get_opaque(uref, &opaque, UDICT_TYPE_OPAQUE, name);\
    if (ret) {                                                              \
        *p = opaque.v;                                                      \
        *size_p = opaque.size;                                              \
    }                                                                       \
    return ret;                                                             \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             const uint8_t *v, size_t size) \
{                                                                           \
    struct udict_opaque opaque;                                             \
    opaque.v = v;                                                           \
    opaque.size = size;                                                     \
    return uref_attr_set_opaque(uref, opaque, UDICT_TYPE_OPAQUE, name);     \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, UDICT_TYPE_OPAQUE, name);                 \
}

/* @This allows to define accessors for a shorthand opaque attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param type shorthand type
 * @param desc description of the attribute
 */
#define UREF_ATTR_OPAQUE_SH(group, attr, type, desc)                        \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             const uint8_t **p,             \
                                             size_t *size_p)                \
{                                                                           \
    struct udict_opaque opaque;                                             \
    bool ret = uref_attr_get_opaque(uref, &opaque, type, NULL);             \
    if (ret) {                                                              \
        *p = opaque.v;                                                      \
        *size_p = opaque.size;                                              \
    }                                                                       \
    return ret;                                                             \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             const uint8_t *v, size_t size) \
{                                                                           \
    struct udict_opaque opaque;                                             \
    opaque.v = v;                                                           \
    opaque.size = size;                                                     \
    return uref_attr_set_opaque(uref, opaque, type, NULL);                  \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, type, NULL);                              \
}

/* @This allows to define accessors for a opaque attribute, with a name
 * depending on printf arguments.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param format printf-style format of the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_OPAQUE_VA(group, attr, format, desc, args_decl, args)     \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             const uint8_t **p,             \
                                             size_t *size_p, args_decl)     \
{                                                                           \
    struct udict_opaque opaque;                                             \
    bool ret = uref_attr_get_opaque_va(uref, &opaque, UDICT_TYPE_OPAQUE,    \
                                       format, args);                       \
    if (ret) {                                                              \
        *p = opaque.v;                                                      \
        *size_p = opaque.size;                                              \
    }                                                                       \
    return ret;                                                             \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             const uint8_t *v,              \
                                             size_t size, args_decl)        \
{                                                                           \
    struct udict_opaque opaque;                                             \
    opaque.v = v;                                                           \
    opaque.size = size;                                                     \
    return uref_attr_set_opaque_va(uref, opaque, UDICT_TYPE_OPAQUE,         \
                                   format, args);                           \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref,          \
                                                args_decl)                  \
{                                                                           \
    return uref_attr_delete_va(uref, UDICT_TYPE_OPAQUE, format, args);      \
}


/*
 * String attributes
 */

/* @This allows to define accessors for a string attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param name string defining the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_STRING(group, attr, name, desc)                           \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             const char **p)                \
{                                                                           \
    return uref_attr_get_string(uref, p, UDICT_TYPE_STRING, name);          \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             const char *v)                 \
{                                                                           \
    return uref_attr_set_string(uref, v, UDICT_TYPE_STRING, name);          \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, UDICT_TYPE_STRING, name);                 \
}                                                                           \
/** @This compares the desc attribute to a given prefix.                    \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param prefix prefix to match                                            \
 * @return true if attribute matches                                        \
 */                                                                         \
static inline bool uref_##group##_match_##attr(struct uref  *uref,          \
                                               const char *prefix)          \
{                                                                           \
    const char *v;                                                          \
    return uref_##group##_get_##attr(uref, &v) && !ubase_ncmp(v, prefix);   \
}

/* @This allows to define accessors for a shorthand string attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param type shorthand type
 * @param desc description of the attribute
 */
#define UREF_ATTR_STRING_SH(group, attr, type, desc)                        \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             const char **p)                \
{                                                                           \
    return uref_attr_get_string(uref, p, type, NULL);                       \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             const char *v)                 \
{                                                                           \
    return uref_attr_set_string(uref, v, type, NULL);                       \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, type, NULL);                              \
}                                                                           \
/** @This compares the desc attribute to a given prefix.                    \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param prefix prefix to match                                            \
 * @return true if attribute matches                                        \
 */                                                                         \
static inline bool uref_##group##_match_##attr(struct uref  *uref,          \
                                               const char *prefix)          \
{                                                                           \
    const char *v;                                                          \
    return uref_##group##_get_##attr(uref, &v) && !ubase_ncmp(v, prefix);   \
}

/* @This allows to define accessors for a string attribute, with a name
 * depending on printf arguments.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param format printf-style format of the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_STRING_VA(group, attr, format, desc, args_decl, args)     \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             const char **p, args_decl)     \
{                                                                           \
    return uref_attr_get_string_va(uref, p, UDICT_TYPE_STRING,              \
                                   format, args);                           \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             const char *v, args_decl)      \
{                                                                           \
    return uref_attr_set_string_va(uref, v, UDICT_TYPE_STRING,              \
                                  format, args);                            \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref,          \
                                                args_decl)                  \
{                                                                           \
    return uref_attr_delete_va(uref, UDICT_TYPE_STRING, format, args);      \
}                                                                           \
/** @This compares the desc attribute to a given prefix.                    \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param prefix prefix to match                                            \
 * @return true if attribute matches                                        \
 */                                                                         \
static inline bool uref_##group##_match_##attr(struct uref  *uref,          \
                                               const char *prefix,          \
                                               args_decl)                   \
{                                                                           \
    const char *v;                                                          \
    return uref_##group##_get_##attr(uref, &v, args) &&                     \
           !ubase_ncmp(v, prefix);                                          \
}


/*
 * Void attributes
 */

/* @This allows to define accessors for a void attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param name string defining the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_VOID(group, attr, name, desc)                             \
/** @This returns the presence of a desc attribute in a uref.               \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref)             \
{                                                                           \
    return uref_attr_get_void(uref, NULL, UDICT_TYPE_VOID, name);           \
}                                                                           \
/** @This sets a desc attribute in a uref.                                  \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref)             \
{                                                                           \
    return uref_attr_set_void(uref, NULL, UDICT_TYPE_VOID, name);           \
}                                                                           \
/** @This deletes a desc attribute from a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, UDICT_TYPE_VOID, name);                   \
}

/* @This allows to define accessors for a shorthand void attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param type shorthand type
 * @param desc description of the attribute
 */
#define UREF_ATTR_VOID_SH(group, attr, type, desc)                          \
/** @This returns the presence of a desc attribute in a uref.               \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref)             \
{                                                                           \
    return uref_attr_get_void(uref, NULL, type, NULL);                      \
}                                                                           \
/** @This sets a desc attribute in a uref.                                  \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref)             \
{                                                                           \
    return uref_attr_set_void(uref, NULL, type, NULL);                      \
}                                                                           \
/** @This deletes a desc attribute from a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, type, NULL);                              \
}

/* @This allows to define accessors for a void attribute, with a name
 * depending on printf arguments.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param format printf-style format of the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_VOID_VA(group, attr, format, desc, args_decl, args)       \
/** @This returns the presence of a desc attribute in a uref.               \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref, args_decl)  \
{                                                                           \
    return uref_attr_get_void_va(uref, NULL, UDICT_TYPE_VOID,               \
                                 format, args);                             \
}                                                                           \
/** @This sets a desc attribute in a uref.                                  \
 *                                                                          \
 * @param uref_p reference to the pointer to the uref (possibly modified)   \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             args_decl)                     \
{                                                                           \
    return uref_attr_set_void_va(uref, NULL, UDICT_TYPE_VOID,               \
                                 format, args);                             \
}                                                                           \
/** @This deletes a desc attribute from a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref,          \
                                                args_decl)                  \
{                                                                           \
    return uref_attr_delete_va(uref, UDICT_TYPE_VOID, format, args);        \
}

/* @This allows to define accessors for a void attribute directly in the uref
 * structure.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param flag name of the flag in @ref uref_flag
 * @param desc description of the attribute
 */
#define UREF_ATTR_VOID_UREF(group, attr, flag, desc)                        \
/** @This returns the presence of a desc attribute in a uref.               \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if the attribute was found                                  \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref)             \
{                                                                           \
    return uref->flags & flag;                                              \
}                                                                           \
/** @This sets a desc attribute in a uref.                                  \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 */                                                                         \
static inline void uref_##group##_set_##attr(struct uref *uref)             \
{                                                                           \
    uref->flags |= flag;                                                    \
}                                                                           \
/** @This deletes a desc attribute from a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 */                                                                         \
static inline void uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    uref->flags &= ~(uint64_t)flag;                                         \
}


/*
 * Small unsigned attributes
 */

/* @This allows to define accessors for a small_unsigned attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param name string defining the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_SMALL_UNSIGNED(group, attr, name, desc)                   \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             uint8_t *p)                    \
{                                                                           \
    return uref_attr_get_small_unsigned(uref, p,                            \
                                        UDICT_TYPE_SMALL_UNSIGNED, name);   \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             uint8_t v)                     \
{                                                                           \
    return uref_attr_set_small_unsigned(uref, v,                            \
                                        UDICT_TYPE_SMALL_UNSIGNED, name);   \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, UDICT_TYPE_SMALL_UNSIGNED, name);         \
}                                                                           \
/** @This compares the desc attribute to given values.                      \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param min minimum value                                                 \
 * @param max maximum value                                                 \
 * @return true if attribute matches                                        \
 */                                                                         \
static inline bool uref_##group##_match_##attr(struct uref  *uref,          \
                                               uint8_t min, uint8_t max)    \
{                                                                           \
    uint8_t v;                                                              \
    return uref_##group##_get_##attr(uref, &v) && (v >= min) && (v <= max); \
}


/* @This allows to define accessors for a shorthand small_unsigned attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param type shorthand type
 * @param desc description of the attribute
 */
#define UREF_ATTR_SMALL_UNSIGNED_SH(group, attr, type, desc)                \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             uint8_t *p)                    \
{                                                                           \
    return uref_attr_get_small_unsigned(uref, p, type, NULL);               \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             uint8_t v)                     \
{                                                                           \
    return uref_attr_set_small_unsigned(uref, v, type, NULL);               \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, type, NULL);                              \
}                                                                           \
/** @This compares the desc attribute to given values.                      \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param min minimum value                                                 \
 * @param max maximum value                                                 \
 * @return true if attribute matches                                        \
 */                                                                         \
static inline bool uref_##group##_match_##attr(struct uref  *uref,          \
                                               uint8_t min, uint8_t max)    \
{                                                                           \
    uint8_t v;                                                              \
    return uref_##group##_get_##attr(uref, &v) && (v >= min) && (v <= max); \
}
 

/* @This allows to define accessors for a small_unsigned attribute, with a name
 * depending on printf arguments.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param format printf-style format of the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_SMALL_UNSIGNED_VA(group, attr, format, desc, args_decl,   \
                                    args)                                   \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             uint8_t *p, args_decl)         \
{                                                                           \
    return uref_attr_get_small_unsigned_va(uref, p,                         \
                                           UDICT_TYPE_SMALL_UNSIGNED,       \
                                           format, args);                   \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             uint8_t v, args_decl)          \
{                                                                           \
    return uref_attr_set_small_unsigned_va(uref, v,                         \
                                           UDICT_TYPE_SMALL_UNSIGNED,       \
                                           format, args);                   \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref,          \
                                                args_decl)                  \
{                                                                           \
    return uref_attr_delete_va(uref, UDICT_TYPE_SMALL_UNSIGNED,             \
                               format, args);                               \
}                                                                           \
/** @This compares the desc attribute to given values.                      \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param min minimum value                                                 \
 * @param max maximum value                                                 \
 * @return true if attribute matches                                        \
 */                                                                         \
static inline bool uref_##group##_match_##attr(struct uref  *uref,          \
                                               uint8_t min, uint8_t max,    \
                                               args_decl)                   \
{                                                                           \
    uint8_t v;                                                              \
    return uref_##group##_get_##attr(uref, &v, args) &&                     \
           (v >= min) && (v <= max);                                        \
}

/*
 * Unsigned attributes
 */

/* @This allows to define accessors for an unsigned attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param name string defining the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_UNSIGNED(group, attr, name, desc)                         \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             uint64_t *p)                   \
{                                                                           \
    return uref_attr_get_unsigned(uref, p, UDICT_TYPE_UNSIGNED, name);      \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             uint64_t v)                    \
{                                                                           \
    return uref_attr_set_unsigned(uref, v, UDICT_TYPE_UNSIGNED, name);      \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, UDICT_TYPE_UNSIGNED, name);               \
}                                                                           \
/** @This compares the desc attribute to given values.                      \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param min minimum value                                                 \
 * @param max maximum value                                                 \
 * @return true if attribute matches                                        \
 */                                                                         \
static inline bool uref_##group##_match_##attr(struct uref  *uref,          \
                                               uint8_t min, uint8_t max)    \
{                                                                           \
    uint64_t v;                                                             \
    return uref_##group##_get_##attr(uref, &v) && (v >= min) && (v <= max); \
}

/* @This allows to define accessors for a shorthand unsigned attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param type shorthand type
 * @param desc description of the attribute
 */
#define UREF_ATTR_UNSIGNED_SH(group, attr, type, desc)                      \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             uint64_t *p)                   \
{                                                                           \
    return uref_attr_get_unsigned(uref, p, type, NULL);                     \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             uint64_t v)                    \
{                                                                           \
    return uref_attr_set_unsigned(uref, v, type, NULL);                     \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, type, NULL);                              \
}                                                                           \
/** @This compares the desc attribute to given values.                      \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param min minimum value                                                 \
 * @param max maximum value                                                 \
 * @return true if attribute matches                                        \
 */                                                                         \
static inline bool uref_##group##_match_##attr(struct uref  *uref,          \
                                               uint8_t min, uint8_t max)    \
{                                                                           \
    uint64_t v;                                                             \
    return uref_##group##_get_##attr(uref, &v) && (v >= min) && (v <= max); \
}


/* @This allows to define accessors for an unsigned attribute, with a name
 * depending on printf arguments.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param format printf-style format of the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_UNSIGNED_VA(group, attr, format, desc, args_decl, args)   \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             uint64_t *p, args_decl)        \
{                                                                           \
    return uref_attr_get_unsigned_va(uref, p, UDICT_TYPE_UNSIGNED,          \
                                     format, args);                         \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             uint64_t v, args_decl)         \
{                                                                           \
    return uref_attr_set_unsigned_va(uref, v, UDICT_TYPE_UNSIGNED,          \
                                     format, args);                         \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref,          \
                                                args_decl)                  \
{                                                                           \
    return uref_attr_delete_va(uref, UDICT_TYPE_UNSIGNED, format, args);    \
}                                                                           \
/** @This compares the desc attribute to given values.                      \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param min minimum value                                                 \
 * @param max maximum value                                                 \
 * @return true if attribute matches                                        \
 */                                                                         \
static inline bool uref_##group##_match_##attr(struct uref  *uref,          \
                                               uint8_t min, uint8_t max,    \
                                               args_decl)                   \
{                                                                           \
    uint64_t v;                                                             \
    return uref_##group##_get_##attr(uref, &v, args) &&                     \
           (v >= min) && (v <= max);                                        \
}

/* @This allows to define accessors for an unsigned attribute directly in the
 * uref structure.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param member name of the member in uref structure
 * @param desc description of the attribute
 */
#define UREF_ATTR_UNSIGNED_UREF(group, attr, member, desc)                  \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             uint64_t *p)                   \
{                                                                           \
    if (uref->member != UINT64_MAX) {                                       \
        *p = uref->member;                                                  \
        return true;                                                        \
    }                                                                       \
    return false;                                                           \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 */                                                                         \
static inline void uref_##group##_set_##attr(struct uref *uref,             \
                                             uint64_t v)                    \
{                                                                           \
    uref->member = v;                                                       \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 */                                                                         \
static inline void uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    uref->member = UINT64_MAX;                                              \
}                                                                           \
/** @This compares the desc attribute to given values.                      \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param min minimum value                                                 \
 * @param max maximum value                                                 \
 * @return true if attribute matches                                        \
 */                                                                         \
static inline bool uref_##group##_match_##attr(struct uref  *uref,          \
                                               uint8_t min, uint8_t max)    \
{                                                                           \
    uint64_t v;                                                             \
    return uref_##group##_get_##attr(uref, &v) && (v >= min) && (v <= max); \
}



/*
 * Int attributes
 */

/* @This allows to define accessors for a int attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param name string defining the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_INT(group, attr, name, desc)                              \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             int64_t *p)                    \
{                                                                           \
    return uref_attr_get_int(uref, p, UDICT_TYPE_INT, name);                \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             int64_t v)                     \
{                                                                           \
    return uref_attr_set_int(uref, v, UDICT_TYPE_INT, name);                \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, UDICT_TYPE_INT, name);                    \
}

/* @This allows to define accessors for a shorthand int attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param type shorthand type
 * @param desc description of the attribute
 */
#define UREF_ATTR_INT_SH(group, attr, type, desc)                           \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             int64_t *p)                    \
{                                                                           \
    return uref_attr_get_int(uref, p, type, NULL);                          \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             int64_t v)                     \
{                                                                           \
    return uref_attr_set_int(uref, v, type, NULL);                          \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, type, NULL);                              \
}

/* @This allows to define accessors for a int attribute, with a name
 * depending on printf arguments.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param format printf-style format of the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_INT_VA(group, attr, format, desc, args_decl, args)        \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             int64_t *p, args_decl)         \
{                                                                           \
    return uref_attr_get_int_va(uref, p, UDICT_TYPE_INT,                    \
                                format, args);                              \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             int64_t v, args_decl)          \
{                                                                           \
    return uref_attr_set_int_va(uref, v, UDICT_TYPE_INT,                    \
                                format, args);                              \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref,          \
                                                args_decl)                  \
{                                                                           \
    return uref_attr_delete_va(uref, UDICT_TYPE_INT, format, args);         \
}


/*
 * Rational attributes
 */

/* @This allows to define accessors for a rational attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param name string defining the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_RATIONAL(group, attr, name, desc)                         \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             struct urational *p)           \
{                                                                           \
    return uref_attr_get_rational(uref, p, UDICT_TYPE_RATIONAL, name);      \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             struct urational v)            \
{                                                                           \
    return uref_attr_set_rational(uref, v, UDICT_TYPE_RATIONAL, name);      \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, UDICT_TYPE_RATIONAL, name);               \
}

/* @This allows to define accessors for a shorthand rational attribute.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param type shorthand type
 * @param desc description of the attribute
 */
#define UREF_ATTR_RATIONAL_SH(group, attr, type, desc)                      \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             struct urational *p)           \
{                                                                           \
    return uref_attr_get_rational(uref, p, type, NULL);                     \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             struct urational v)            \
{                                                                           \
    return uref_attr_set_rational(uref, v, type, NULL);                     \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref)          \
{                                                                           \
    return uref_attr_delete(uref, type, NULL);                              \
}

/* @This allows to define accessors for a rational attribute, with a name
 * depending on prrationalf arguments.
 *
 * @param group group of attributes
 * @param attr readable name of the attribute, for the function names
 * @param format prrationalf-style format of the attribute
 * @param desc description of the attribute
 */
#define UREF_ATTR_RATIONAL_VA(group, attr, format, desc, args_decl, args)   \
/** @This returns the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param p pointer to the retrieved value (modified during execution)      \
 * @return true if the attribute was found, otherwise p is not modified     \
 */                                                                         \
static inline bool uref_##group##_get_##attr(struct uref *uref,             \
                                             struct urational *p, args_decl)\
{                                                                           \
    return uref_attr_get_rational_va(uref, p, UDICT_TYPE_RATIONAL,          \
                                   format, args);                           \
}                                                                           \
/** @This sets the desc attribute of a uref.                                \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @param v value to set                                                    \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_set_##attr(struct uref *uref,             \
                                             struct urational v, args_decl) \
{                                                                           \
    return uref_attr_set_rational_va(uref, v, UDICT_TYPE_RATIONAL,          \
                                   format, args);                           \
}                                                                           \
/** @This deletes the desc attribute of a uref.                             \
 *                                                                          \
 * @param uref pointer to the uref                                          \
 * @return true if no allocation failure occurred                           \
 */                                                                         \
static inline bool uref_##group##_delete_##attr(struct uref *uref,          \
                                                args_decl)                  \
{                                                                           \
    return uref_attr_delete_va(uref, UDICT_TYPE_RATIONAL, format, args);    \
}

UREF_ATTR_UNSIGNED_UREF(attr, priv, priv, private (internal pipe use))

#ifdef __cplusplus
}
#endif
#endif
