# Andre Manada TALLY thesis source

This folder contains a cleaned reference copy of Andre Manada Correia da Silva's TALLY thesis source, extracted from `Tally_André_Manada.zip` (`EstiloTeses/`).

- `andre-manada-thesis.tex` is a flattened single-file LaTeX source. The original `fcthesis.sty` and chapter files have been inlined.
- `images/` contains every image asset from the source archive, with diagram filenames normalized to avoid spaces.
- The original archive also contained `thesis.pdf`, `.lof`, `.lot`, and `refs.bib`; those are not needed by the flattened source. The bibliography used by the thesis was already inline in `thesis.tex`.

To build locally, run from this folder:

```sh
latexmk -pdf andre-manada-thesis.tex
```

Or run `pdflatex andre-manada-thesis.tex` repeatedly until LaTeX stops asking for another pass.

The source includes TikZ diagrams and ordinary `listings` code blocks. The original preamble loaded `minted`, but the thesis source does not use any minted environments, so the flattened file leaves it disabled to avoid requiring `-shell-escape`.
