#pragma once
#include <stddef.h>
#include <ctype.h>
#include "dynarray.h"
#include <string.h>

typedef enum {
    JSON_OCURLY,
    JSON_CCURLY,
    JSON_OBRACKET,
    JSON_CBRACKET,
    JSON_COMMA,
    JSON_NUMBER,
    JSON_STRING,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL,
    UNKNOWN
} TokenType;

typedef struct {
    TokenType type;

    char value[256];
    int line, col;
} Token;

typedef enum {
    STATE_START,
    STATE_IN_NUMBER,
    STATE_IN_ARRAY,
    STATE_IN_STRING,
    STATE_IN_OBJECT,
    STATE_IN_TRUE,
    STATE_IN_FALSE,
    STATE_IN_NULL,
    STATE_IN_UNKNOWN
} TokenizerState;

typedef struct {
    Token* items;
    size_t count;
    size_t capacity;
} Tokens;



bool is_hex(char c) {
    c = tolower(c);
    return (c == 'a') || (c == 'b') || (c == 'c') || (c == 'd') || (c == 'e') || (c == 'f') || ((0 <= c - '0') && (c - '0' <= 9));
}

bool is_value(const char* str) {
    return false;
}

// tokenizes buffer into tokens array then returns number of tokens
int tokenizeJSON(char *buffer, dynarray_t *tokens, int max_tokens)
{
    char* ptr = buffer;
    TokenizerState state = STATE_START;
    int token_count = 0;
    char temp[256];
    int temp_idx = 0;
    
    // Ensure capacity
    da_reserve(tokens, max_tokens);
    
    while (*ptr != '\0' && token_count < max_tokens)
    {
        // Skip whitespace
        if (state == STATE_START && isspace(*ptr)) {
            ptr++;
            continue;
        }

        switch (state) {
            case STATE_START: {
                if (isdigit(*ptr) || *ptr == '-') {
                    state = STATE_IN_NUMBER;
                    temp_idx = 0;
                    temp[temp_idx++] = *ptr++;
                } else if (*ptr == '"') {
                    state = STATE_IN_STRING;
                    temp_idx = 0;
                    ptr++; 
                } else if (*ptr == '[') {
                    Token t = { .type = JSON_OBRACKET };
                    t.value[0] = '\0';
                    da_push(tokens, &t);
                    token_count++;
                    ptr++;
                } else if (*ptr == ',') {
                    Token t = { .type = JSON_COMMA };
                    t.value[0] = '\0';
                    da_push(tokens, &t);
                    token_count++;
                    ptr++;
                } else if (*ptr == ':') {
                    ptr++;
                } else if (*ptr == '{') {
                    Token t = { .type = JSON_OCURLY };
                    t.value[0] = '\0';
                    da_push(tokens, &t);
                    token_count++;
                    ptr++;
                } else if (*ptr == ']') {
                    Token t = { .type = JSON_CBRACKET };
                    t.value[0] = '\0';
                    da_push(tokens, &t);
                    token_count++;
                    ptr++;
                } else if (*ptr == '}') {
                    Token t = { .type = JSON_CCURLY };
                    t.value[0] = '\0';
                    da_push(tokens, &t);
                    token_count++;
                    ptr++;
                } else if (*ptr == 't') {
                    if (strncmp(ptr, "true", 4) == 0) {
                        Token t = { .type = JSON_TRUE };
                        strncpy(t.value, "true", 4);
                        t.value[4] = '\0';
                        da_push(tokens, &t);
                        token_count++;
                        ptr += 4;
                    } else {
                        state = STATE_IN_UNKNOWN;
                    }
                } else if (*ptr == 'f') {
                    if (strncmp(ptr, "false", 5) == 0) {
                        Token t = { .type = JSON_FALSE };
                        strncpy(t.value, "false", 5);
                        t.value[5] = '\0';
                        da_push(tokens, &t);
                        token_count++;
                        ptr += 5;
                    } else {
                        state = STATE_IN_UNKNOWN;
                    }
                } else if (*ptr == 'n') {
                    if (strncmp(ptr, "null", 4) == 0) {
                        Token t = { .type = JSON_NULL };
                        strncpy(t.value, "null", 4);
                        t.value[4] = '\0';
                        da_push(tokens, &t);
                        token_count++;
                        ptr += 4;
                    } else {
                        state = STATE_IN_UNKNOWN;
                    }
                } else {
                    state = STATE_IN_UNKNOWN;
                }
                break;
            }
            case STATE_IN_NUMBER: {
                if (isdigit(*ptr) || *ptr == '.' || *ptr == 'e' || *ptr == 'E' || *ptr == '+' || *ptr == '-') {
                    temp[temp_idx++] = *ptr++;
                } else {
                    temp[temp_idx] = '\0';
                    Token t = { .type = JSON_NUMBER };
                    strncpy(t.value, temp, 255);
                    da_push(tokens, &t);
                    token_count++;
                    state = STATE_START;
                }
                break;
            }
            case STATE_IN_STRING: {
                if (*ptr == '\\') {
                    ptr++; 
                    if (*ptr != '\0') temp[temp_idx++] = *ptr++;
                } else if (*ptr == '"') {
                    temp[temp_idx] = '\0';
                    Token t = { .type = JSON_STRING };
                    strncpy(t.value, temp, 255);
                    da_push(tokens, &t);
                    token_count++;
                    ptr++;
                    state = STATE_START;
                } else {
                    temp[temp_idx++] = *ptr++;
                }
                break;
            }
            case STATE_IN_UNKNOWN: {
                ptr++;
                state = STATE_START;
                break;
            }
            default:
                state = STATE_START;
                break;
        }
    }
    return token_count;
}