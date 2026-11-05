# Regex test data

- `re2_search_cases.txt`: the `simple_tests` table of RE2's
  `re2/testing/search_test.cc`, hex encoded as `pattern<TAB>text`. BSD-3-Clause,
  see `LICENSE.re2`.
- `golden.txt`: expected results for those cases and for random patterns, from Go's
  `regexp` package. Regenerate with `scripts/regex/gen_golden.go`, whose header
  describes the format.

None of this is built into `fiber_lib`. Go is only needed to regenerate.
