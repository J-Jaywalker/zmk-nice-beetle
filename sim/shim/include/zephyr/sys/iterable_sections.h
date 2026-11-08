/*
 * Simulator replacement for Zephyr's iterable sections.
 *
 * Zephyr collects these with its own linker script.  On Windows/PE we use the
 * linker's "$" grouping: every element of section NAME goes into input section
 * ".zsim$NAME_b", and sim/shim/src/sections.c places zero-sized start/end
 * markers in ".zsim$NAME_a" / ".zsim$NAME_c".  ld sorts grouped sections by the
 * text after "$", so elements end up contiguous between the markers.
 */
#ifndef INCLUDE_ZEPHYR_SYS_ITERABLE_SECTIONS_H_
#define INCLUDE_ZEPHYR_SYS_ITERABLE_SECTIONS_H_

#include <zephyr/sys/__assert.h>
#include <zephyr/toolchain.h>

#define SIM_SECTION_STR(name, sfx) ".zsim$" #name sfx
#define SIM_IN_SECTION(name, sfx) __attribute__((section(SIM_SECTION_STR(name, sfx)), used))

#define TYPE_SECTION_ITERABLE(type, varname, secname, section_postfix)                             \
    type varname SIM_IN_SECTION(secname, "_b")

#define TYPE_SECTION_START(secname) _CONCAT(_##secname, _list_start)
#define TYPE_SECTION_END(secname) _CONCAT(_##secname, _list_end)
#define TYPE_SECTION_START_EXTERN(type, secname) extern type TYPE_SECTION_START(secname)[]
#define TYPE_SECTION_END_EXTERN(type, secname) extern type TYPE_SECTION_END(secname)[]

#define TYPE_SECTION_FOREACH(type, secname, iterator)                                              \
    TYPE_SECTION_START_EXTERN(type, secname);                                                      \
    TYPE_SECTION_END_EXTERN(type, secname);                                                        \
    for (type *iterator = TYPE_SECTION_START(secname); iterator < TYPE_SECTION_END(secname);       \
         iterator++)

#define TYPE_SECTION_GET(type, secname, i, dst)                                                    \
    do {                                                                                           \
        TYPE_SECTION_START_EXTERN(type, secname);                                                  \
        *(dst) = &TYPE_SECTION_START(secname)[i];                                                  \
    } while (0)

#define TYPE_SECTION_COUNT(type, secname, dst)                                                     \
    do {                                                                                           \
        TYPE_SECTION_START_EXTERN(type, secname);                                                  \
        TYPE_SECTION_END_EXTERN(type, secname);                                                    \
        *(dst) = ((uintptr_t)TYPE_SECTION_END(secname) - (uintptr_t)TYPE_SECTION_START(secname)) / \
                 sizeof(type);                                                                     \
    } while (0)

#define STRUCT_SECTION_START(struct_type) TYPE_SECTION_START(struct_type)
#define STRUCT_SECTION_START_EXTERN(struct_type) TYPE_SECTION_START_EXTERN(struct struct_type, struct_type)
#define STRUCT_SECTION_END(struct_type) TYPE_SECTION_END(struct_type)
#define STRUCT_SECTION_END_EXTERN(struct_type) TYPE_SECTION_END_EXTERN(struct struct_type, struct_type)

#define STRUCT_SECTION_ITERABLE_ALTERNATE(secname, struct_type, varname)                           \
    TYPE_SECTION_ITERABLE(struct struct_type, varname, secname, varname)
#define STRUCT_SECTION_ITERABLE(struct_type, varname)                                              \
    STRUCT_SECTION_ITERABLE_ALTERNATE(struct_type, struct_type, varname)
#define STRUCT_SECTION_ITERABLE_NAMED(struct_type, name, varname)                                  \
    STRUCT_SECTION_ITERABLE_ALTERNATE(struct_type, struct_type, varname)
#define STRUCT_SECTION_ITERABLE_ARRAY(struct_type, varname, size)                                  \
    struct struct_type varname[size] SIM_IN_SECTION(struct_type, "_b")

#define STRUCT_SECTION_FOREACH_ALTERNATE(secname, struct_type, iterator)                           \
    TYPE_SECTION_FOREACH(struct struct_type, secname, iterator)
#define STRUCT_SECTION_FOREACH(struct_type, iterator)                                              \
    STRUCT_SECTION_FOREACH_ALTERNATE(struct_type, struct_type, iterator)
#define STRUCT_SECTION_GET(struct_type, i, dst)                                                    \
    TYPE_SECTION_GET(struct struct_type, struct_type, i, dst)
#define STRUCT_SECTION_COUNT(struct_type, dst)                                                     \
    TYPE_SECTION_COUNT(struct struct_type, struct_type, dst)

/* Defines the start/end markers for an iterable section (used in sections.c). */
#define SIM_SECTION_MARKERS(type, secname)                                                         \
    type TYPE_SECTION_START(secname)[0] SIM_IN_SECTION(secname, "_a");                             \
    type TYPE_SECTION_END(secname)[0] SIM_IN_SECTION(secname, "_c")

#endif
