#include "error.h"
#include <stdio.h>

JSOC_API const char *jsoc_error_message(jsoc_error_code code) {
  switch (code) {
  case JSOC_OK:
    return "Success";
  case JSOC_ERR_NOMEM:
    return "Out of memory";
  case JSOC_ERR_SYNTAX:
    return "Syntax error";
  case JSOC_ERR_UNTERMINATED_STRING:
    return "Unterminated string";
  case JSOC_ERR_UNTERMINATED_ARRAY:
    return "Unterminated array";
  case JSOC_ERR_UNTERMINATED_OBJECT:
    return "Unterminated object";
  case JSOC_ERR_INVALID_ESCAPE:
    return "Invalid escape sequence";
  case JSOC_ERR_INVALID_UNICODE:
    return "Invalid Unicode escape";
  case JSOC_ERR_INVALID_NUMBER:
    return "Invalid number format";
  case JSOC_ERR_NUMBER_OVERFLOW:
    return "Number overflow";
  case JSOC_ERR_EXPECTED_VALUE:
    return "Expected value";
  case JSOC_ERR_EXPECTED_COMMA_OR_END:
    return "Expected ',' or ']'/'}'";
  case JSOC_ERR_EXPECTED_COLON:
    return "Expected ':' after key";
  case JSOC_ERR_EXPECTED_KEY:
    return "Expected string key";
  case JSOC_ERR_TRAILING_GARBAGE:
    return "Trailing garbage after value";
  case JSOC_ERR_DEPTH_EXCEEDED:
    return "Nesting depth exceeded";
  case JSOC_ERR_INVALID_UTF8:
    return "Invalid UTF-8 sequence";
  case JSOC_ERR_INVALID_CONTROL:
    return "Unescaped control character in string";
  case JSOC_ERR_INPUT_TOO_LARGE:
    return "Input too large (max 4 GiB)";
  case JSOC_ERR_USER_STOP:
    return "Parsing stopped by user";
  default:
    return "Unknown error";
  }
}

JSOC_API void jsoc_error_format(const jsoc_error *err, char *buf,
                                size_t bufsz) {
  if (!err || !buf || bufsz == 0)
    return;
  if (err->code == JSOC_OK) {
    snprintf(buf, bufsz, "OK");
    return;
  }
  if (err->line > 0 && err->column > 0) {
    snprintf(buf, bufsz, "Error at line %zu, column %zu (offset %zu): %s",
             err->line, err->column, err->offset,
             jsoc_error_message(err->code));
  } else {
    snprintf(buf, bufsz, "Error at offset %zu: %s", err->offset,
             jsoc_error_message(err->code));
  }
}