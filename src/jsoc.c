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
int tokenizeJSON(char *buffer, Token* tokens, int max_tokens)
{
    int line = 1, col = 1;
    char* ptr = buffer;
    TokenizerState state = STATE_START;
    int token_count = 0;
    char temp[256];
    int temp_idx;
    int line_count = 0, col_pos = 0;

    // int token
    while (*ptr != '\0' && token_count < max_tokens)
    {
        bool is_minus = false;
        bool is_period = false;
        if (*ptr == '\n') {
            line_count++;
            col_pos = 0;
        }
        
        switch (state) {
            case STATE_START: {
                if (isspace(*ptr)) ptr++; // handled WHITESPACE

                else if (isdigit(*ptr) || *ptr == '-') {
                    state = STATE_IN_NUMBER;
                    temp_idx = 0;
                    if (*ptr == '-') {
                        is_minus = true;
                        temp[temp_idx++] = *ptr++;
                    }
                } else if (*ptr == '"') {
                    state = STATE_IN_STRING;
                    temp_idx = 0;
                    ptr++; // Skip opening quote
                } else if (*ptr == '[') {
                    state = STATE_IN_ARRAY;
                    tokens[token_count++].type = JSON_OBRACKET; // FUCK
                    ptr++; // Skip opening bracket
                    // temp_idx = 0;
                } else if (*ptr == ',') {
                    // assert that we are inside an array
                    // or an object
                    // probably will be done by the parser tho
                    tokens[token_count++].type = JSON_COMMA;
                    ptr++; // Skip comma
                } else if (*ptr == '{') {
                    state = STATE_IN_OBJECT;
                    tokens[token_count++].type = JSON_OCURLY;
                    ptr++; // Skip opening curly
                    // temp_idx = 0;
                } else if (*ptr == 't') { // true
                    state = STATE_IN_TRUE;
                    tokens[token_count].type = JSON_TRUE;
                } else if (*ptr == 'f') { // false
                    state = STATE_IN_FALSE;
                    tokens[token_count].type = JSON_FALSE;
                } else if (*ptr == 'n') { // null
                    state = STATE_IN_NULL;
                    tokens[token_count].type = JSON_NULL;
                } else {
                    state = STATE_IN_UNKNOWN;
                    tokens[token_count].type = UNKNOWN;
                }
                break;
            }
            case STATE_IN_NUMBER: {
                if (is_minus) {
                    state = STATE_IN_UNKNOWN;
                    break;
                }
                if (is_period) {
                    state = STATE_IN_UNKNOWN;
                    break;
                }

                if (*ptr == '.') {
                    is_period = true;
                    temp[temp_idx++] = *ptr++;
                } else if (*ptr == 'e' || *ptr == 'E') { // Exponent
                    char next = *(ptr + 1);
                    if (next != '-' && next != '+') {
                        state = STATE_IN_UNKNOWN;
                        break;
                    }
                    
                    // 1 or more digits
                    char next2 = *(ptr + 2);
                    if (!isdigit(next2)) {
                        state = STATE_IN_UNKNOWN;
                        break;
                    }
                    
                    temp[temp_idx++] = *ptr++;
                    
                } else if (isdigit(*ptr)) {
                    temp[temp_idx++] = *ptr++;
                } else {
                    temp[temp_idx] = '\0';
                    tokens[token_count].type = JSON_NUMBER;
                    strcpy(tokens[token_count].value, temp);
                    token_count++;
                    state = STATE_START;
                }
                break;
            }

            case STATE_IN_STRING: {
                if (*ptr == '\\') { /* TODO: handle case where \\\\\\\\ -> Only even amount of \'s allowed */
                    char next = *(ptr + 1);
                    char* next_ptr = ptr + 1;
                    int backslash_count = 1, i = 0;
                    while (*(next_ptr + i) == '\\') {
                        backslash_count++;
                        i++;
                    }
                    if (backslash_count % 2 != 0) {
                        state = STATE_IN_UNKNOWN;
                        break;
                    }
                    // The number of backslashes will be halved after this
                    
                    if (next == '"' ||
                        next == '/' ||
                        next == '\\' ||
                        next == 'b' ||
                        next == 'f' ||
                        next == 'n' ||
                        next == 'r' ||
                        next == 't' ||
                        (next == 'u' && is_hex(*(ptr + 2)) && is_hex(*(ptr + 3)) && is_hex(*(ptr + 4)) && is_hex(*(ptr + 5)))) { // u followed by 4 hex digits => unicode code point
                        ptr++; // Skip the backslash \ character and just increment the pointer, the string will contain the escaped symbol
                    }
                    else { // We have \ but no valid escape characters
                        state = STATE_IN_UNKNOWN;
                        break;
                    }
                }
                if (*ptr == '"' && *(ptr - 1) != 0x5c) { // 0x5c == '\'
                    temp[temp_idx] = '\0';
                    tokens[token_count].type = JSON_STRING;
                    strcpy(tokens[token_count].value, temp);
                    token_count++;
                    ptr++; // Skip closing quote
                    state = STATE_START;
                } else {
                    temp[temp_idx++] = *ptr++;
                }
                break;
            }

            case STATE_IN_ARRAY: {
                
                if (*ptr == ']') {
                    state = STATE_START;
                    ptr++; // Skip closing bracket
                    tokens[token_count].type = JSON_CBRACKET;
                    token_count++;
                } else {
                    state = STATE_START;
                }
                break;
            }
            case STATE_IN_OBJECT: {
                if (*ptr == '}') {
                    state = STATE_START;
                    ptr++;
                    tokens[token_count].type = JSON_CCURLY;
                    token_count++;
                } else {
                    state = STATE_START;
                }
                break;
            }
            case STATE_IN_TRUE: {
                ptr += 4; // 4 bytes of 'true'
                token_count++;
                state = STATE_START;
                break;
            }
            case STATE_IN_FALSE: {
                ptr += 5; // 5 bytes of 'false'
                token_count++;
                state = STATE_START;
                break;
            }
            case STATE_IN_NULL: {
                ptr += 4; // 4 bytes of 'null'
                token_count++;
                state = STATE_START;
                break;
            }
            case STATE_IN_UNKNOWN: {
                // We dont know what the token is, we just increment
                ptr++;
                state = STATE_START;
                break;
            }
        }

        return token_count;
    }
}

int main()
{
    const char *filename = "example.json";
    // dynarray_t* tokens = da_create(sizeof(Token), 256, NULL);

    char* buffer;
    readFileIntoBuffer(&buffer ,filename);

    printString(buffer);
    size_t buf_length = strlen(buffer);


    Token tokens[buf_length]; // VLA: potentially dangerous ⚠️
    // tokenizeJSON(buffer, buf_length, tokens);

    // for (int i = 0; i < 100; i++) {
    //     da_push(tokens, &i);
    // }
    // size_t da_len = da_size(tokens);
    // for (size_t i = 0; i < da_len; i++) {
    //     int elem = *(int*) da_get(tokens, i);
    //     printf("Element nr %zu is %i\n", i, elem);
    // }

    // da_free(tokens);
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
