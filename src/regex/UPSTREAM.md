# RE2 upstream

This engine is a port of [RE2](https://github.com/google/re2), BSD-3-Clause, see
`LICENSE.re2`. It was written against commit
`2da0056` (2026-10-02), without Abseil. The design and every deliberate
difference are in `feature/regex.md`.

| fiber::regex | RE2 |
|---|---|
| `Parser.cpp` | `re2/parse.cc`, `re2/perl_groups.cc` |
| `Ast.cpp`, `CharClass.cpp` | `re2/regexp.cc` |
| `Simplify.cpp` | `re2/simplify.cc` |
| `Compiler.cpp` | `re2/compile.cc` |
| `Program.cpp` | `re2/prog.cc` |
| `PikeVm.cpp` | `re2/nfa.cc` |
| `BitState.cpp` | `re2/bitstate.cc` |
| `OnePass.cpp` | `re2/onepass.cc` |
| `Dfa.cpp` | `re2/dfa.cc` |
| `Regex.cpp` | the dispatch in `re2/re2.cc` |
| `RegexSet.cpp` | `re2/set.cc` |
| `Sparse.h` | `re2/sparse_array.h`, `re2/sparse_set.h`, `re2/bitmap256.h` |

Differences that matter when comparing the two:

- No reference counting and no `Walker`: nodes live in an arena, and the parser's
  height limit makes plain recursion safe.
- Case folding is ASCII only and there are no Unicode classes, so the Unicode
  tables are gone.
- `Encoding::Bytes` decodes the pattern byte by byte instead of transcoding from
  Latin-1, so error offsets refer to the original pattern.
- Positions are offsets, not pointers, and all matching memory comes from
  `Scratch`.
- Unsupported syntax is reported as `ErrorCode::UnsupportedFeature`, and `\C` is
  rejected.
- The DFA never fails. Its caches belong to the worker's `Scratch`, so there are no
  locks or atomics; a shortage of memory degrades to stepping without a cache. This
  also gives `RegexSet` the fallback `RE2::Set` lacks.
- The reverse program is compiled up front, so a `Regex` never changes once built.
- `LiteralSet` and the streaming `Scanner` have no RE2 counterpart.
