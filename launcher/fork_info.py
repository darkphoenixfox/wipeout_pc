# SPDX-License-Identifier: GPL-2.0-or-later
"""This fork's version and releases (github.com/darkphoenixfox/wipeout_pc): the launcher shows
APP_VERSION and its update check looks for newer releases here (gow3_launcher_win.py). Upstream
does not have this file, so its version bumps merge without conflicts (docs/fork.md)."""

APP_VERSION = '0.3.1'
RELEASES_API = 'https://api.github.com/repos/darkphoenixfox/wipeout_pc/releases/latest'
RELEASES_PAGE = 'https://github.com/darkphoenixfox/wipeout_pc/releases/latest'
