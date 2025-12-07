#include "../include/jsoc.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstdint>

#define MAX_READ 1024 * 50

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

bool readFileIntoBuffer(char *buffer, const char *filename)
{
    buffer = 0;
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
    buffer = malloc(length);
    // why can't we do
    // defer free(buffer);
    // like in zig!?
    if (!buffer)
    {
        perror("Could't allocate using malloc\n");
        return false;
    }
    fread(buffer, 1, length, fp);
    fclose(fp);
}

int main()
{
    const char *filename = "example.json";
    FILE *fp = fopen(filename, "r+");
    if (!fp)
    {
        perror("Error reading file :(");
        return EXIT_FAILURE;
    }

    appendStringToFile("\n{'test': 'hello'}", filename);

    int c;
    while ((c = fgetc(fp)) != EOF)
    {
        char ch = c;
        switch (ch)
        {
        case '{':
        {
            putchar(ch);
            putchar(' ');
            break;
        }
        case '}':
        {
            putchar(ch);
            putchar(' ');
            break;
        }
        default:
        {
            putchar(ch);
            putchar(' ');
            break;
        }
        }
    }

    if (ferror(fp)) {

        puts("I/O error when reading");
    }
    else if (feof(fp))
    {
        // puts("End of file is reached successfully");
        return EXIT_SUCCESS;
    }

    fclose(fp);
    // remove(filename);
    // printf("Hello, World\n");
    return 0;
}
