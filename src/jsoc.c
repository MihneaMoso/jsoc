#include "../include/jsoc.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dynarray.h"

#define MAX_READ 1024 * 50

typedef enum {
    OPEN_CURLY,
    CLOSED_CURLY,
    OPEN_PAREN,
    CLOSED_PAREN,
    OPEN_BRACKET,
    CLOSED_BRACKET,
    NUMBER,
    STRING,
} Token;

void writeStringToFile(const char *string, const char *filename)
{
    FILE *fp = fopen(filename, "w");
    if (!fp)
    {
        perror("Error reading file :(");
        return;
    }
    fprintf(fp, string);
    fclose(fp);
}

void appendStringToFile(const char *string, const char *filename)
{
    FILE *fp = fopen(filename, "a");
    if (!fp)
    {
        perror("Error reading file :(");
        return;
    }
    fprintf(fp, string);
    fclose(fp);
}

bool readFileIntoBuffer(char **buffer, const char *filename)
{
    *buffer = NULL;
    int64_t length = 0;
    FILE *fp = fopen(filename, "rb");
    if (!fp)
    {
        perror("Couldn't open file\n");
        return false;
    }
    fseek(fp, 0, SEEK_END);
    length = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // Allocate buffer with space for null terminator
    *buffer = malloc(length + 1);
    // why can't we do
    // defer free(buffer);
    // like in zig!?
    if (!*buffer)
    {
        perror("Couldn't allocate using malloc\n");
        fclose(fp);
        return false;
    }
    fread(*buffer, 1, length, fp);
    (*buffer)[length] = '\0'; // Null terminate
    fclose(fp);
    return true; // Success!
}

void printString(char *string)
{
    size_t len = strlen(string);
    for (size_t i = 0; i < len; i++)
    {
        putchar(string[i]);
    }
}

void tokenizeJSON(char *buffer, size_t length, dynarray_t* tokens)
{
    for (size_t i = 0; i < length; i++)
    {
        Token token;
        char ch = buffer[i];
        switch (ch)
        {
        case '{':
        {
            token = OPEN_CURLY;
            break;
        }
        case '}':
        {
            token = CLOSED_CURLY;
            break;
        }
        case '(':
        {
            token = OPEN_PAREN;
            break;
        }
        case ')':
        {
            token = CLOSED_PAREN;
            break;
        }
        case '[':
        {
            token = OPEN_BRACKET;
            break;
        }
        case ']':
        {
            token = CLOSED_BRACKET;
            break;
        }
        default:
        {
            putchar(ch);
            putchar(' ');
            break;
        }
        }
        da_push(tokens, (Token*) token);
    }
}

int main()
{
    const char *filename = "example.json";
    dynarray_t* tokens = da_create(sizeof(Token), 256, NULL);
    
    char* buffer;
    readFileIntoBuffer(&buffer ,filename);
    printString(buffer);
    size_t buf_length = strlen(buffer);
    // tokenizeJSON(buffer, buf_length, tokens);

    for (int i = 0; i < 100; i++) {
        da_push(tokens, &i);
    }
    size_t da_len = da_size(tokens);
    for (size_t i = 0; i < da_len; i++) {
        int elem = *(int*) da_get(tokens, i);
        printf("Element nr %zu is %i\n", i, elem);
    }
    
    free(buffer);
    da_free(tokens);

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
