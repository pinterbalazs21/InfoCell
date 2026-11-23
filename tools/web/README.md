# InfoCell Viewer

```
python tools/web/server.py
```

Then open http://127.0.0.1:8765. Standard library only; it listens on 127.0.0.1.

The page shows what this build of `infocell-cli` can do (it reads the subcommands from `--help`):

| Page | Needs |
|---|---|
| **Tasks**: training and test grids, changed pixels, object cells; hover a pixel for its Pixel, Object and Grid cell, click to pin | nothing |
| **ToolFinder on this task**: one tool per grid slot (width, height, colors, objects, …) that fits every training pair | `toolfinder` |
| **ToolFinder**: free number examples | `toolfinder` |
| **Cell types**: the compiled types, their slots, supertypes, methods and users | `types` |
| **Concept**: the ideas behind info cells, searching and the layers (`concept.html`, also opens on its own) | nothing |
| Solving, learned programs, partial programs, search settings | `explain` |
| Solver vocabulary on the Cell types page | `vocabulary` |

Results are cached in `build/web-cache/` and recomputed when `infocell-cli` is rebuilt.

Options: `--arc1`, `--arc2` (data folders with `training/` and `evaluation/`; default `../ARC-AGI/data` and `../ARC-AGI-2/data`), `--exe`, `--port`, `--timeout`.
