#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
    *buffer = (char*) malloc(length + 1);
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