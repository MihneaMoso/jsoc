#include "token.h"
#include <limits.h>
#include <stdio.h>

JSOC_API void jsoc_token_print(const jsoc_token *tok, const char *input,
                               char *buf, size_t bufsz) {
  if (!tok || !input || !buf || bufsz == 0)
    return;

  const char *type_name = jsoc_token_type_name(tok->type);
  if (tok->length == 0) {
    snprintf(buf, bufsz, "%s", type_name);
    return;
  }

  /* `%.*s` takes an int precision; clamp so huge tokens cannot wrap. */
  size_t len = tok->length;
  size_t max = bufsz - 1;
  if (len > max)
    len = max;
  if (len > (size_t)INT_MAX)
    len = (size_t)INT_MAX;

  snprintf(buf, bufsz, "%s(%.*s)", type_name, (int)len, input + tok->offset);
}
