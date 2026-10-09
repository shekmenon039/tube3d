# Contributing to Tube3D

Thanks for helping! Bug reports, fixes and features are all welcome.

## Reporting a bug

Open an issue with:

- your console and system version,
- the Tube3D version (Settings → More → Version),
- what you did, what you expected and what happened,
- for a video problem: the YouTube link or the file's format (container, resolution, frame rate,
  audio codec - MediaInfo shows them), and whether it's YouTube, the SD card, SMB or SFTP,
- a photo of the screen if something looks wrong.

Please don't post passwords, server addresses you want to keep private, or the contents of
`servers.txt`.

## Building

See [Build](README.md#build) in the README. `make -C client` gives a `.3dsx`,
`make -C client release` a `.cia`.

## Pull requests

- **Keep one change per pull request** and say how you tested it.
- **Test on a 3DS.** Emulators can't decode video (the MVD hardware decoder is not
  emulated), so anything touching playback, the decoder, the GPU or timing must be tried on a
  console. Say which videos/files you played and how it went (smooth? sound in sync? seeking?
  HOME and back?).
- **Run the checks** before pushing - they must all print "clean" / "ok" / "All checks passed" /
  "Success":

  ```bash
  pip install --user clang-format==21.1.8 clang-tidy==22.1.8 ruff==0.16.10 mypy==2.4.0
  sh tools/format.sh fix                    # format the C/C++ (.clang-format)
  make -C client prepare                    # libraries and generated headers, for the lint
  sh tools/lint.sh                          # gcc warnings, clang-tidy, clang-format, ruff, mypy, secrets
  make -C client WERROR=1                   # no compiler warnings allowed in Tube3D's code
  sh tools/check_commits.sh origin/main..   # commit messages
  ```

  The CI runs the same checks, before it builds, on every push and pull request.
- **Commit messages follow [Conventional Commits](https://www.conventionalcommits.org)**:
  `type(scope): summary`, with type one of feat, fix, docs, style, refactor, perf, test, build,
  ci, chore or revert, e.g. `fix(player): keep audio in sync after a seek`.
- **Match the code around you**: C (gnu11), formatted by clang-format, short comments that say *why*, no
  allocations per packet or per frame in the player (fixed buffers; RAM is tight), no blocking network calls on the main thread (they freeze the HOME
  button).
- **UI changes**: add before/after photos or screenshots of every screen you touched, both
  screens. Keep wording plain ("Play", not "Play 3D") and text sharp (draw through `uifont`).
- **No secrets** in commits: an optional YouTube Data API key goes in `client/secrets.mk`
  (git-ignored; see `client/secrets.mk.example`). The lint fails on a Google API key in a
  tracked file.
- By contributing you agree that your contribution is licensed under the GPL-3.0, like the rest
  of Tube3D.

## Releases

Bump `VERSION` in `client/Makefile`, commit, and push a tag `vX.Y.Z`: the CI builds it and publishes a GitHub Release with `Tube3D.cia` and `Tube3D.3dsx`. Settings → More shows the version and the commit, e.g. `1.0.0 (e206264)`.
