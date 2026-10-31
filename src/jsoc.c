#include "../include/jsoc.h"
#include "util.h"

#include <stdbool.h>
#include "tokenizer.h"

#define MAX_READ 1024 * 50




int main()
{
    const char *filename = "example.json";
    // dynarray_t* tokens = da_create(sizeof(Token), 256, NULL);

    char* buffer;
    readFileIntoBuffer(&buffer ,filename);

    printString(buffer);
    size_t buf_length = strlen(buffer);


    // Use dynamic array functions instead of direct allocation
    dynarray_t *tokens = da_create(sizeof(Token), 1024, NULL);
    
    // Pass the dynarray pointer to the tokenizer
    int token_count = tokenizeJSON(buffer, tokens, 1024);

    for (int i = 0; i < token_count; i++) {
        Token *t = (Token*)da_get(tokens, i);
        const char *type_names[] = {
            "OCURLY", "CCURLY", "OBRACKET", "CBRACKET", "COMMA", "NUMBER", "STRING", "TRUE", "FALSE", "NULL", "UNKNOWN"
        };
        printf("TOKEN %d: Type %s, Value: '%s'\n", i, type_names[t->type], t->value);
    }

    da_free(tokens);
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
