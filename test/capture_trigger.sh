#!/bin/sh
set -eu

if [ "$#" -ne 1 ] || [ -z "${MIST_TEST_TRIGGER_OUTPUT:-}" ]; then
  exit 2
fi

output="${MIST_TEST_TRIGGER_OUTPUT}.$1"
temporary="${output}.tmp.$$"
trap 'rm -f -- "$temporary"' EXIT HUP INT TERM
{
  printf '%s\n' "$1"
  sed ''
} >"$temporary"
mv "$temporary" "$output"
trap - EXIT HUP INT TERM
