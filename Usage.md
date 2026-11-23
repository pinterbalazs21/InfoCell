# Build and usage

## Requirements

- CMake 3.27+, Ninja, a C++20 compiler (tested: Visual Studio 2026, GCC 15)
- Conan 2, for example in a virtual environment: `python -m venv .venv`, activate it, `pip install conan`, then once `conan profile detect`

## Build

Windows (in a "x64 Native Tools" prompt):

```
conan install . --output-folder=conan/Release --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset WinRelease
cmake --build build/Release
```

Linux:

```
conan install . --output-folder=conan/Release --build=missing -s build_type=Release -s compiler.cppstd=20
cmake --preset LinuxRelease
cmake --build build/Release -j
```

Debug: replace `Release` with `Debug` everywhere, and use the `WinDebug` / `LinuxDebug` preset.

All executables end up in `build/Release/bin/`.

## Data

Expected next to the repository:

| Folder | Content | Source |
|---|---|---|
| `../ARC-AGI/data/` | ARC-AGI-1: `training/` and `evaluation/`, one `<task-id>.json` each | `git clone https://github.com/fchollet/ARC-AGI.git` |
| `../arc-prize-2026-arc-agi-2/` | ARC-AGI-2: `arc-agi_training_challenges.json`, `arc-agi_training_solutions.json`, `arc-agi_evaluation_challenges.json`, `arc-agi_evaluation_solutions.json`, `arc-agi_test_challenges.json` | Kaggle data of the ARC Prize 2026 ARC-AGI-2 competition, linked from https://arcprize.org/competitions/2026/arc-agi-2 |
| `../arc-prize-2024/` | ARC Prize 2024 files, used by some tests | https://www.kaggle.com/competitions/arc-prize-2024/data |

Other locations: `-f` / `-df` on the command line, or the `INFOCELL_*` variables in `CMakePresets.json`.

## Usage

Pick a task, then compare the edges of two of its grids:

```
infocell-cli arc-agi-1 -d -t -i 007bbfb7 compare-edges input -pk training -pi 0 -ik input output -pk training -pi 0 -ik output
```

- `arc-agi-1`: `-d` default data folder or `-f <folder>`; `-t` training or `-e` evaluation; `-i <task-id>`
- `arc-agi-2`: `-dd` or `-df <folder>`; `-ft` / `-fe` training or evaluation with solutions; `-i <task-id>`
- `compare-edges input … output …`: each grid is `-pk training|test`, `-pi <pair index>`, `-ik input|output`

ToolFinder on number examples, and the compiled cell types, as JSON:

```
infocell-cli toolfinder --sequence 1 2 4
infocell-cli toolfinder --pairs 3:9 5:15 --test 7
infocell-cli types
```

`infocell-cli --help` and `<command> --help` list everything.

`infocell-tui`: the terminal UI.

Viewer in the browser: `python tools/web/server.py`, then http://127.0.0.1:8765 (see `tools/web/README.md`).

## Solver (experimental)

```
infocell-cli solve -p ../ARC-AGI/data/training
infocell-cli explain ../ARC-AGI/data/training/007bbfb7.json --depth 4 --beam 2000
infocell-cli explain <task.json> --partial
infocell-cli grammar data/formal/pq.txt
infocell-cli vocabulary
```

- `solve`: learn a program per task from its training pairs and score the test outputs; `-i <task-id>`, `-v`, `-c` / `-l` concept prior
- `explain`: one task as JSON for the viewer; `--partial`: programs learned from single pairs
- `grammar`: axiom and generator schema from valid strings ([doc/Grammar.md](doc/Grammar.md))
- Change log and scores: [doc/SolverRules.md](doc/SolverRules.md)

## Tests

`cellstest`, `ToolFinderTest` and `arctest` in `build/Release/bin/`.
