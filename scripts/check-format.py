#!/usr/bin/env python3
"""Check changed C++ lines; manual runs check the most recent commit.

A missing push base or a changed formatting configuration checks every C++ file.
The working tree and Git index are never modified.
"""

import argparse
import difflib
import os
from pathlib import Path
import re
import subprocess
import sys


EXTENSIONS = {".c", ".h", ".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx",
              ".ipp", ".inl", ".cppm", ".ixx"}
CONFIG_NAMES = {".clang-format", "_clang-format", ".clang-format-ignore"}
HUNK = re.compile(rb"^@@ -[0-9,]+ \+(\d+)(?:,(\d+))? @@", re.MULTILINE)


def git(*args):
    return subprocess.run(
        ["git", *args], check=True, stdout=subprocess.PIPE
    ).stdout


def resolve_commit(revision):
    result = subprocess.run(
        ["git", "rev-parse", "--verify", "--end-of-options",
         revision + "^{commit}"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    return result.stdout.decode("ascii").strip() if result.returncode == 0 else None


def paths(data):
    return [os.fsdecode(path) for path in data.split(b"\0") if path]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default=os.environ.get("FORMAT_BASE", ""))
    parser.add_argument("--head", default=os.environ.get("FORMAT_HEAD") or "HEAD")
    parser.add_argument("--event", default=os.environ.get("FORMAT_EVENT_NAME", "workflow_dispatch"))
    parser.add_argument("--binary", default="clang-format")
    args = parser.parse_args()

    os.chdir(os.fsdecode(git("rev-parse", "--show-toplevel")).strip())
    head = resolve_commit(args.head)
    if not head or head != resolve_commit("HEAD"):
        parser.error("--head must be the checked-out commit")

    base = resolve_commit(args.base) if args.base else None
    if args.event == "pull_request":
        if not base:
            parser.error("the pull request base commit is unavailable")
        base = git("merge-base", base, head).decode("ascii").strip()
    elif args.event == "workflow_dispatch" and not args.base:
        base = resolve_commit(head + "^")

    changed = paths(git("diff", "--name-only", "-z", "--no-renames",
                        base, head, "--")) if base else []
    full = not base or any(Path(path).name in CONFIG_NAMES for path in changed)
    if full:
        print("Checking all C++ files: no comparison base or formatting configuration changed.")
        candidates = paths(git("ls-tree", "-r", "--name-only", "-z", head))
    else:
        print(f"Checking changed C++ lines: {base}..{head}")
        candidates = paths(git("diff", "--name-only", "-z", "--no-renames",
                               "--diff-filter=ACM", base, head, "--"))
    candidates = [path for path in candidates if Path(path).suffix.lower() in EXTENSIONS]
    if not candidates:
        print("No changed C++ files to check.")
        return 0

    version = subprocess.run(
        [args.binary, "--version"], check=True, stdout=subprocess.PIPE
    ).stdout.decode("utf-8", errors="replace")
    if not re.search(r"\b22\.1\.8\b", version):
        parser.error("clang-format 22.1.8 is required")

    failures = 0
    for path in candidates:
        original = git("show", f"{head}:{path}")
        if not original:
            continue
        command = [args.binary, "--style=file", "--assume-filename=" + str(Path(path).absolute())]
        if not full:
            patch = git("diff", "--no-ext-diff", "--no-textconv", "--no-renames",
                        "--unified=0", base, head, "--", path)
            ranges = [(int(start), int(count or b"1"))
                      for start, count in HUNK.findall(patch)
                      if count != b"0"]
            if not ranges:
                continue
            for start, count in ranges:
                first = max(1, start)
                last = first + count - 1
                command.append(f"--lines={first}:{last}")
        formatted = subprocess.run(
            command, input=original, check=True, stdout=subprocess.PIPE
        ).stdout
        if original != formatted:
            failures += 1
            print(f"Formatting differs: {path}")
            sys.stdout.writelines(difflib.unified_diff(
                original.decode("utf-8", errors="replace").splitlines(keepends=True),
                formatted.decode("utf-8", errors="replace").splitlines(keepends=True),
                fromfile=path, tofile=path + " (clang-format)",
            ))
    print(f"Formatting check: {failures} file(s) need changes.")
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Formatting check failed: {error}", file=sys.stderr)
        sys.exit(2)
