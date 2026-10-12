"""Exercise version selection against real, isolated Git repositories."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools/build_version.py"
spec = importlib.util.spec_from_file_location("build_version", SCRIPT)
build_version = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_version)


class VersionTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.repo = Path(self.directory.name)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.name", "Version test")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.git("config", "tag.gpgsign", "false")
        (self.repo / "source").write_text("first\n")
        self.commit()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], text=True).strip()

    def commit(self):
        self.git("add", "source")
        self.git("commit", "-qm", "test")

    def test_branch_and_new_commit(self):
        self.assertEqual(build_version.version(self.repo), "main-" + self.git("rev-parse", "--short=7", "HEAD"))
        old = build_version.version(self.repo)
        (self.repo / "source").write_text("second\n")
        self.commit()
        self.assertNotEqual(build_version.version(self.repo), old)

    def test_exact_tag_and_descendant(self):
        self.git("tag", "-a", "v1.2.3-rc.1+build.2", "-m", "release")
        self.assertEqual(build_version.version(self.repo), "v1.2.3-rc.1+build.2")
        (self.repo / "source").write_text("next\n")
        self.commit()
        self.assertTrue(build_version.version(self.repo).startswith("main-"))

    def test_dirty_tag_and_untracked_files(self):
        self.git("tag", "v1.2.3")
        (self.repo / "source").write_text("changed\n")
        self.assertRegex(build_version.version(self.repo), r"^main-[0-9a-f]+-dirty$")
        self.git("checkout", "--", "source")
        (self.repo / "untracked").touch()
        self.assertTrue(build_version.version(self.repo).endswith("-dirty"))

    def test_detached_release_and_missing_branch(self):
        self.git("tag", "v1.2.3")
        self.git("checkout", "-q", "--detach", "v1.2.3")
        self.assertEqual(build_version.version(self.repo), "v1.2.3")
        self.git("tag", "-d", "v1.2.3")
        with self.assertRaisesRegex(ValueError, "branch"):
            build_version.version(self.repo)

    def test_semver_and_ambiguity(self):
        for tag in ["v01.2.3", "v1.2", "v1.2.3-01", "release", "1.2.3"]:
            self.git("tag", tag)
        self.assertTrue(build_version.version(self.repo).startswith("main-"))
        self.git("tag", "v1.2.3")
        self.git("tag", "v1.2.4")
        with self.assertRaisesRegex(ValueError, "several"):
            build_version.version(self.repo)

    def test_header_is_safe_and_not_rewritten_when_unchanged(self):
        self.git("checkout", "-qb", 'feature/<long-branch-name-with-"quotes">')
        with tempfile.TemporaryDirectory() as output:
            header = Path(output) / "build_version.h"
            command = ["python3", str(SCRIPT), "--repo", str(self.repo), "--header", str(header)]
            subprocess.check_output(command)
            stamp = header.stat().st_mtime_ns
            subprocess.check_output(command)
            self.assertEqual(stamp, header.stat().st_mtime_ns)
            source = Path(output) / "version.cpp"
            source.write_text('#include "build_version.h"\n#include <cstdio>\n'
                              'int main() { std::puts(KNIFE_BUILD_VERSION); }\n')
            exe = Path(output) / "version"
            subprocess.check_call(["g++", str(source), "-o", str(exe)])
            self.assertEqual(subprocess.check_output([str(exe)], text=True).strip(), build_version.version(self.repo))
            self.assertLessEqual(len(build_version.idf_version(build_version.version(self.repo)).encode()), 31)


if __name__ == "__main__":
    unittest.main()
