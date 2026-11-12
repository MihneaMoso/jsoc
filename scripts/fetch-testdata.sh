#!/usr/bin/env bash
#
# Fetch the conformance corpora that make the test suite meaningful:
#
#   jsonchecker/     from simdjson-data (11 MB): fail*/pass*/adversarial/...
#   jsonexamples/    from simdjson-data (24 MB): real-world documents
#   JSONTestSuite/   test_parsing + test_parse_should_be_fast (1 MB)
#
# Run once; keep the results in JSOC_TEST_DATA (default ~/simdjson-data).
set -euo pipefail

dir="${JSOC_TEST_DATA:-$HOME/simdjson-data}"
mkdir -p "$(dirname "$dir")"

if [[ ! -f "$dir/jsonchecker/fail01.json" ]] || [[ ! -f "$dir/jsonexamples/twitter.json" ]]; then
  echo "== cloning simdjson-data (jsonchecker + jsonexamples)"
  git clone --depth 1 https://github.com/simdjson/simdjson-data.git "$dir"
fi

json_test_suite="$dir/JSONTestSuite"
if [[ ! -d "$json_test_suite/test_parsing" ]]; then
  echo "== cloning JSONTestSuite"
  git clone --depth 1 https://github.com/nst/JSONTestSuite.git "$json_test_suite"
fi

echo "== ready:"
printf '  %-28s %s\n' \
  "$dir/jsonchecker"      "$(find "$dir/jsonchecker" -name '*.json' | wc -l) files" \
  "$dir/jsonexamples"     "$(find "$dir/jsonexamples" -name '*.json' | wc -l) files" \
  "$dir/JSONTestSuite"    "$(find "$dir/JSONTestSuite" -name '*.json' | wc -l) files"
