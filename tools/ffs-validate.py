#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
#
# Validate an FFS image with amitools, an FFS implementation written apart from
# Pkg: boot block, root, directory tree, every file's block chain, and the
# bitmap against the blocks actually in use. Prints each finding, exits 1 on
# any warning or error, 0 when the image is clean.
#
#   python3 tools/ffs-validate.py <image>     (with amitools importable)

import sys
from amitools.fs.blkdev.BlkDevFactory import BlkDevFactory
from amitools.fs.validate.Validator import Validator
from amitools.fs.validate.Log import Log

path = sys.argv[1]
blkdev = BlkDevFactory().open(path, read_only=True)
v = Validator(blkdev, Log.WARN)
ok = v.scan_boot()[0] and v.scan_root()
if ok:
    # These three report through the log, not through a return value.
    v.scan_dir_tree()
    v.scan_files()
    v.scan_bitmap()
v.log.dump()
warnings, errors = v.get_summary()
print("ffs-validate: %s, %d warnings, %d errors%s"
      % (path, warnings, errors, "" if ok else ", no readable boot and root block"))
blkdev.close()
sys.exit(0 if ok and warnings == 0 and errors == 0 else 1)
