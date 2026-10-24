#!/usr/bin/env python3
"""Exercise run_tidy.sh's public CLI without requiring LLVM or a project build."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


TIDY_SCRIPT = Path(__file__).resolve().parents[1] / "run_tidy.sh"


class RunTidyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="fiber-tidy-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.bin = self.root / "bin"
        self.bin.mkdir()
        # A closed PATH keeps installed LLVM versions/runners from influencing
        # the fixture. Only ordinary shell utilities and the recording stub run.
        for command in (
            "bash", "git", "nproc", "grep", "head", "tail", "cut", "sed", "awk",
            "basename", "dirname", "realpath", "find", "xargs", "tee",
            "sort", "uniq", "tr",
        ):
            executable = shutil.which(command)
            self.assertIsNotNone(executable, command)
            (self.bin / command).symlink_to(executable)
        self.env = dict(os.environ, PATH=str(self.bin), GIT_CONFIG_NOSYSTEM="1")
        for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "CLANG_TIDY"):
            self.env.pop(key, None)
        self.calls = self.root / "calls.jsonl"
        self.env["TIDY_TEST_CALLS"] = str(self.calls)
        # An LLVM tree off PATH. The database's compiler lives here, so tools
        # looked up beside it stay inside the fixture.
        self.toolchain = self.root / "toolchain"
        self.toolchain.mkdir()
        self.stub = self.make_tidy("clang-tidy-20", "20.1.8")
        shutil.copyfile(TIDY_SCRIPT, self.repo / "run_tidy.sh")
        for directory in ("src", "include", "apps", "tests", "example", "fuzz", "build"):
            (self.repo / directory).mkdir()
        self.write(".gitignore", "build/\n")
        self.write("include/Direct.h", "// direct\n")
        self.write("include/Leaf.h", "// indirect\n")
        self.write("include/Wrapper.h", '#include "Leaf.h"\n')
        self.write("src/direct.cpp", '#include "Direct.h"\n')
        self.write("src/indirect.cpp", '#include "Wrapper.h"\n')
        self.write("src/unrelated.cpp", "int value;\n")
        self.write("src/generated/ignored.cpp", "int generated;\n")
        self.write("src/vendor/ignored.cpp", "int vendor;\n")
        self.write("fuzz/disabled.cpp", "int fuzz;\n")
        self.git("init", "-q")
        self.git("add", ".")
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "-qm", "fixture")
        self.project_tus = {"src/direct.cpp", "src/indirect.cpp", "src/unrelated.cpp"}
        # Excluded files are deliberately in the database; the fuzz TU is not.
        self.db_tus = sorted(self.project_tus | {"src/generated/ignored.cpp", "src/vendor/ignored.cpp"})
        # Never created: the major comes from the name, as for a removed compiler.
        self.write_database(self.toolchain / "clang++-20")

    def write_database(self, compiler):
        database = [
            {"directory": str(self.repo), "file": str(self.repo / name),
             "command": f"{compiler} -c " + str(self.repo / name)}
            for name in self.db_tus
        ]
        self.write("build/compile_commands.json", json.dumps(database, indent=2))

    def make_stub(self, path, version_line):
        """An executable that answers --version and logs any other call."""
        path.write_text(
            f"#!{sys.executable}\n"
            "import json, os, sys\n"
            "if '--version' in sys.argv:\n"
            f"    print({version_line!r})\n"
            "else:\n"
            "    with open(os.environ['TIDY_TEST_CALLS'], 'a') as out:\n"
            "        out.write(json.dumps(sys.argv) + '\\n')\n"
        )
        path.chmod(0o755)
        return path

    def make_tidy(self, name, version, directory=None):
        return self.make_stub((directory or self.bin) / name, "LLVM version " + version)

    def write(self, path, text):
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)

    def git(self, *args):
        subprocess.run(["git", *args], cwd=self.repo, env=self.env,
                       check=True, capture_output=True)

    def run_tidy(self, *args, cwd=None, expected_rc=0):
        """Runs the script and returns the TUs it checked.

        Also records the clang-tidy binaries that ran (self.binaries), runner
        invocations (self.runner_calls) and stderr (self.stderr).
        """
        if self.calls.exists():
            self.calls.unlink()
        result = subprocess.run(
            ["bash", str(self.repo / "run_tidy.sh"), "-j", "1", *args],
            cwd=cwd or self.repo, env=self.env, text=True, capture_output=True,
        )
        self.assertEqual(result.returncode, expected_rc, result.stdout + result.stderr)
        self.assertNotIn("command not found", result.stderr)
        self.stderr = result.stderr
        invocations = []
        if self.calls.exists():
            invocations = [json.loads(line) for line in self.calls.read_text().splitlines()]
        selected = []
        self.binaries = set()
        self.runner_calls = []
        for binary, *invocation in invocations:
            if Path(binary).name.startswith("run-clang-tidy"):
                self.runner_calls.append([binary, *invocation])
                self.assertEqual(invocation[invocation.index("-p") + 1], str(self.repo / "build"))
                files = invocation[invocation.index("--") + 1:]
            else:
                self.binaries.add(binary)
                self.assertIn("-p=" + str(self.repo / "build"), invocation)
                files = invocation[-1:]
            selected.extend(str(Path(name).relative_to(self.repo)) for name in files)
        self.assertEqual(len(selected), len(set(selected)), "duplicate TU invocation")
        return set(selected)

    def test_cpp_only_stays_scoped(self):
        self.write("src/direct.cpp", '#include "Direct.h"\nint changed;\n')
        self.assertEqual(self.run_tidy(), {"src/direct.cpp"})

    def test_direct_header_selects_consumers(self):
        self.write("include/Direct.h", "// changed\n")
        self.assertEqual(self.run_tidy(), self.project_tus)

    def test_transitive_header_selects_consumers(self):
        self.write("include/Leaf.h", "// changed\n")
        self.assertEqual(self.run_tidy(), self.project_tus)

    def test_header_and_unrelated_cpp_do_not_lose_consumers(self):
        self.write("include/Direct.h", "// changed\n")
        self.write("src/unrelated.cpp", "int changed;\n")
        self.assertEqual(self.run_tidy(), self.project_tus)

    def test_explicit_header(self):
        self.assertEqual(self.run_tidy("include/Leaf.h"), self.project_tus)

    def test_header_only_directory(self):
        self.assertEqual(self.run_tidy("include"), self.project_tus)

    def test_untracked_header(self):
        self.write("include/New.h", "// new\n")
        self.assertEqual(self.run_tidy(), self.project_tus)

    def test_deleted_header(self):
        (self.repo / "include/Leaf.h").unlink()
        self.assertEqual(self.run_tidy(), self.project_tus)

    def test_absolute_database_path(self):
        self.assertEqual(self.run_tidy("-p", str(self.repo / "build"), "src/direct.cpp"),
                         {"src/direct.cpp"})

    def test_subdirectory_relative_paths(self):
        self.assertEqual(self.run_tidy("-p", "../build", "direct.cpp", cwd=self.repo / "src"),
                         {"src/direct.cpp"})

    def test_subdirectory_default_database_falls_back_to_repo(self):
        self.assertEqual(self.run_tidy("direct.cpp", cwd=self.repo / "src"), {"src/direct.cpp"})

    def test_explicit_paths_are_normalized_and_deduplicated(self):
        self.assertEqual(self.run_tidy("src/direct.cpp", "src/../src/direct.cpp"), {"src/direct.cpp"})

    def test_clean_checkout(self):
        self.assertEqual(self.run_tidy(), set())

    def test_all_excludes_generated_vendor_and_unconfigured_fuzz(self):
        self.assertEqual(self.run_tidy("-a"), self.project_tus)

    def test_explicit_files_replace_changes(self):
        self.write("include/Leaf.h", "// changed\n")
        self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})

    def test_missing_explicit_header_is_an_error(self):
        self.assertEqual(self.run_tidy("include/missing.h", expected_rc=2), set())

    def test_bare_version_20(self):
        self.stub.rename(self.bin / "clang-tidy")
        self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})
        self.assertEqual(self.binaries, {str(self.bin / "clang-tidy")})
        self.assertNotIn("baselined", self.stderr)

    def test_versions_below_the_floor_are_rejected(self):
        self.stub.unlink()
        for version in ("17.0.6", "18.1.8"):
            with self.subTest(version=version):
                bare = self.make_tidy("clang-tidy", version)
                self.make_tidy("clang-tidy-" + version.split(".")[0], version)
                self.assertEqual(self.run_tidy("src/direct.cpp", expected_rc=2), set())
                self.assertIn("need LLVM 19+", self.stderr)
                self.assertIn(f"rejected {bare}: version {version}", self.stderr)

    def test_newer_majors_run_with_a_baseline_note(self):
        self.stub.unlink()
        for version in ("19.1.7", "21.1.0", "22.1.6"):
            with self.subTest(version=version):
                tidy = self.make_tidy("clang-tidy", version)
                self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})
                self.assertEqual(self.binaries, {str(tidy)})
                self.assertIn("baselined on clang-tidy 20.x", self.stderr)
                self.assertIn("differs", self.stderr)

    def test_prefers_the_database_compilers_major(self):
        tidy22 = self.make_tidy("clang-tidy-22", "22.1.6")
        self.write_database(self.toolchain / "clang++-22")
        self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})
        self.assertEqual(self.binaries, {str(tidy22)})
        self.assertNotIn("differs", self.stderr)

        self.write_database(self.toolchain / "clang++-20")
        self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})
        self.assertEqual(self.binaries, {str(self.stub)})

    def test_reads_the_major_from_a_generic_cxx_driver(self):
        # CMAKE_CXX_COMPILER=c++ resolving to some clang: only its report says which.
        self.make_stub(self.toolchain / "c++", "clang version 22.1.6")
        tidy22 = self.make_tidy("clang-tidy-22", "22.1.6")
        self.write_database(self.toolchain / "c++")
        self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})
        self.assertEqual(self.binaries, {str(tidy22)})

    def test_finds_tools_beside_an_unversioned_compiler_off_path(self):
        # The compiler's own --version names the major; the matching
        # clang-tidy and its runner sit beside it, not on PATH.
        self.make_stub(self.toolchain / "clang++", "Ubuntu clang version 22.1.6")
        beside = self.make_tidy("clang-tidy", "22.1.6", directory=self.toolchain)
        runner = self.make_stub(self.toolchain / "run-clang-tidy", "runner")
        self.make_tidy("clang-tidy-21", "21.1.0")
        self.write_database(self.toolchain / "clang++")
        self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})
        self.assertEqual([call[0] for call in self.runner_calls], [str(runner)])
        call = self.runner_calls[0]
        self.assertEqual(call[call.index("-clang-tidy-binary") + 1], str(beside))

    def test_falls_back_to_the_baseline_then_the_newest_major(self):
        self.make_tidy("clang-tidy-22", "22.1.6")
        newest = self.make_tidy("clang-tidy-23", "23.0.1")
        self.write_database(self.toolchain / "clang++-21")  # no clang-tidy-21
        self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})
        self.assertEqual(self.binaries, {str(self.stub)})
        self.assertIn("differs", self.stderr)

        self.stub.unlink()
        self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})
        self.assertEqual(self.binaries, {str(newest)})

    def test_clang_tidy_environment_overrides_detection(self):
        chosen = self.make_tidy("custom-tidy", "21.1.0", directory=self.toolchain)
        self.env["CLANG_TIDY"] = str(chosen)
        self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})
        self.assertEqual(self.binaries, {str(chosen)})

        # A relative path is the caller's, though the script runs from the repo root.
        self.env["CLANG_TIDY"] = "../../toolchain/custom-tidy"
        self.assertEqual(self.run_tidy("direct.cpp", cwd=self.repo / "src"), {"src/direct.cpp"})
        self.assertEqual({Path(binary).resolve() for binary in self.binaries}, {chosen.resolve()})

        for unusable in (self.toolchain / "missing",
                         self.make_tidy("old-tidy", "18.1.8", directory=self.toolchain)):
            with self.subTest(unusable=unusable.name):
                self.env["CLANG_TIDY"] = str(unusable)
                self.assertEqual(self.run_tidy("src/direct.cpp", expected_rc=2), set())
                self.assertIn(f"CLANG_TIDY={unusable}", self.stderr)

    def test_runner_must_come_from_the_chosen_release(self):
        self.make_stub(self.bin / "run-clang-tidy-22", "runner")
        self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})
        self.assertEqual(self.runner_calls, [])
        self.assertEqual(self.binaries, {str(self.stub)})

        runner = self.make_stub(self.bin / "run-clang-tidy-20", "runner")
        self.assertEqual(self.run_tidy("src/direct.cpp"), {"src/direct.cpp"})
        self.assertEqual([call[0] for call in self.runner_calls], [str(runner)])
        call = self.runner_calls[0]
        self.assertEqual(call[call.index("-clang-tidy-binary") + 1], str(self.stub))


if __name__ == "__main__":
    unittest.main()
