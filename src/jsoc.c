#include "../include/jsoc.h"
#include "util.h"

#include "dynarray.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#define MAX_READ 1024 * 50

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

// typedef struct {
//     int pos, col, row;
// } Cur;

// typedef struct {
//     const char *content;
//     size_t count;
//     const char *file_path;

//     Cur cur;

//     TokenType token;
//     String_Builder string;
//     size_t row, col;
// } Lexer;

bool is_hex(char c) {
    c = tolower(c);
    return (c == 'a') || (c == 'b') || (c == 'c') || (c == 'd') || (c == 'e') || (c == 'f') || ((0 <= c - '0') && (c - '0' <= 9));
}

bool is_value(const char* str) {
    return false;
}

// tokenizes buffer into tokens array then returns number of tokens
int tokenizeJSON(char *buffer, Tokens *tokens, int max_tokens)
{
    char* ptr = buffer;
    TokenizerState state = STATE_START;
    int token_count = 0;
    char temp[256];
    int temp_idx = 0;
    
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
                    tokens->items[token_count].type = JSON_OBRACKET;
                    tokens->items[token_count].value[0] = '\0';
                    token_count++;
                    ptr++;
                } else if (*ptr == ',') {
                    tokens->items[token_count].type = JSON_COMMA;
                    tokens->items[token_count].value[0] = '\0';
                    token_count++;
                    ptr++;
                } else if (*ptr == ':') {
                    // Skip colon as it is not a required token in this tokenizer's design
                    ptr++;
                } else if (*ptr == '{') {
                    tokens->items[token_count].type = JSON_OCURLY;
                    tokens->items[token_count].value[0] = '\0';
                    token_count++;
                    ptr++;
                } else if (*ptr == ']') {
                    tokens->items[token_count].type = JSON_CBRACKET;
                    tokens->items[token_count].value[0] = '\0';
                    token_count++;
                    ptr++;
                } else if (*ptr == '}') {
                    tokens->items[token_count].type = JSON_CCURLY;
                    tokens->items[token_count].value[0] = '\0';
                    token_count++;
                    ptr++;
                } else if (*ptr == 't') {
                    if (strncmp(ptr, "true", 4) == 0) {
                        tokens->items[token_count].type = JSON_TRUE;
                        strncpy(tokens->items[token_count].value, "true", 4);
                        tokens->items[token_count].value[4] = '\0';
                        token_count++;
                        ptr += 4;
                    } else {
                        state = STATE_IN_UNKNOWN;
                    }
                } else if (*ptr == 'f') {
                    if (strncmp(ptr, "false", 5) == 0) {
                        tokens->items[token_count].type = JSON_FALSE;
                        strncpy(tokens->items[token_count].value, "false", 5);
                        tokens->items[token_count].value[5] = '\0';
                        token_count++;
                        ptr += 5;
                    } else {
                        state = STATE_IN_UNKNOWN;
                    }
                } else if (*ptr == 'n') {
                    if (strncmp(ptr, "null", 4) == 0) {
                        tokens->items[token_count].type = JSON_NULL;
                        strncpy(tokens->items[token_count].value, "null", 4);
                        tokens->items[token_count].value[4] = '\0';
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
                    tokens->items[token_count].type = JSON_NUMBER;
                    strncpy(tokens->items[token_count].value, temp, 255);
                    token_count++;
                    state = STATE_START;
                }
                break;
            }
            case STATE_IN_STRING: {
                if (*ptr == '\\') {
                    // Just skip the escape character for now as a simple implementation
                    ptr++; 
                    if (*ptr != '\0') temp[temp_idx++] = *ptr++;
                } else if (*ptr == '"') {
                    temp[temp_idx] = '\0';
                    tokens->items[token_count].type = JSON_STRING;
                    strncpy(tokens->items[token_count].value, temp, 255);
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
            // Array and Object states were redundant in START state for simple tokenization
            default:
                state = STATE_START;
                break;
        }
    }
    return token_count;
}

int main()
{
    const char *filename = "example.json";
    // dynarray_t* tokens = da_create(sizeof(Token), 256, NULL);

    char* buffer;
    readFileIntoBuffer(&buffer ,filename);

    printString(buffer);
    size_t buf_length = strlen(buffer);


    // Correct usage of tokens pointer
    Tokens tokens = {0};
    tokens.items = malloc(sizeof(Token) * 1024);
    tokens.capacity = 1024;
    
    int token_count = tokenizeJSON(buffer, &tokens, 1024);

    for (int i = 0; i < token_count; i++) {
        const char *type_names[] = {
            "OCURLY", "CCURLY", "OBRACKET", "CBRACKET", "COMMA", "NUMBER", "STRING", "TRUE", "FALSE", "NULL", "UNKNOWN"
        };
        printf("TOKEN %d: Type %s, Value: '%s'\n", i, type_names[tokens.items[i].type], tokens.items[i].value);
    }

    free(tokens.items);
    free(buffer);

    // Flush stdout to ensure output is displayed
    // fflush(stdout);

    // if (ferror(fp))
    // {

    //     puts("I/O error when reading");
    // }
    // else if (feof(fp))
    // {
    //     // puts("End of file is reached successfully");
    //     fclose(fp);
    //     return EXIT_SUCCESS;
    // }


    return 0;
}
