# Symbolic solver – rules and changes log

Every change to the ARC solver (`infocell-cli solve`) is recorded here: what was added, why,
and what it did to the score.

## Protocol (no peeking)

- Development uses only the **training** sets: `ARC-AGI/data/training` (400 tasks) and
  `ARC-AGI-2/data/training` (1000 tasks).
- The **evaluation** sets are scored only as an aggregate number at milestones. No evaluation
  task is opened, and no per-task evaluation result is looked at.
- The solver itself never reads test outputs while solving. They are read only afterwards, to score.
- Two attempts per test input, as in the ARC rules.

## Baseline (before this log)

| Set | Solved |
|---|---|
| ARC-1 training | 133 / 400 (33%) |
| ARC-1 evaluation | 68 / 400 (17%) |
| ARC-2 training | 199 / 1000 (20%) |
| ARC-2 evaluation | 0 / 120 (0%) |

The baseline solver is about 100 hand-written families in `src/arc/ProgramSearch.cc`.

## The rule form

The aim is to replace hand-written families with one rule form whose instances are *learned*:

```
for each <entity> in parse(input) :  action( T[ feature(entity) ] )
```

- **parse**: how pixels are grouped into entities.
- **feature**: a generic property of the entity, chosen by the search.
- **action**: one entry of a small, fixed vocabulary.
- **T**: a table learned from the training pairs, mapping a feature value to an action.

A human-style description of a task is then one line, for example
`objects[single color] exists by color: 1 -> delete, 8 -> keep; color by scene: smallest object shape: ... -> recolor 2`.

## Change 1 – object rules, first version

**What.** The rule form at object level, with one action per object taken from a small list
(keep, delete, recolor, rotate/mirror, move, slide, ray, outline, fill holes, fill box).
An action was consistent for an object when every pixel it painted had that color in the
output. Per feature value, the actions of all objects sharing that value were intersected.

**Score.** ARC-1 training 133 (±0), ARC-2 training 205 (+6).

**Lesson.** One action per object is too coarse. Many tasks need two things at once
("move **and** recolor"), and the parameter often depends on another object. That led to change 2.

## Change 2 – the fate of an object: independent properties (`src/arc/ObjectRules.cc`)

**What.** Each object's fate is made of five properties. Each property gets its **own**
feature table, so "color depends on the shape, position depends on the size" is expressible.

| Property | Values |
|---|---|
| `exists` | keep, delete |
| `geometry` | the 8 symmetries of the square, applied inside the object's own box with the centre kept |
| `position` | `stay`, `shift(dx,dy)`, `slide d` (until blocked by another object or the border), `slide toward nearest object` |
| `color` | keep, `recolor c`, `color of nearest object` |
| `addition` | none, `ray d` (each pixel extends in direction d through background), `outline c`, `outline8 c`, `fill holes c`, `fill box c`, `connect to same color` (straight lines that end on another object of the same color) |

**Parses** (how pixels become objects; the background is the most frequent color):

- single color, 4-connected
- single color, 8-connected
- multi color, 8-connected
- by color: all pixels of one color form one object, connected or not

**Object features** (the table keys):

- color, size, is largest, is smallest, width, height, is rectangle, has hole, hole count
- touches border, touches other object
- same shape count, same color count, color count, is line, size rank, color rank
- is symmetric, shape (a hash of the mask, the same in every grid)
- **scene features**, the same for every object of one grid: shape of the smallest object,
  color of the smallest object, object count, color count. These let a rule refer to a "key"
  object elsewhere in the grid.

**How it learns.**

1. *Observe.* For every object of every training pair, enumerate the combinations of
   geometry × position × color whose painted pixels all match the output. Each property gets
   the set of values that occur in some matching combination. Additions are checked on their own.
   - A deleted object (some of its pixels became background, and no body fits) does not
     constrain the other properties.
   - Recoloring to the background color is not a value; that is `delete`.
   - A move must land at least half on free space, not on pixels that another object
     explains in place.
   - An addition must not paint the background color.
2. *Tables.* For every property and every feature subset of size 0, 1 or 2, intersect the
   value sets of all objects with the same key. A subset whose intersections are all
   non-empty is a candidate table, and each key takes its simplest value.
3. *MDL ranking of tables.* Candidate tables are ranked by:
   1. **residual**: for additions, how many new pixels the table explains, more is better;
   2. **evidence**: how many keys are seen in only one training pair, fewer is better;
      an entry seen once is memorized, not learned;
   3. the number of features;
   4. the number of entries.
4. *Combine and verify.* The best few tables per property are combined, ordered by their
   summed rank, up to 300 combinations. Each combination is rendered on an empty background
   canvas (additions first, then the object bodies). The first combination that reproduces
   **every** training output exactly is the rule.
5. *Predict.* A feature value never seen in training gives no answer. A deleted object
   skips its other properties.

**Why.** Your two examples are one-line instances of this form:
"background around a Y pixel becomes X" is `addition by color: Y -> outline8 X`, and
"every shape rotates 90° about its centre" is `geometry: rotate90`. Neither is hand-coded.

**Examples learned on training tasks** (printed by `infocell-cli solve -v`):

| Task | Rule |
|---|---|
| `009d5c81` | `exists by color: 1 -> delete, 8 -> keep; color by scene: smallest object shape: … -> recolor 2 / 3 / 7` — the key disappears, and its shape decides the color |
| `0a2355a6` | `color by hole count: 1 -> recolor 1, 2 -> recolor 3, 3 -> recolor 2, 4 -> recolor 4` |
| `00dbd492` | `addition by size: 16 -> fill holes 8, 24 -> fill holes 4, 32 -> fill holes 3` |
| `b2862040` | `color by has hole: 1 -> recolor 8` |
| `f0f8a26d` | `geometry: rotate90` |

**Supporting changes.**
- The perception helpers (background, objects, dihedral, crop, enclosed mask, rays) moved
  from `ProgramSearch.cc` to `src/arc/Perception.h`, so both modules share them.
- `Learner` got an optional `explain` hook, so the solver prints the learned rule, not only
  the family name.
- Diagnostics: `INFOCELL_OBJ_DEBUG=1` prints why the object learner failed and the mismatching
  pixels of its first candidate. `INFOCELL_OBJ_DEBUG2=1` prints every object's consistent
  values and the candidate tables.

**Score.** *(pending)*

**Score (v4: fate + scene features + MDL ranking).** ARC-1 training 134 / 400, ARC-2 training 212 / 1000.

## Change 3 – a feature grammar instead of a feature list

**Why.** The hand-written feature list (`is largest`, `has hole`, `scene: smallest object shape`, …)
was still me doing the compression: the search could only pick from my menu. Now only **atoms**
and **combinators** are defined. Every feature is a formula the search builds, and the solver
prints the formula it used.

**Atoms** (what perception measures on one object):
`color, size, width, height, left, top, right, bottom, holes, colors, touches border,
is rectangle, is symmetric, shape` (shape is a hash of the mask, the same in every grid).

**Relations** (from an object to another object; undefined when ambiguous):
`nearest` (smallest box gap), `largest`, `smallest` (unique in the scene), `touching` (the only
8-adjacent object), `container` (the smallest box that contains this box).

**Formulas:**

| Form | Example | Meaning |
|---|---|---|
| `B(self)` | `holes` | the object's own atom |
| `B(r)` | `size(smallest)`, `color(nearest)` | the atom of a related object |
| `B(self) == B(r)` | `color == color(touching)` | comparison with a related object |
| rank of `B` | `rank of size (largest first)` | 0 = the largest, 1 = second, …; `is largest` is not a feature any more |
| count | `objects with the same shape` | how many objects share the value |
| scene | `number of objects`, `number of colors` | the same for all objects of a grid |

There are 198 formulas.

**Description length.** A formula costs `log2(#forms) + log2(#atoms) [+ log2(#relations)]` bits.
A table costs its formulas plus 4 bits per entry plus 8 bits per entry seen in only one
training pair (memorization). Tables are ranked by residual first (additions), then bits.

**Observational equivalence.** For a task, two formulas that split all training objects into
the same groups are the same concept. Only the shorter one is kept, and it's usually a handful
of distinct concepts. Pairs of formulas are tried only among the 30 shortest distinct ones.

**Examples** (rules as printed):

| Task | Rule |
|---|---|
| `b2862040` | `color by holes: 0 -> keep, 1 -> recolor 8` |
| `0a2355a6` | `color by holes: 1 -> recolor 1, 2 -> recolor 3, 3 -> recolor 2, 4 -> recolor 4` |
| `009d5c81` | `exists by color: 1 -> delete, 8 -> keep; color by size(smallest): … -> recolor 7 / 2 / 3` |

## Change 4 – learned object selection (output = one object of the input)

**What.** For tasks whose output is smaller than the input:

```
output = symmetry_k( crop( the object whose <formula> is <v | the unique value | the maximum | the minimum> ) )
```

The formula comes from the same grammar. The crop is either everything inside the object's box
or only the object's own pixels. The selection must pick exactly one object in every training
pair, and the crop must equal the output exactly. The unique, maximum and minimum modes are
tried before a fixed value, because they don't memorize a constant.

Example of a printed rule: `crop the object[single color] with the unique shape, then mirrorH`.

**Score (v5: grammar + selection).** ARC-1 training 137 / 400 (+3), ARC-2 training 216 / 1000 (+4).
One task was lost (`1d61978c`). Its old rule was a 9-entry `by size` lookup table, which the
bit cost now correctly rejects as memorization.

## Change 5 – library learning, first step: a learned concept prior

**Why.** This is the emergent layer. The grammar defines what *can* be said; which concepts
are *worth* saying should be learned from experience, not set by me.

**What.**
- Rule descriptions mark every formula as `{formula}`, e.g. `color by {holes}: 1 -> recolor 8`.
- `infocell-cli solve --learn-concepts <file>` adds the formulas of every **solved training
  task** to a counts file (`count<TAB>formula`). This uses training tasks only.
- `infocell-cli solve --concepts <file>` loads the counts as a prior:

  ```
  cost(f) = -log2( (n_f + w * p_f) / (N + w) )
  ```

  - `n_f` is how often formula f explained a solved task, and `N` is the total.
  - `p_f = 2^-length(f)` is the grammar prior, normalized.
  - `w = 20` pseudo-observations, so unseen formulas are not forbidden, only more expensive.

  The formula order (used for deduplication and for which pairs are tried) follows the
  learned cost.

**Effect.** Formulas that keep explaining tasks become cheap. They are tried first, win ties,
and are preferred when two explanations fit. The learned vocabulary is visible: it is the
counts file, sorted.

**Validation protocol.** The prior is learned on ARC-1 training only and measured on the
ARC-2 training tasks that are *not* ARC-1 training tasks, a holdout the prior never saw.

**Result.** Learned on ARC-1 training, the prior contains only 8 concepts (e.g. `holes`,
`color`, `size`). Most solved tasks are still won by the older pixel-feature tables, whose
features are not marked as concepts yet. On the holdout (609 ARC-2 training tasks that are
not ARC-1 training tasks) it gives 83 → 84 solved; ARC-2 training overall 216 → 217. The
mechanism works, but the library is still too small to matter.

## Change 6 – corner rays, template stamps, broader delete, residual on new pixels only

| Addition | Meaning | Example |
|---|---|---|
| `corner ray d` | a line from the object's leading corner (the unique pixel furthest in direction d), through background | `5c0a986e`: `addition by {color}: 1 -> corner ray up-left, 2 -> corner ray down-right` |
| `stamp the most colorful / largest object, aligned by my color` | the template (the unique most colorful or largest object) is copied so that its pixel with the marker's color lands on the marker | markers that receive a copy of a pattern |
| `stamp …, centred on me` | the same, with the template centred on the marker | |

Two fixes came with these:

- **`delete` is offered whenever any pixel of the object changes color**, not only when it
  becomes background. A marker that gets painted over by a stamp is "deleted" while its
  addition stays.
- **The residual counts only new pixels.** A new pixel is background in the input and is
  painted by the addition. Before, a stamp painting over existing pixels looked like it
  explained more than a real hole fill (`00dbd492` regressed and is solved again).

**Score (v6).** ARC-1 training 140 / 400 (+3). ARC-2 training: *(pending)*.

## Change 7 – the library covers the pixel tables too

The pixel-feature tables (`table[...]`) win most of the solved tasks, so the concept library
must include their features. Their names are now printed as concepts, e.g.
`table[{color}, {enclosed}]`, and counted by `--learn-concepts`. With a prior loaded, a pixel
feature's cost in the ranking becomes `0.6 * bits(f) / log2(#features)` instead of a flat 0.6,
so a uniform prior gives the old value. This matters because every fitting program votes for
the two attempts with weight `2^-(2 * complexity)`: learned concepts now steer which answer is
submitted.

## Change 8 – more ways to see objects, and interior recolor

**New parses:**

| Parse | Objects are | Typical rule |
|---|---|---|
| `background regions` | the connected empty spaces (4-connected background components) | "the largest empty region becomes 1, the smallest 8": `color by {rank of size}` |
| `rows` | every row of the grid, background included | "a uniform row becomes 5, the others are cleared": `color by {colors}` |
| `columns` | every column | the same for columns |

Regions, rows and columns don't move, so shifts are not enumerated for them.

**Rendering canvas.** The output now starts from the input with every object erased, instead
of an empty canvas. This is the same for the old parses, where every foreground pixel is part
of some object, and correct for the new ones, where the objects don't cover everything.

**New color value: `recolor interior c`.** Pixels whose 4 neighbours all belong to the object
get color c, and the border keeps its colors ("fill the inside of the rectangle with 2").

**Score (v6).** ARC-1 training 140 / 400, ARC-2 training 219 / 1000 (+3).

## Change 9 – rules report their own description length; black is the background

**Ranking.** Every fitting program votes for the two attempts, weighted by its complexity.
Object rules used to carry a flat complexity of 3.0, so in `bb43febb` the correct zero-feature
rule `color: recolor interior 2` lost to a pixel table of complexity 2.6 that answered wrong.
A learner can now return its own description length (`Learner::learnFull`, which returns the
predictor, the readable rule and the complexity in one call). For object rules:

```
complexity = 2.0 + Σ over non-default properties ( 0.3 + 0.6 · formulas + 0.05 · table entries )
```

For the object selection: `2.6`, plus `0.3` for a symmetry, `0.3` for a fixed value and `0.2` for
object-pixels-only.

**Background.** "Most frequent color" fails when the objects cover more of the grid than the
empty space (the `bb43febb` test input has more 5s than 0s). The object rules now take
**black (0) whenever it is present**, otherwise the most frequent color.

**Score (v7: regions, rows, columns, interior recolor, self-reported complexity, black background).**
ARC-1 training 146 / 400 (+6), ARC-2 training 227 / 1000 (+8).

## Change 10 – packing objects into a small grid

**What.** For outputs smaller than the input:

```
output (W × H) = layout( [ value(o) for o in sort_by_atom(objects) ] )
```

| Part | Choices |
|---|---|
| sort | one atom (left, top, size, color, right, bottom), ascending or descending; ties by top, then left |
| layout | row by row, column by column, snake rows, snake columns |
| size | constant W × H, or n × 1, or 1 × n for n objects |
| value | the object's own color, or a learned table `formula → color` (at most half as many entries as objects) |

Cells beyond the n objects must be empty (0) in every training output.

**Rules learned on training tasks:**

| Task | Rule |
|---|---|
| `cdecee7f` | `pack the objects[single color] sorted by {left}, snake rows` |
| `5289ad53` | `pack the objects[single color] sorted by {color} descending, row by row` |
| `f8ff0b80` | `pack the objects[single color, diagonal] sorted by {size} descending, row by row` |
| `e872b94a` | `pack the objects[background regions] sorted by {left}, row by row (one column)` — a count of the empty regions |
| `44f52bb0` | `pack the objects[by color] sorted by {left}, value by {is symmetric}: 0 -> 7, 1 -> 1` |

**Score (v8).** ARC-1 training 148 / 400 (+2), ARC-2 training 231 / 1000 (+4).

## Where the remaining gap is (measured, v6/v7)

| Unsolved tasks | ARC-1 training | ARC-2 training |
|---|---|---|
| no program fits the training pairs | 211 | 675 |
| programs fit, but none answers the test input | 23 | 67 |
| an answer is given, but it is wrong | 20 | 39 |

About 85% of the gap is expressiveness: nothing can describe the transformation. Large gains
therefore need *composition* (multi-step programs and partial rules combined), not more single
concepts. Planned order:

1. output size as its own learned rule;
2. backoff for unseen table cases;
3. symmetry augmentation;
4. compositional search with observational equivalence;
5. library learning over sub-programs.

## Change 11 – backoff for unseen cases (object rules)

Every object table now remembers the value that most training objects had (`CompRule::fallback`).
A second learner per parse (`backoff objects[...]`) uses it when the test input shows a feature
value never seen in training, instead of giving no answer. It costs +1.5 complexity, so it only
decides when nothing stricter answers. The learned rule is cached (`learnRuleCached`), so the
strict and the backoff learner share one learning run.

**Score (v9).** ARC-1 training 149 / 400 (+1), ARC-2 training 234 / 1000 (+3). Wrong answers
roughly doubled (41 → 81 on ARC-2). Those tasks used to give no answer, so this costs nothing.

## Change 12 – compositional search with observational equivalence

**What.** Programs become chains of up to three **parameter-free steps**, optionally finished by
a learned step:

- **State**: the grids of all training inputs after the steps so far.
- **Steps** (about 60):
  - the 7 symmetries;
  - crop to content, crop the largest / smallest / unique object;
  - the halves, and the halves overlaid (OR, AND, XOR, NOR; left/right and top/bottom);
  - remove color c, keep only color c;
  - remove empty or repeated rows and columns;
  - scale up/down by 2 and 3;
  - append the mirror right, below, or four ways;
  - pixels fall in 4 directions, objects fall in 4 directions;
  - outline only, separator cells, remove single pixels.
- **Observational equivalence**: a state whose training grids were already reached by a
  shorter chain is dropped. Two programs with the same effect are one program, and the shorter
  is kept. This is the ToolFinder idea of filtering repeated tools, applied to whole grids.
- **Beam**: per depth, the 300 states closest to the outputs survive, size first, then the
  fraction of differing pixels.
- **Finish**:
  - an exact match;
  - a learned color map;
  - for the 2 closest states, every learner as the last step (tables, object rules, packing, …).

  The name is the program, e.g. `left half, then overlay left/right AND, then color map`.
- Tuning: `INFOCELL_COMPOSE_BEAM` and `INFOCELL_COMPOSE_LEARN` override the beam width and the
  number of states that get the full learners.

**Score (v10).** ARC-1 training 156 / 400 (+7), ARC-2 training 246 / 1000 (+12). This is the
largest single step so far, and it comes from composition, not from a new concept. Examples:

| Task | Program |
|---|---|
| `0520fde7` | `overlay left/right AND, then color map` |
| `2013d3e2` | `crop to content, then left half, then top half` |
| `90c28cc7` | `remove repeated rows and columns, then crop to content` |
| `ae4f1146` | `keep only color 1, then crop largest object (multi color), then color map` |
| `1caeab9d` | `objects fall up, then backoff objects[single color, diagonal] …` |

## Milestone: evaluation sets (aggregate only)

| Set | Baseline | v10 |
|---|---|---|
| ARC-1 evaluation | 68 / 400 (17%) | **86 / 400 (21.5%)** |
| ARC-2 evaluation | 0 / 120 | **0 / 120** (11 attempted, all wrong) |

On ARC-1 the training gains carry over to unseen tasks, so this is not overfitting. On ARC-2
nothing is solved yet.

## Change 13 – more composition steps; a wider beam

New steps. Synthetic pixels get the marker color 10, which the final color map names:

- mark enclosed areas
- extend pixels up / down / left / right (through background, in their own color)
- connect same colors horizontally / vertically
- mark object boxes
- keep only the largest / smallest object
- invert (foreground ↔ background)

**Score (v11).** ARC-1 training 155 / 400 (−1), ARC-2 training 246 / 1000 (±0). The new steps find
programs such as `mark enclosed areas, then color map` and `connect same colors horizontally`,
but mostly for tasks that other learners already solved. One depth-3 program
(`e3497940`: `append mirror right, then overlay left/right OR, then left half`) fell out of the
300-state beam because there are more competing states now. The beam was widened to 500.

**Score (v12, beam 500).** ARC-1 training 156 / 400, ARC-2 training 247 / 1000. The lost depth-3
program is found again, and runtime is unchanged (about 30 minutes for both training sets on this
machine).

## Change 14 – one background rule for self-similarity too

The `self-similar` family copies the input into every block whose input pixel is foreground, as in
3×3 → 9×9 tasks. It took the most frequent color as the background. In `007bbfb7` the most
frequent color is orange, so the family placed copies under the wrong pixels and fitted nothing.
It now uses the same rule as the object rules and the composition steps: black (0) if present,
otherwise the most frequent color. No new rule was added; only an inconsistency was removed.

**Score (v13).** ARC-1 training 158 / 400 (+2: `007bbfb7`, `8f2ea7aa`), ARC-2 training 250 / 1000
(+3). No task was lost.

## Change 15 – regions, layers and seeds: negative space and time

New perception: walls (the second most frequent color) cut the grid into **background regions**,
objects made of negative space. Each region is **peeled**: its inner border is layer 0, the inner
border of what is left is layer 1, and so on. The layer is the time step at which a pixel is
reached when the same step is repeated on the leftover.

New family on top: the non-background pixels on a region's corner diagonal are **seeds**, one
period of a color sequence over the layers; an empty position means the region's own color.
Every layer gets the color of `layer mod period`; a region without seeds stays as it is.
Two variants: peeling through 4 or 8 neighbours.

Shown by the owner on ARC-2 evaluation task `13e47133`; that task now counts as seen and is
excluded from evaluation scores. Tested first on 20 generated tasks (all solved).

**Score (v14).** ARC-1 training 158 / 400 (±0), ARC-2 training 251 / 1000 (+1: `e69241bd`, regions
filled by their single seed: the same concept with a period of one). No task was lost and the
family gave no wrong answer.

## Current status

| Set | Baseline | Now |
|---|---|---|
| ARC-1 training | 133 / 400 (33.3%) | 158 / 400 (39.5%) |
| ARC-2 training | 199 / 1000 (19.9%) | 251 / 1000 (25.1%) |
| ARC-1 evaluation | 68 / 400 (17.0%) | 86 / 400 (21.5%) (milestone v10) |
| ARC-2 evaluation | 0 / 120 | 0 / 120 (milestone v10) |
