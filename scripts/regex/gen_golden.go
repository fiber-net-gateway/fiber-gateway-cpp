// Generates tests/support/regex/golden.txt, the expected results that
// tests/RegexGoldenTest.cpp checks fiber::regex against. Go's regexp package is
// the oracle: it shares RE2's syntax and leftmost-first semantics, and running it
// here keeps it out of the build.
//
//	go run scripts/regex/gen_golden.go -cases tests/support/regex/re2_search_cases.txt \
//	    -out tests/support/regex/golden.txt
//
// Each output line is four tab-separated fields: the mode (U for Encoding::Utf8,
// B for Encoding::Bytes), the pattern and the text as hex, and either "-" for no
// match or the submatch offsets, comma separated, with "x" for an unset group.
package main

import (
	"bufio"
	"encoding/hex"
	"flag"
	"fmt"
	"math/rand"
	"os"
	"regexp"
	"strings"
	"unicode/utf8"
)

// Go has no byte mode, but a byte-per-rune transcoding gives it one: every byte
// becomes the rune of the same value, and rune indexes are then byte offsets.
func latin1ToUtf8(b []byte) string {
	var sb strings.Builder
	for _, c := range b {
		sb.WriteRune(rune(c))
	}
	return sb.String()
}

func isASCII(b []byte) bool {
	for _, c := range b {
		if c >= 0x80 {
			return false
		}
	}
	return true
}

// Escape sequences and classes the engine rejects or defines differently.
var unsupported = regexp.MustCompile(`\\[pPC]`)

// Case folding is ASCII only here but Unicode in Go, and invalid UTF-8 text is
// matched by Go as U+FFFD but never by a UTF-8 program here. Neither says
// anything useful about the engine, so those cases are left out.
func comparable(mode byte, pattern, text []byte) bool {
	if unsupported.Match(pattern) {
		return false
	}
	fold := strings.Contains(string(pattern), "(?i") || strings.Contains(string(pattern), "i)") ||
		strings.Contains(string(pattern), "i:")
	if fold && (!isASCII(pattern) || !isASCII(text)) {
		return false
	}
	if mode == 'U' {
		return utf8.Valid(pattern) && utf8.Valid(text)
	}
	return true
}

func offsets(mode byte, text []byte, goText string, loc []int) string {
	if loc == nil {
		return "-"
	}
	// Byte offsets in the transcoded text become byte offsets in the original.
	toSource := func(i int) int {
		if mode == 'U' {
			return i
		}
		return utf8.RuneCountInString(goText[:i])
	}
	parts := make([]string, 0, len(loc)/2)
	for i := 0; i+1 < len(loc); i += 2 {
		if loc[i] < 0 {
			parts = append(parts, "x")
		} else {
			parts = append(parts, fmt.Sprintf("%d:%d", toSource(loc[i]), toSource(loc[i+1])))
		}
	}
	return strings.Join(parts, ",")
}

func emit(w *bufio.Writer, mode byte, pattern, text []byte) bool {
	if !comparable(mode, pattern, text) {
		return false
	}
	goPattern, goText := string(pattern), string(text)
	if mode == 'B' {
		goPattern, goText = latin1ToUtf8(pattern), latin1ToUtf8(text)
	}
	re, err := regexp.Compile(goPattern)
	if err != nil {
		return false
	}
	fmt.Fprintf(w, "%c\t%s\t%s\t%s\n", mode, hex.EncodeToString(pattern), hex.EncodeToString(text),
		offsets(mode, text, goText, re.FindStringSubmatchIndex(goText)))
	return true
}

// The flag and anchoring variants RE2's own tester runs each table entry under.
func variants(pattern string) []string {
	return []string{
		pattern,
		"(?i)" + pattern,
		"(?s)" + pattern,
		"(?m)" + pattern,
		`\A(?:` + pattern + `)\z`,
		"(?U)" + pattern,
	}
}

var atoms = []string{
	"a", "b", "c", ".", "[ab]", "[^a]", "[a-c]", `\w`, `\d`, `\s`, `\W`, "", "(?:)", "^", "$", `\b`, `\B`, `\A`, `\z`,
	"(a)", "(b)", "(?:a|b)", "(a|b)", "(a|ab)", "(a*)", "(a+)", "(?i:A)", "(?i)", "(?s:.)", "(?m:^)", "(?m:$)", "\n",
}

var operators = []string{
	"%s*", "%s+", "%s?", "%s*?", "%s+?", "%s??", "%s{2}", "%s{1,2}", "%s{0,2}", "%s{2,}", "%s{0}", "%s{2,3}?",
	"(%s)", "(?:%s)", "(?i:%s)",
}

var binaryOperators = []string{"%s%s", "%s|%s", "(%s)%s", "%s(%s)"}

func randomPattern(r *rand.Rand, depth int) string {
	if depth == 0 || r.Intn(4) == 0 {
		return atoms[r.Intn(len(atoms))]
	}
	if r.Intn(2) == 0 {
		return fmt.Sprintf(operators[r.Intn(len(operators))], randomPattern(r, depth-1))
	}
	return fmt.Sprintf(binaryOperators[r.Intn(len(binaryOperators))], randomPattern(r, depth-1), randomPattern(r, depth-1))
}

const textAlphabet = "abcAB_1 \n"

func randomText(r *rand.Rand) string {
	n := r.Intn(8)
	b := make([]byte, n)
	for i := range b {
		b[i] = textAlphabet[r.Intn(len(textAlphabet))]
	}
	return string(b)
}

// High bytes exercise Bytes mode, and valid multi-byte runes exercise Utf8 mode.
var highPatterns = []string{
	`.`, `..`, `[^a]`, `[^a]+`, `\x{ff}`, `\xc2\xb4`, `\x{c2}\x{b4}`, `[\x80-\xff]+`, `[\x{e0}-\x{ef}]`, `a.b`, `(.)(.)`,
	`[^\x00-\x7f]`, `\W+`, `\b.`, `.\b`,
}

var highTexts = []string{"\xff", "a\xffb", "\xc2\xb4", "a\xc2\xb4b", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "é", "aéb", "€uro"}

func main() {
	casesPath := flag.String("cases", "", "file of pattern<TAB>text lines, hex encoded")
	outPath := flag.String("out", "", "output file")
	random := flag.Int("random", 2000, "number of random patterns")
	seed := flag.Int64("seed", 1, "random seed")
	depth := flag.Int("depth", 4, "maximum depth of random patterns")
	texts := flag.Int("texts", 3, "random texts per random pattern")
	flag.Parse()

	out, err := os.Create(*outPath)
	if err != nil {
		panic(err)
	}
	defer out.Close()
	w := bufio.NewWriter(out)
	defer w.Flush()

	count := 0
	both := func(pattern, text []byte) {
		for _, mode := range []byte{'U', 'B'} {
			if emit(w, mode, pattern, text) {
				count++
			}
		}
	}

	cases, err := os.Open(*casesPath)
	if err != nil {
		panic(err)
	}
	defer cases.Close()
	scanner := bufio.NewScanner(cases)
	scanner.Buffer(make([]byte, 1<<20), 1<<24)
	for scanner.Scan() {
		fields := strings.Split(scanner.Text(), "\t")
		pattern, _ := hex.DecodeString(fields[0])
		text, _ := hex.DecodeString(fields[1])
		for _, variant := range variants(string(pattern)) {
			both([]byte(variant), text)
		}
	}

	for _, pattern := range highPatterns {
		for _, text := range highTexts {
			both([]byte(pattern), []byte(text))
		}
	}

	r := rand.New(rand.NewSource(*seed))
	for i := 0; i < *random; i++ {
		pattern := randomPattern(r, *depth)
		for j := 0; j < *texts; j++ {
			text := []byte(randomText(r))
			both([]byte(pattern), text)
			// Anchored, so that the one-pass engine gets to run.
			both([]byte(`\A(?:`+pattern+`)`), text)
		}
	}

	fmt.Fprintf(os.Stderr, "%d cases\n", count)
}
