// Adds the expected results to the payloads of a CRS fixture, with Go's regexp
// package as the oracle:
//
//	go run scripts/regex/gen_crs_golden.go tests/support/third_party/crs_bd1c42a
//
// Each line of payloads.txt becomes the hex payload, a tab, and the comma-separated
// indexes of the rules that match it, in the order of rx_patterns.txt. Rules run the
// way ModSecurity runs @rx: on bytes, with DOTALL. See gen_golden.go for how Go gets
// a byte mode.
package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

func latin1ToUtf8(b []byte) string {
	var sb strings.Builder
	for _, c := range b {
		sb.WriteRune(rune(c))
	}
	return sb.String()
}

func readLines(path string) []string {
	file, err := os.Open(path)
	if err != nil {
		panic(err)
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 1<<20), 1<<24)
	var lines []string
	for scanner.Scan() {
		lines = append(lines, scanner.Text())
	}
	return lines
}

func main() {
	dir := os.Args[1]
	var rules []*regexp.Regexp
	for _, line := range readLines(filepath.Join(dir, "rx_patterns.txt")) {
		pattern := strings.SplitN(line, "\t", 2)[1]
		rules = append(rules, regexp.MustCompile("(?s)"+latin1ToUtf8([]byte(pattern))))
	}

	payloadPath := filepath.Join(dir, "payloads.txt")
	payloads := readLines(payloadPath)
	out, err := os.Create(payloadPath)
	if err != nil {
		panic(err)
	}
	defer out.Close()
	w := bufio.NewWriter(out)
	defer w.Flush()

	hits := 0
	for _, line := range payloads {
		encoded := strings.SplitN(line, "\t", 2)[0]
		payload, err := hex.DecodeString(encoded)
		if err != nil {
			panic(err)
		}
		text := latin1ToUtf8(payload)
		var matched []string
		for i, rule := range rules {
			if rule.MatchString(text) {
				matched = append(matched, strconv.Itoa(i))
			}
		}
		hits += len(matched)
		fmt.Fprintf(w, "%s\t%s\n", encoded, strings.Join(matched, ","))
	}
	fmt.Fprintf(os.Stderr, "%d payloads, %d hits\n", len(payloads), hits)
}
