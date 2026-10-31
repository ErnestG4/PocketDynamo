# Copyright 2026 Combust.
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# PlatformIO pre-script: bakes the commit the firmware was built from into
# PD_GIT ("abc1234", "-dirty" when the tree has uncommitted changes), so a
# binary always says exactly which source it came from.
import subprocess
Import("env")


def rev():
    try:
        h = subprocess.check_output(["git", "rev-parse", "--short=7", "HEAD"], cwd=env["PROJECT_DIR"]).decode().strip()
        dirty = subprocess.call(["git", "diff", "--quiet", "HEAD"], cwd=env["PROJECT_DIR"]) != 0
        return h + ("-dirty" if dirty else "")
    except Exception:
        return "unknown"


env.Append(CPPDEFINES=[("PD_GIT", env.StringifyMacro(rev()))])
