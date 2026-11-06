#pragma once

/*
 * Private DOM layout shared by the implementation and the white-box tests.
 * Nothing here is part of the stable ABI.
 */

#include "dom.h"
#include <stdint.h>

#define JSOC_NO_INDEX UINT32_MAX

struct jsoc_value {
    union {
        struct {
            uint32_t offset;  /* raw lexeme / string contents */
            uint32_t length;
        };
        struct {
            uint32_t first;   /* first child value (array) or member (object) */
            uint32_t count;   /* children or members */
        };
    };
    uint32_t next;            /* next sibling value */
    uint8_t type;
    uint8_t flags;
    uint16_t reserved;
};

struct jsoc_member {
    uint32_t key_offset;
    uint32_t key_length;
    uint32_t value;
    uint32_t next;
};
