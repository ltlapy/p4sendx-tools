"""Tests empty their work folder first. This keeps a wrong argument from emptying anything else."""
import atexit
import sys
from pathlib import Path

MARK = ".p4test-work"


def _mark(work):
    if work.is_dir():
        (work / MARK).touch()


def guard(work):
    """Stops unless the folder is new, empty, or one a test has used before."""
    work = Path(work)
    if work.exists() and any(work.iterdir()) and not (work / MARK).exists():
        sys.exit(f"{work}: not a test's work folder, so it is left alone. Check the arguments.")
    atexit.register(_mark, work)
