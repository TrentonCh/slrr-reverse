"""The command line of the package (cli.py):

    python -I slrrc ...         the package directory run as a script
    python -m slrrc ...         the package run as a module

Both are started from the directory that holds the package.  "python -I -m
slrrc" does not work from there: isolated mode keeps the current directory
off the module path, so the package is not found.  Run as a directory the
interpreter puts the package directory itself on the path, where the modules
of the package would be importable as top-level modules; that entry is
replaced by the directory above it before anything is imported.
"""
import os
import sys

if __package__:
    from .cli import main
else:
    _here = os.path.dirname(os.path.abspath(__file__))
    sys.path[:] = [p for p in sys.path
                   if os.path.normcase(os.path.abspath(p or os.curdir)) != os.path.normcase(_here)]
    sys.path.insert(0, os.path.dirname(_here))
    from slrrc.cli import main

sys.exit(main())
