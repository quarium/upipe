/*
 * Copyright (C) 2026 EasyTools
 *
 * SPDX-License-Identifier: MIT
 */

/** @file
 * @short Upipe helper functions for
 */

#ifndef _UPIPE_UPIPE_HELPER_OPTIONS_H_
# define _UPIPE_UPIPE_HELPER_OPTIONS_H_
#ifdef __cplusplus
extern "C" {
#endif

/** @hidden */
struct upipe;

/** @internal @This describes an upipe option. */
struct upipe_option {
    /** Option name */
    const char *name;
    /** Option initializer */
    void (*init)(struct upipe *);
    /** Option setter */
    int (*set)(struct upipe *, const char *value);
    /** Option getter */
    int (*get)(struct upipe *, const char **value);
};

/** @internal @This add an entry in an option list. */
#define UPIPE_HELPER_OPTION_DECLARE(Struct, Field, Name) \
    {                                                    \
        .name = Name,                                    \
        .init = Struct##_init_##Field,                   \
        .set = Struct##_set_##Field,                     \
        .get = Struct##_get_##Field,                     \
    },

/** @internal @This generates helper functions for an option. */
#define UPIPE_HELPER_OPTION_DEFINE(Struct, Field, Name)        \
/** @internal @This initializes the option. */                 \
static inline void Struct##_init_##Field(struct upipe *upipe)  \
{                                                              \
    struct Struct *Struct = Struct##_from_upipe(upipe);        \
    Struct->Field = NULL;                                      \
}                                                              \
                                                               \
/** @internal @This sets the option value. */                  \
static inline int Struct##_set_##Field(struct upipe *upipe,    \
                                       const char *value)      \
{                                                              \
    struct Struct *Struct = Struct##_from_upipe(upipe);        \
    return ubase_strdup(&Struct->Field, value);                \
}                                                              \
                                                               \
/** @internal @This gets the option value. */                  \
static inline int Struct##_get_##Field(struct upipe *upipe,    \
                                       const char **value)     \
{                                                              \
    struct Struct *Struct = Struct##_from_upipe(upipe);        \
    if (value)                                                 \
        *value = Struct->Field;                                \
    return UBASE_ERR_NONE;                                     \
}

/** @This generates helper functions to manipulate custom options for a pipe.
 *
 * The options list must be a macro with an argument, typically named Opt.
 * This argument is used to add an option with 3 parameters:
 * @list
 * @item the name of the private structure
 * @item the name of the private structure field that is used to store the value
 * @item the option name
 * @end list
 *
 * For instance, supposing the current private structure with two options, foo
 * and bar:
 * @code
 * struct upipe_foo {
 *  struct upipe upipe;
 *  char *foo;
 *  char *bar;
 * };
 * @end code
 *
 * The option list should have the form:
 * @code
 * #define UPIPE_FOO_OPTIONS(Opt)  \
 *      Opt(upipe_foo, foo, "foo") \
 *      Opt(upipe_foo, bar, "bar")
 * @end code
 *
 * Then you should use UPIPE_HELPER_OPTIONS to generated helper functions:
 *
 * @code
 * UPIPE_HELPER_OPTIONS(upipe_foo, UPIPE_FOO_OPTIONS)
 * @end code
 *
 * This macro generates an initializer, a setter and a getter for each option of
 * the list.
 * For instance:
 * @code
 * upipe_foo_init_foo(struct upipe *upipe);
 * upipe_foo_set_foo(struct upipe *upipe, const char *value);
 * upipe_foo_get_foo(struct upipe *upipe, const char **value);
 *
 * upipe_foo_init_bar(struct upipe *upipe);
 * upipe_foo_set_bar(struct upipe *upipe, const char *value);
 * upipe_foo_get_bar(struct upipe *upipe, const char **value);
 * @end code
 *
 * This macro also generates helpers functions:
 * @list
 * @item @code
 *  void upipe_foo_init_options(struct upipe *upipe)
 * @end code
 * Typically called in your upipe_foo_alloc() function, this initializes all
 * options to NULL.
 *
 * @item @code
 *  void upipe_foo_clean_options(struct upipe *upipe)
 * @end code
 * Typically called in your upipe_foo_free() function, this frees all options.
 *
 * @item @code
 *  int upipe_foo_handle_set_options(struct upipe *upipe, const char *option,
 *                                   const char *value)
 * @end code
 * Typically called in your upipe_foo_control() function. This looks for an
 * option named option in the option list and sets it to value if an option is
 * found or returns UPIPE_ERR_UNHANDLED otherwise.
 *
 * @item @code
 *  int upipe_foo_handle_get_options(struct upipe *upipe, const char *option,
 *                                   const char **value)
 * @end code
 * Typically called in your upipe_foo_control() function. This looks for an
 * option named option in the option list and gets the current value or returns
 * UPIPE_ERR_UNHANDLED otherwise.
 * @end list
 *
 * @param STRUCTURE name of your private upipe structure
 * @param OPTIONS list of custom options to handle
 */
#define UPIPE_HELPER_OPTIONS(STRUCTURE, OPTIONS)                            \
OPTIONS(UPIPE_HELPER_OPTION_DEFINE)                                         \
                                                                            \
static const struct upipe_option STRUCTURE##_options[] = {                  \
    OPTIONS(UPIPE_HELPER_OPTION_DECLARE)                                    \
};                                                                          \
                                                                            \
/** @This initializes all options to NULL. */                               \
static void STRUCTURE##_init_options(struct upipe *upipe)                   \
{                                                                           \
    for (unsigned i = 0; i < UBASE_ARRAY_SIZE(STRUCTURE##_options); i++)    \
        STRUCTURE##_options[i].init(upipe);                                 \
}                                                                           \
                                                                            \
/** @This frees all options. */                                             \
static void STRUCTURE##_clean_options(struct upipe *upipe)                  \
{                                                                           \
    for (unsigned i = 0; i < UBASE_ARRAY_SIZE(STRUCTURE##_options); i++)    \
        STRUCTURE##_options[i].set(upipe, NULL);                            \
}                                                                           \
                                                                            \
/** @This looks for an option named option and sets it to value, return     \
 * UBASE_ERR_UNHANDLED if no option is found.                               \
 */                                                                         \
static int STRUCTURE##_handle_set_options(struct upipe *upipe,              \
                                          const char *option,               \
                                          const char *value)                \
{                                                                           \
    for (unsigned i = 0; i < UBASE_ARRAY_SIZE(STRUCTURE##_options); i++)    \
        if (!strcmp(option, STRUCTURE##_options[i].name))                   \
            return STRUCTURE##_options[i].set(upipe, value);                \
    return UBASE_ERR_UNHANDLED;                                             \
}                                                                           \
                                                                            \
/** @This looks for an option named option and gets its value, return       \
 * UBASE_ERR_UNHANDLED if no option is found.                               \
 */                                                                         \
static int STRUCTURE##_handle_get_options(struct upipe *upipe,              \
                                          const char *option,               \
                                          const char **value)               \
{                                                                           \
    for (unsigned i = 0; i < UBASE_ARRAY_SIZE(STRUCTURE##_options); i++)    \
        if (!strcmp(option, STRUCTURE##_options[i].name))                   \
            return STRUCTURE##_options[i].get(upipe, value);                \
    return UBASE_ERR_UNHANDLED;                                             \
}

#ifdef __cplusplus
}
#endif
#endif /* !_UPIPE_UPIPE_HELPER_OPTIONS_H_ */
