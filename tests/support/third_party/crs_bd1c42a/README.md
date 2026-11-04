# OWASP CRS regular expressions

Test data from the [OWASP Core Rule Set](https://github.com/coreruleset/coreruleset)
at commit `bd1c42a`. Apache-2.0, see `LICENSE`.

- `rx_patterns.txt`: every `@rx` pattern, as `file:line<TAB>pattern`.
- `rule_groups.txt`: which rules look at the same text and so can share a `RegexSet`.
- `payloads.txt`: attack payloads from the CRS regression tests, hex encoded, with the
  rules that match each one according to Go's `regexp`.
- `data/`: the word lists behind `@pmFromFile`, unmodified.

`tests/RegexCrsTest.cpp` uses these as the acceptance set for `fiber::regex`, and
`example/regex_crs_bench.cpp` as its benchmark. None of this enters `fiber_lib`.

To regenerate, run `scripts/regex/extract_crs.py` and then
`scripts/regex/gen_crs_golden.go`.
