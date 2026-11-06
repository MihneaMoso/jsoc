#pragma once
#include "export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum jsoc_error_code : uint8_t {
  JSOC_OK = 0,
  JSOC_ERR_NOMEM,
  JSOC_ERR_SYNTAX,
  JSOC_ERR_UNTERMINATED_STRING,
  JSOC_ERR_UNTERMINATED_ARRAY,
  JSOC_ERR_UNTERMINATED_OBJECT,
  JSOC_ERR_INVALID_ESCAPE,
  JSOC_ERR_INVALID_UNICODE,
  JSOC_ERR_INVALID_NUMBER,
  JSOC_ERR_NUMBER_OVERFLOW,
  JSOC_ERR_EXPECTED_VALUE,
  JSOC_ERR_EXPECTED_COMMA_OR_END,
  JSOC_ERR_EXPECTED_COLON,
  JSOC_ERR_EXPECTED_KEY,
  JSOC_ERR_TRAILING_GARBAGE,
  JSOC_ERR_DEPTH_EXCEEDED,
  JSOC_ERR_INVALID_UTF8,
  JSOC_ERR_INVALID_CONTROL,
  JSOC_ERR_INPUT_TOO_LARGE,
  JSOC_ERR_USER_STOP,
} jsoc_error_code;

typedef struct {
  jsoc_error_code code;
  size_t offset;
  size_t line;
  size_t column;
} jsoc_error;

JSOC_API const char *
jsoc_error_message(jsoc_error_code code) JSOC_CONST JSOC_NODISCARD;
JSOC_API void jsoc_error_format(const jsoc_error *err, char *buf, size_t bufsz);

#define JSOC_ERR_INIT {JSOC_OK, 0, 0, 0}
#define JSOC_ERR_AT(code, off) ((jsoc_error){(code), (off), 0, 0})

#if JSOC_C23
#define JSOC_TRY(expr)                                                         \
  ({                                                                           \
    auto _err = (expr);                                                        \
    if (_err.code != JSOC_OK)                                                  \
      return _err;                                                             \
  })
#else
#define JSOC_TRY(expr)                                                         \
  do {                                                                         \
    jsoc_error _err = (expr);                                                  \
    if (_err.code != JSOC_OK)                                                  \
      return _err;                                                             \
  } while (0)
#endif
#ifdef __cplusplus
}
#endif
