<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Testing and reporting

[Usage](USAGE.md) · [development](DEVELOPMENT.md) · [contributing](../CONTRIBUTING.md) · [edenprobe](EDENPROBE.md).
How to test a build, and the form a report takes so that whoever reads it can compare it with the last one without
asking. Nothing of a game goes into a report: no keys, dumps, game files, or shader and texture dumps
([CONTRIBUTING](../CONTRIBUTING.md#game-data-stays-out)).

## Index

* [What to test](#what-to-test)
* [Which build](#which-build)
* [A game that does not run as it should](#a-game-that-does-not-run-as-it-should)
* [A test of a pull request or a fix](#a-test-of-a-pull-request-or-a-fix)
* [Naming your machine](#naming-your-machine)
* [What happens to a report](#what-happens-to-a-report)

## What to test

A report is worth most when it says what ran, for how long and how it ended, so say that and nothing more:

- The scene, in words someone can reach: "New Game, first cutscene, then the first fight" and not "it breaks sometimes".
- The input, if a script drove it (`--script=`, [Run / diagnose](DEVELOPMENT.md#run--diagnose)), or what was pressed.
- The result: ran to the end of the scene, crashed, froze, looked wrong or sounded wrong, and the time it took.
- How many times. One passing run shows nothing about a crash that comes one run in five, and one failing run is
  worth the failure's log. `tools/run_game.py <id> --runs 5 --seconds 60` repeats a scene and summarizes each run.

## Which build

Say exactly which build ran, since two nightlies a day apart can behave differently:

- A nightly: its tag, `nightly-<date>-<commit>`, from the release the file came from.
- A pull request's build: `pr-<number>` and the commit it was made from, which its release notes name. The build is
  replaced at the pull request's next push.
- A build from source: `git rev-parse --short HEAD`, and whether the tree was clean.
- The game's log starts with a `build:` line that says the same, so a report that carries the log carries the build.

A game's code is recompiled only when the recompiler changes ([CLAUDE.md](../CLAUDE.md)), so a `games/` folder made by an
older build can still hold old code. Say so when you tested with one, as a result can come from the old module and not
from the new runtime.

## A game that does not run as it should

Use the [Game report](https://github.com/scarletnx/ScarletNX/issues/new?template=game-report.yml) form. It asks for:

| Field | What goes in it |
| --- | --- |
| Game | Its name, and its version or update if known (the log has the title ID and version once the game is read) |
| What happens | The closest of: does not start, crashes, freezes, looks wrong, sounds wrong, controls, slow |
| What you did and what you saw | Where in the game, what was pressed before, and what you expected instead |
| Log | The game's `.log`, or the launcher's *Share log* file. A log over 25 MB can be zipped first |
| Save | Optional. The zipped save folder, which lets a problem deep into the game be reached directly |
| Screenshots or video | Optional |

The log carries the build, the operating system, the processor, the RAM and the graphics card with its driver, so the
form does not ask for them. A report that cannot attach the log, such as one from a phone that cannot share it,
names them instead, as [Naming your machine](#naming-your-machine) says.

The title of the issue is `[game] <name>` followed by the symptom in a few words, as in
`[game] Some Game: black screen after the logo`.

## A test of a pull request or a fix

A comment or a pull request's `Testing:` section that reports a test uses this shape:

```
Tested <commit> on <OS and version>, <processor>, <graphics card> (driver <version>), <RAM>.

Build: <nightly tag, or pr-<number> and commit, or built from source>
Game: <name>, version <version>, title ID <id>
Scene: <where, and what was pressed>
Runs: <number> of <number> ended as expected
Before: <how the same scene ended without the change, with the build that shows it>
Result: <what happened, with the lines of the log that decide it>
Not tested: <what the change touches that this run did not reach>
```

- `Before` is what makes a fix visible. For a crash, the failing build and its first decisive log line. For a visual
  fix, the same frame without and with the change.
- `Result` quotes the decisive lines of the log inline (the `build:`, `vulkan:` or crash lines, `FATAL`) and attaches
  the whole log, or puts it in a `<details>` block when it is long.
- `Not tested` is part of the report. A change checked on one game and one GPU says so.
- A fix for an intermittent crash needs enough runs to show it is gone: the number of runs that used to fail one in
  five and now do not.

## Naming your machine

When no log is attached, open the report with one line naming the machine. It is read by people who have a different
GPU, and the same build behaves differently on a different driver:

```
Windows 10 (10.0.19044), Xeon E5-2640 v3 (8 cores, 16 threads), GeForce RTX 3070 (driver 596.49), 16 GB RAM
```

- The operating system with its version, and the processor with its core and thread count.
- The graphics card with the driver version. On Linux, `vulkaninfo --summary` names the Vulkan device and its
  driver, and `lscpu` the processor. On Windows, *Settings > System > About* names the processor and RAM, and the
  graphics driver is in the vendor's control panel or in `vulkaninfo`. On Android, the model and the Android version.
- A software renderer (lavapipe, llvmpipe) is named as such. It is a different result from a GPU.

Do not add the machine's name, user name, or home folder. The log's own text already leaves them out.

## What happens to a report

A game report with a log attached gets one comment from the workflow within a few minutes, edited as logs are added.
It names the build, the game and its version, the GPU, how the run ended, the game function it is pinned on, the
service commands the runtime does not implement, the frames over 50 ms, and the other reports that failed the same
way. It labels the issue `game report` and `log: <how it ended>`. `build/scarletnx --scan-log LOG` gives the same
summary on your machine ([Run / diagnose](DEVELOPMENT.md#run--diagnose)).

A maintainer who reproduces the failure labels it `status: confirmed`, which posts a comment that the report is ready
to become an [edenprobe](EDENPROBE.md) case, so that the next build is held to the console's answer. A fix refers to the report.
