#include <string.h>
#include <ctype.h>
#include <stdbool.h>

typedef struct {
    const char* data;
    size_t count;
} String_View;

#define SV_Fmt "%.*s"
#define SV_Arg(s) (s).count, (s).data

String_View sv(const char *cstr) {
    return (String_View) {
        .data = cstr,
        .count = strlen(cstr),
    };
}

void sv_chop_left (String_View *sv, size_t n) {
    if (n > sv->count) n = sv->count;
    sv->count -= n;
    sv-> data += n;
}

void sv_chop_right(String_View* sv, size_t n) {
    if (n > sv->count) n = sv->count;
    sv->count -= n;
}

void sv_trim_left(String_View *sv) {
    while (sv->count > 0 && isspace((sv->data[0])))
    {
        sv_chop_left(sv, 1);
    }
}

void sv_trim_right(String_View *sv) {
    while (sv-> count > 0 && isspace(sv->data[sv->count - 1]))
    {
        sv_chop_right(sv, 1);
    }
}

void sv_trim(String_View *sv) {
    sv_trim_left(sv);
    sv_trim_right(sv);
}

String_View sv_chop_by_delim(String_View *sv, char delim, bool include_delim) {
    int delim_inc = include_delim ? 0 : 1;
    size_t i = 0;
    while (i < sv->count && sv->data[i] != delim) {
        i += 1;
    }
    if (i < sv->count) {
        String_View result = {
            .count = i,
            .data = sv->data,
        };
        sv_chop_left(sv, i + delim_inc);
        return result;
    }

    String_View result = *sv;
    sv_chop_left(sv, sv->count);
    return result;

}
