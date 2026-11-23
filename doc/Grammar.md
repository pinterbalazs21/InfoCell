# Generator induction from valid strings

`infocell-cli grammar <file>`: the file holds valid strings, one per line, and `#` starts a comment.
The output is the shortest description that all of them fit: a pattern, an axiom schema and a rule schema.

```
> infocell-cli grammar data/formal/pq.txt
Pattern:       x p y q x y
Axiom schema:  x p - q x -
Rule schema:   x p y q x y  ->  x p y- q x y-
```

This is Hofstadter's pq-system (GEB, chapter 2), found from 36 theorems with no derivations given.

## How it works

1. **Hypotheses are patterns.**
   - A pattern is a sequence of constant symbols and variables.
   - A variable stands for a non-empty string over its own alphabet.
   - A variable may occur more than once. This is the only way to say "the same thing again", and it is how `x y` comes back after `q`.
2. **Candidates come from the data.** Every pattern that fits all examples also fits the shortest one, so it is a segmentation of that string. The search enumerates those segmentations and rejects any whose constants do not appear, in order, in every example.
3. **Choice by MDL.** The search picks the candidate with the smallest score:
   - **model bits:** the pattern's tokens and each variable's alphabet;
   - **data bits:** for each example, the length and content of each variable's value.

   A repeated variable costs nothing in the data, which is why `x p y q x y` beats `x p y q z`. Too general a pattern also loses: `x` alone has to spell out every symbol.
4. **From pattern to generator.**
   - **Axiom:** every variable at its smallest value.
   - **Rules:** each variable grows by one symbol of its alphabet, at every place it occurs.
   - **Hofstadter's form:** the axiom schema keeps all variables free except the last; the rule grows the last one.

Nothing in the search is specific to a task: no symbol is special and no rule form is built in.

## What it finds

Example files are in [data/formal](../data/formal).

| File | Found | Exact? |
|---|---|---|
| pq.txt (addition) | `x p y q x y` | yes, the GEB system |
| copy.txt (`w c w`) | `x c x`, x ∈ {a,b}+ | yes |
| anbn.txt | `x y`, x ∈ a+, y ∈ b+ | too general: the counts are not tied |
| tq.txt (multiplication) | `x t y q z` | too general: "z = x·y" can't be expressed |
| miu.txt | `M x`, x ∈ {I,U}+ | too general: it derives MU |

Every answer is a language the examples fit, which is all that positive data can guarantee (Gold, 1967).
The misses show what the hypothesis language lacks:

- **a tied copy under a symbol map:** `y = x with a→b`, which is needed for aⁿbⁿ;
- **repetition:** `z = y repeated |x| times`, which is needed for tq;
- **rules that rewrite rather than grow:** `III → U`, which is needed for MIU.
