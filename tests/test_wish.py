import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

WISH = Path(__file__).resolve().parents[1] / "wish"
ERROR = "An error has occurred\n"


class WishTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="wish-tests-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def batch(self, commands, *, cwd=None):
        batch_file = self.root / "batch.txt"
        batch_file.write_text(commands, encoding="utf-8")
        return subprocess.run(
            [str(WISH), str(batch_file)],
            cwd=cwd or self.root,
            text=True,
            capture_output=True,
            timeout=15,
        )

    def script(self, name, text):
        directory = self.root / "programs"
        directory.mkdir(exist_ok=True)
        program = directory / name
        program.write_text("#!/bin/sh\n" + text, encoding="utf-8")
        program.chmod(0o755)
        return program

    def test_interactive_prompt(self):
        result = subprocess.run(
            [str(WISH)], input="echo hello\nexit\n", text=True,
            capture_output=True, timeout=5,
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "wish> hello\nwish> ")
        self.assertEqual(result.stderr, "")

    def test_interactive_eof(self):
        result = subprocess.run(
            [str(WISH)], input="", text=True,
            capture_output=True, timeout=5,
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "wish> ")

    def test_batch_no_prompt(self):
        result = self.batch("echo hello\necho world\n")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "hello\nworld\n")
        self.assertEqual(result.stderr, "")

    def test_blank_lines_and_tabs(self):
        result = self.batch("  \t\n\necho\t spaced \t words \n\n")
        self.assertEqual(result.stdout, "spaced words\n")
        self.assertEqual(result.stderr, "")

    def test_invalid_invocation(self):
        result = subprocess.run(
            [str(WISH), "one", "two"], text=True, capture_output=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, ERROR)

    def test_nonexistent_batch_file(self):
        result = subprocess.run(
            [str(WISH), str(self.root / "missing.txt")],
            text=True, capture_output=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, ERROR)

    def test_unknown_executable_continues(self):
        result = self.batch("totally_unknown_program_name\necho still_running\n")
        self.assertEqual(result.stdout, "still_running\n")
        self.assertEqual(result.stderr, ERROR)

    def test_exit_stops_batch(self):
        result = self.batch("exit\necho not_executed\n")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")

    def test_exit_arguments_invalid(self):
        result = self.batch("exit extra\necho yes\n")
        self.assertEqual(result.stdout, "yes\n")
        self.assertEqual(result.stderr, ERROR)

    def test_cd_updates_parent_working_directory(self):
        new_dir = self.root / "target"
        new_dir.mkdir()
        result = self.batch(f"cd {new_dir}\npwd\n")
        self.assertEqual(result.stdout.strip(), str(new_dir))
        self.assertEqual(result.stderr, "")

    def test_cd_argument_errors(self):
        result = self.batch("cd\ncd a b\ncd /this/path/does/not/exist\necho after\n")
        self.assertEqual(result.stdout, "after\n")
        self.assertEqual(result.stderr, ERROR * 3)

    def test_path_replacement_empty_and_restore(self):
        result = self.batch("path\necho forbidden\npath /bin\necho restored\n")
        self.assertEqual(result.stdout, "restored\n")
        self.assertEqual(result.stderr, ERROR)

    def test_custom_executable_path(self):
        self.script("hello_test", "printf 'custom executable\\n'\n")
        result = self.batch(f"path {self.root / 'programs'}\nhello_test\n")
        self.assertEqual(result.stdout, "custom executable\n")
        self.assertEqual(result.stderr, "")

    def test_missing_executable_after_path_replacement(self):
        self.script("hello_test", "printf 'custom executable\\n'\n")
        result = self.batch(f"path {self.root / 'programs'}\necho unavailable\n")
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, ERROR)

    def test_redirect_without_spaces_and_read_file(self):
        result = self.batch("echo first>out.txt\ncat out.txt\n")
        self.assertEqual(result.stdout, "first\n")
        self.assertEqual((self.root / "out.txt").read_text(), "first\n")
        self.assertEqual(result.stderr, "")

    def test_redirect_both_stdout_and_stderr(self):
        self.script("both", "printf 'OUT\\n'\nprintf 'ERR\\n' >&2\n")
        result = self.batch(f"path {self.root / 'programs'}\nboth>result.txt\n")
        self.assertEqual(result.stdout, "")
        self.assertEqual(result.stderr, "")
        self.assertEqual(set((self.root / "result.txt").read_text().splitlines()), {"OUT", "ERR"})

    def test_redirect_truncates_existing_file(self):
        (self.root / "out.txt").write_text("previous text\n")
        result = self.batch("echo replacement > out.txt\n")
        self.assertEqual(result.stderr, "")
        self.assertEqual((self.root / "out.txt").read_text(), "replacement\n")

    def test_redirect_syntax_errors(self):
        malformed = [
            "echo hello >",
            "> result.txt",
            "echo hello >> result.txt",
            "echo hello > result.txt extra",
            "echo hello > result.txt > other.txt",
            "echo hello>result.txt>other.txt",
        ]
        result = self.batch("\n".join(malformed) + "\necho success\n")
        self.assertEqual(result.stdout, "success\n")
        self.assertEqual(result.stderr, ERROR * len(malformed))
        self.assertFalse((self.root / "result.txt").exists())

    def test_redirect_to_unwritable_location(self):
        result = self.batch("echo hello > /nonexistent_wish_folder/out.txt\necho after\n")
        self.assertEqual(result.stdout, "after\n")
        self.assertEqual(result.stderr, ERROR)

    def test_parallel_without_spaces(self):
        result = self.batch("echo one&echo two\n")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(set(result.stdout.splitlines()), {"one", "two"})
        self.assertEqual(result.stderr, "")

    def test_parallel_actual_concurrency(self):
        start = time.monotonic()
        result = self.batch("sleep 1 & sleep 1\n")
        elapsed = time.monotonic() - start
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stderr, "")
        self.assertLess(elapsed, 1.8, f"Two 1-second commands ran in {elapsed:.2f}s")

    def test_parallel_with_independent_redirects(self):
        result = self.batch("echo one>first.txt&echo two>second.txt\n")
        self.assertEqual(result.stderr, "")
        self.assertEqual((self.root / "first.txt").read_text(), "one\n")
        self.assertEqual((self.root / "second.txt").read_text(), "two\n")

    def test_bad_parallel_separator_and_recovery(self):
        result = self.batch("&echo alpha&&echo beta\necho last&\n")
        self.assertEqual(result.stderr, ERROR * 2)
        self.assertEqual(set(result.stdout.splitlines()), {"alpha", "beta", "last"})

    def test_long_input(self):
        word = "x" * 12000
        result = self.batch(f"echo {word}\n")
        self.assertEqual(result.stdout, word + "\n")
        self.assertEqual(result.stderr, "")

    def test_builtin_redirect_reports_error(self):
        result = self.batch("path /bin > output.txt\necho works\n")
        self.assertEqual(result.stdout, "works\n")
        self.assertEqual(result.stderr, ERROR)
        self.assertFalse((self.root / "output.txt").exists())


if __name__ == "__main__":
    unittest.main()
