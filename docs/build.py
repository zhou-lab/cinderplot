#!/usr/bin/env python3
"""Build the cinderplot documentation site (GitHub Pages, served from docs/).

The site is plain static HTML with pre-rendered figures committed under
docs/figs/ — GitHub Pages hosts it directly, nothing runs at serve time. This
script only regenerates those figures and the HTML pages.

Single source of truth: VARIANTS below. For each we render the cinderplot SVG
and the ggplot2 reference (PDF -> PNG), then emit:
  - docs/index.html      landing page
  - docs/gallery.html    scatterplot gallery (relative asset paths, for hosting)
  - <preview>            self-contained copy, images inlined (local preview only)

Datasets live in the sibling cinderplot-examples repo (not here). Point at them
with $CINDERPLOT_EXAMPLES (default: ../cinderplot-examples). The binary is
$CINDERPLOT (default: the built ./cinderplot at the repo root).

Rendering is incremental: `python3 docs/build.py` only builds figures whose
output is missing and always rebuilds the HTML. Re-render specific figures with
`python3 docs/build.py <slug> ...`, everything with `--all`, or just the HTML
with `--html`. (Editing one figure only re-renders that one.)
"""
import base64, html, os, pathlib, subprocess

HERE     = pathlib.Path(__file__).resolve().parent      # <repo>/docs
REPO     = HERE.parent                                  # <repo>
EXAMPLES = pathlib.Path(os.environ.get(
    "CINDERPLOT_EXAMPLES", REPO.parent / "cinderplot-examples")).resolve()
# Figures are NOT committed to this repo — they're hosted from the
# cinderplot-examples Pages site so this repo stays free of growing binary
# assets. render() writes them into the examples repo (docs/figs/, published
# on its Pages); the gallery links them by absolute URL.
FIGS     = pathlib.Path(os.environ.get("CINDERPLOT_FIGS", EXAMPLES / "docs" / "figs")).resolve()
FIG_BASE = os.environ.get("CINDERPLOT_FIG_BASE",
                          "https://zhou-lab.github.io/cinderplot-examples/figs")
BIN      = os.environ.get("CINDERPLOT", str(REPO / "cinderplot"))
PREVIEW  = os.environ.get("CINDERPLOT_PREVIEW", str(HERE / "_preview.html"))
# Reference genome annotation (cytoband, seqinfo, bgzip+tabix gene models) is
# read straight from the local genomes repo — cinderplot decompresses gzip and
# region-queries the tabix index in memory, so no flattened copies live in
# data/. Override the location with $CINDERPLOT_GENOMES.
GENOMES  = os.environ.get("CINDERPLOT_GENOMES", os.path.expanduser("~/repo/genomes/hg38"))

# Gallery figures grouped into sections. Each section renders as a full-width
# separator line + heading + its figure grid. Add future sections (heatmaps,
# genomic tracks) as new entries here.
# section title -> list of (slug, title, cinderplot DSL, ggplot2 expression)
SECTIONS = [
    ("Scatterplots", [
        # title describes the feature the example illustrates, not the dataset
        ("group-cyl", "Colour by group",
         "data/mtcars.csv + aes(wt, mpg, colour=factor(cyl)) + geom_point()",
         "ggplot(mtcars, aes(wt, mpg, colour = factor(cyl))) + geom_point()"),
        ("quakes", "Continuous colour scale",
         'data/quakes.csv + aes(long, lat, colour=mag) + geom_point() + scale_colour_gradient(low="#132b43", high="#56b1f7")',
         'ggplot(quakes, aes(long, lat, colour = mag)) + geom_point() +\n  scale_colour_gradient(low = "#132b43", high = "#56b1f7")'),
        ("diamonds", "Colour by category",
         "data/diamonds_sample.csv + aes(carat, price, colour=cut) + geom_point()",
         "# diamonds, a 2000-row sample\nggplot(diamonds, aes(carat, price, colour = cut)) + geom_point()"),
        ("logx", "Log-scaled x-axis",
         "data/mtcars.csv + aes(hp, mpg) + geom_point() + scale_x_log10(limits=c(10,1000)) + ylim(0, 50)",
         'ggplot(mtcars, aes(hp, mpg)) + geom_point() +\n'
         '  scale_x_log10(breaks = trans_breaks("log10", function(x) 10^x),\n'
         '                labels = trans_format("log10", math_format(10^.x)),\n'
         '                limits = c(10, 1000)) +\n'
         '  annotation_logticks(sides = "b") + ylim(0, 50)'),
    ]),
    ("Annotations", [
        # the geoms & scales added most recently: reference lines, text labels,
        # a manual discrete palette, and multi-line titles via labs().
        ("reflines", "Reference lines",
         'data/mtcars.csv + aes(wt, mpg) + geom_point()'
         ' + geom_abline(slope=-5, intercept=37, color="#c0392b")'
         ' + geom_hline(yintercept=20, color="#7f8c8d")',
         'ggplot(mtcars, aes(wt, mpg)) + geom_point() +\n'
         '  geom_abline(slope = -5, intercept = 37, colour = "#c0392b") +\n'
         '  geom_hline(yintercept = 20, colour = "#7f8c8d")'),
        ("textlabels", "Repelled text labels",
         "data/mtcars.csv + aes(wt, mpg, label=model) + geom_point() + geom_text_repel(size=2.6)",
         "# mtcars model names, ggrepel-style placement\n"
         "library(ggrepel)\n"
         "ggplot(mtcars, aes(wt, mpg, label = rownames(mtcars))) +\n"
         "  geom_point() + geom_text_repel(size = 2.6)"),
        ("manualcol", "Manual colour scale",
         'data/mtcars.csv + aes(wt, mpg, colour=factor(cyl)) + geom_point()'
         ' + scale_colour_manual(values=c("4"="#1b9e77","6"="#d95f02","8"="#7570b3"))',
         'ggplot(mtcars, aes(wt, mpg, colour = factor(cyl))) + geom_point() +\n'
         '  scale_colour_manual(values = c("4"="#1b9e77","6"="#d95f02","8"="#7570b3"))'),
        ("labs", "Titles & captions",
         'data/mtcars.csv + aes(hp, mpg, colour=factor(cyl)) + geom_point()'
         ' + labs(title="Fuel economy", subtitle="lower is thirstier",'
         ' caption="source: mtcars", colour="cylinders")',
         'ggplot(mtcars, aes(hp, mpg, colour = factor(cyl))) + geom_point() +\n'
         '  labs(title = "Fuel economy", subtitle = "lower is thirstier",\n'
         '       caption = "source: mtcars", colour = "cylinders")'),
    ]),
    ("Distributions", [
        ("density", "Kernel density (KDE)",
         "data/gm12878_betas.tsv + aes(beta) + geom_density() + scale_x_continuous(labels=percent)",
         "# GM12878 methylation betas (bimodal)\n"
         "ggplot(betas, aes(beta)) + geom_density() +\n"
         "  scale_x_continuous(labels = scales::percent)"),
    ]),
    ("Heatmaps", [
        # matrix + row/column clustering, both dendrograms, a row annotation,
        # legend and a diverging colormap. R reference uses ComplexHeatmap.
        ("heatmap", "Complex assembly",
         'data/mtcars_heat.csv + heatmap(name="m", cluster=both, rownames=right)'
         ' + dendrogram(top_of("m")) + dendrogram(left_of("m"))'
         ' + annotation("data/mtcars_cyl.csv", left_of("m")) + legend(right_of("m"))'
         ' + scale_fill_gradient2(low="#2166ac", mid="#f7f7f7", high="#b2182b")',
         '# mtcars, z-scored features (ComplexHeatmap)\n'
         'library(ComplexHeatmap)\n'
         'm <- scale(as.matrix(mtcars[, c("mpg","disp","hp","drat","wt","qsec","carb")]))\n'
         'Heatmap(m, name = "z", cluster_rows = TRUE, cluster_columns = TRUE,\n'
         '        left_annotation = rowAnnotation(cyl = factor(mtcars$cyl)))'),
    ]),
    ("Genomics", [
        # whole-genome CNV: per-bin log2 ratios + CBS segment lines over a genome
        # coordinate axis (scale_x_genome) with a cytoband ideogram. Real K562
        # (EPICv2) data; R reference is sesame's visualizeSegments (not ggplot2).
        ("k562cnv", "Whole-genome CNV",
         'data/k562.bins.tsv'
         ' + aes(chrom=chrom, x=start, xend=end, y=log2ratio, colour=log2ratio)'
         ' + geom_point(size=0.5)'
         ' + geom_segment(data="data/k562.segments.tsv", y=seg.mean, color="black")'
         f' + scale_x_genome("{GENOMES}/seqinfo.tsv.gz")'
         ' + scale_colour_gradient2(low="#3b4cc0", mid="#dcdcdc", high="#b40426", midpoint=0, limits=c(-0.3,0.3))'
         f' + ideogram("{GENOMES}/cytoband.tsv.gz")'
         ' + labs(title="K562 copy number", y="log2 ratio")',
         "# K562 copy number, EPICv2 (sesame)\n"
         "library(sesame)\n"
         "visualizeSegments(cnv)  # bins + CBS segments + ideogram, whole-genome x"),
        # locus browser fused with a genome-anchored heatmap: cytoband + gene
        # models + probe->column map lines + a per-sample beta heatmap (parula).
        # Long/tidy betas (chrom beg end Probe_ID beta sample_name). R ref = sesame.
        ("region", "Region view (anchored heatmap)",
         'region()'
         f' + cytoband("{GENOMES}/cytoband.tsv.gz", height=0.5)'
         f' + genes("{GENOMES}/genes.bed.gz", height=1.5)'
         ' + matrix("data/region_betas_long.tsv", name="betas", cluster=samples, rownames=off, height=10)',
         "# ADA locus methylation, HM450 (sesame)\n"
         "library(sesame)\n"
         'visualizeRegion("chr20", 44616522, 44655233, betas,\n'
         '                platform = "HM450", cluster.samples = TRUE)'),
    ]),
]

# Themes showcase: the same scatter under each theme. cinderplot renders these
# (theme-*-cp.svg); no ggplot2 reference PNG is generated for them (render()
# skips gg for "theme-*" slugs, so the build needs no ggpubr/ggthemes).
_THBASE = "data/mtcars.csv + aes(wt, mpg, colour=factor(cyl)) + geom_point()"
_THEMES = [("bw", ""), ("minimal", ""), ("classic", ""), ("void", ""),
           ("linedraw", ""), ("light", ""), ("dark", ""), ("few", "requires ggthemes")]
SECTIONS.append(("Themes", [
    (f"theme-{t}", f"theme_{t}", f"{_THBASE} + theme_{t}()",
     (f"# {note}\n" if note else "")
     + f"ggplot(mtcars, aes(wt, mpg, colour = factor(cyl))) + geom_point() + theme_{t}()")
    for t, note in _THEMES
]))

VARIANTS = [v for _title, vs in SECTIONS for v in vs]   # flat list for render()/datasets

# R datasets to export to <examples>/data/ so cinderplot and ggplot read
# identical rows. varname (used in ggplot code), csv path, R expr (None = exists)
DATASETS = [
    ("mtcars",   "data/mtcars.csv",          "cbind(model = rownames(mtcars), mtcars)"),
    ("quakes",   "data/quakes.csv",          "quakes"),
    ("diamonds", "data/diamonds_sample.csv", "{set.seed(42); diamonds[sample(nrow(diamonds), 2000), ]}"),
    ("betas",    "data/gm12878_betas.tsv",   None),   # GM12878 methylation betas (TSV)
]

# figures rendered at a non-default size (WxH inches); genome plots are wide.
# k562cnv is grammar mode (wide whole-genome aspect) so it needs an explicit
# size; the heatmap and region (track) cards auto-fit from their content.
SIZES = {"k562cnv": "12x3.6"}
# figures whose gallery card spans the full row (wide banners).
WIDE = set()   # no full-width cards — every figure sits in a normal grid cell
# figures rendered as PNG instead of SVG — dense scatters / heatmaps (thousands
# of cells) are far lighter as a raster than as per-element vector shapes.
RASTER = {"k562cnv", "region"}

def cp_ext(slug):
    return "png" if slug in RASTER else "svg"

def no_gg(slug):
    """Slugs whose R reference needs a non-base package, so render() skips the
    gg PNG: theme showcases (same ggplot), the heatmap (ComplexHeatmap), the
    repelled labels (ggrepel), and the CNV plot (sesame)."""
    return slug.startswith("theme-") or slug in ("heatmap", "textlabels", "k562cnv", "region")

def _up_to_date(slug):
    """A figure is up to date when its cinderplot output exists (and, for
    figures that have a ggplot reference, the gg PNG too)."""
    if not (FIGS / f"{slug}-cp.{cp_ext(slug)}").exists():
        return False
    if not no_gg(slug) and not (FIGS / f"{slug}-gg.png").exists():
        return False
    return True

def render(only=None, force=False):
    """Render figures into FIGS. By default only figures whose output is
    MISSING are (re)built; pass `only` (a set/list of slugs) to force exactly
    those, or force=True to rebuild everything. Re-running R and cinderplot for
    unchanged figures is the slow part, so this skips them."""
    if not EXAMPLES.is_dir():
        raise SystemExit(f"example datasets not found at {EXAMPLES}\n"
                         f"clone zhou-lab/cinderplot-examples beside this repo, "
                         f"or set CINDERPLOT_EXAMPLES.")
    FIGS.mkdir(parents=True, exist_ok=True)
    (FIGS.parent / ".nojekyll").touch()   # serve the examples Pages verbatim

    if only is not None:
        targets = [v for v in VARIANTS if v[0] in set(only)]
    elif force:
        targets = list(VARIANTS)
    else:
        targets = [v for v in VARIANTS if not _up_to_date(v[0])]
    if not targets:
        print("figures up to date (nothing to render)")
        return
    print("rendering: " + ", ".join(slug for slug, *_ in targets))

    tmp = HERE / "_gen"
    tmp.mkdir(exist_ok=True)
    # 0. export any missing R datasets to <examples>/data/ (only if some are absent)
    exp = [f"write.csv({expr}, '{csv}', row.names=FALSE)"
           for _v, csv, expr in DATASETS if expr and not (EXAMPLES / csv).exists()]
    if exp:
        (tmp / "export.R").write_text("suppressMessages(library(ggplot2))\n"
                                      + "\n".join(exp) + "\n")
        subprocess.run(["Rscript", str(tmp / "export.R")], cwd=EXAMPLES, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # 1. ggplot references — one R script, only for the targeted non-no_gg figures
    gg = [(slug, rc) for slug, _t, _cp, rc in targets if not no_gg(slug)]
    if gg:
        r = ["suppressMessages({library(ggplot2); library(scales)})"]
        for var, csv, _e in DATASETS:
            rd = "read.delim" if csv.endswith(".tsv") else "read.csv"
            r.append(f"{var} <- {rd}('{csv}')")
        for slug, rc in gg:
            r.append(f"ggsave('{FIGS / (slug + '-gg.pdf')}', {rc}, width=6, height=4)")
        (tmp / "gen.R").write_text("\n".join(r) + "\n")
        subprocess.run(["Rscript", str(tmp / "gen.R")], cwd=EXAMPLES, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # 2. rasterise ggplot PDFs, render cinderplot figures (both read <examples>/data)
    for slug, _t, cp, _rc in targets:
        if not no_gg(slug):
            subprocess.run(["pdftoppm", "-r", "150", "-png", "-singlefile",
                            str(FIGS / f"{slug}-gg.pdf"), str(FIGS / f"{slug}-gg")],
                           check=True)
            (FIGS / f"{slug}-gg.pdf").unlink(missing_ok=True)   # keep only PNG
        ext = cp_ext(slug)
        cmd = [BIN, cp, "-o", str(FIGS / f"{slug}-cp.{ext}")]
        if slug in SIZES:
            cmd += ["--size", SIZES[slug]]
        if ext == "png":
            cmd += ["--dpi", "200"]
        subprocess.run(cmd, cwd=EXAMPLES, check=True, stdout=subprocess.DEVNULL)

def esc(s):
    return html.escape(s)

ZOOM_SVG = ('<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"'
            ' stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'
            '<circle cx="11" cy="11" r="7"/><line x1="20.5" y1="20.5" x2="16.5" y2="16.5"/>'
            '<line x1="11" y1="8.2" x2="11" y2="13.8"/><line x1="8.2" y1="11" x2="13.8" y2="11"/></svg>')

def card_html(i, slug, title, cp, rc, src):
    """One figure card. i is the global 1-based index (also shown in the caption
    and used as the zoom button's 0-based data-i)."""
    out_line = "  -o out.svg" + (f" --size {SIZES[slug]}" if slug in SIZES else "")
    # Render uses the absolute genomes-repo path; the shown command abbreviates
    # it to hg38/… so the published HTML carries no local home directory.
    shown = cp.replace(GENOMES, "hg38")
    cp_cmd = "cinderplot '" + shown.replace(" + ", "\n  + ") + "' \\\n" + out_line
    cls = "cell wide" if slug in WIDE else "cell"
    return f'''        <figure class="{cls}" tabindex="0" role="button" aria-label="Figure {i}: {esc(title)} — show code" data-title="{i} · {esc(title)}">
          <div class="thumb"><button class="zoom" data-i="{i-1}" aria-label="Maximize figure {i}">{ZOOM_SVG}</button><img src="{src(slug, 'cp.' + cp_ext(slug))}" alt="{esc(title)} — cinderplot"></div>
          <figcaption><span class="num">{i}</span>{esc(title)}</figcaption>
          <template class="code">
            <div class="snip"><span class="k">cinderplot</span><button class="copy" type="button" aria-label="Copy cinderplot command">Copy</button><pre>{esc(cp_cmd)}</pre></div>
            <div class="snip"><span class="k">R</span><pre>{esc(rc)}</pre></div>
          </template>
        </figure>'''

def sections(src):
    """Render all figures as one continuous stream: each section contributes an
    inline divider (label + rule that fills the rest of the row) followed by its
    cards, so cards pack continuously across sections. Figures are numbered
    globally (1..N) in document order."""
    out, i = [], 0
    for sec_title, variants in SECTIONS:
        out.append('      <div class="divider" role="button" tabindex="0" aria-expanded="true">'
                   '<span class="caret" aria-hidden="true">&#9656;</span>'
                   '<span class="d__label">{}</span>'
                   '<span class="d__n">{}</span></div>'.format(esc(sec_title), len(variants)))
        for (slug, title, cp, rc) in variants:
            i += 1
            out.append(card_html(i, slug, title, cp, rc, src))
    return '    <div class="stream">\n' + "\n".join(out) + '\n    </div>'

STYLE = """<style>
  :root{
    --accent:#0a5bb5; --accent-hover:#084a96; --accent-bright:#2f86ff; --accent-deep:#06223f;
    --accent-soft:#dce8f7; --accent-tint:#eef4fb;
    --ink:#0b1220; --body:#3a4658; --muted:#6b7688; --line:#e6eaf0; --bg:#ffffff; --bg-soft:#eef1f6;
    --code-ink:#e7eef7; --code-dim:#7fa8d8;
    --font:"Inter",system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
    --head:"Space Grotesk",var(--font); --mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
    --radius:10px; --shadow:0 1px 2px rgba(11,18,32,.05),0 4px 14px rgba(11,18,32,.05);
  }
  *{box-sizing:border-box;}
  /* app shell: header + drawer are fixed rows; only .scroll scrolls, so the
     drawer sits OUTSIDE the figures' scrollbar and stays put as they scroll. */
  html,body{height:100%;}
  body{margin:0;background:var(--bg-soft);color:var(--body);font-family:var(--font);
       font-size:15px;line-height:1.5;-webkit-font-smoothing:antialiased;
       height:100vh;display:flex;flex-direction:column;overflow:hidden;}
  a{color:inherit;}
  .wrap{max-width:none;margin:0;padding:0 28px;}   /* gallery fills the display */
  header.site{flex:0 0 auto;background:var(--bg);border-bottom:1px solid var(--line);}
  header.site .wrap{display:flex;align-items:center;gap:12px;height:56px;}
  .mark{display:flex;align-items:center;gap:8px;text-decoration:none;color:var(--ink);
        font-family:var(--head);font-weight:700;font-size:1.1rem;letter-spacing:-.02em;}
  .mark .sq{width:12px;height:12px;border-radius:3px;background:var(--accent);}
  .mark .sub{color:var(--muted);font-weight:500;font-size:.85rem;}
  .nav{margin-left:auto;display:flex;gap:18px;font-family:var(--head);font-weight:500;font-size:.85rem;}
  .nav a{color:var(--muted);text-decoration:none;transition:color .15s;}
  .nav a:hover,.nav a.active{color:var(--accent);}
  main.scroll{flex:1 1 auto;min-height:0;overflow-y:auto;padding:20px 0 60px;}
  /* one continuous stream: cards flow across sections; each section label is an
     inline divider that grows to fill the rest of its row, so there are no
     per-section blocks and no ragged gaps between sections. */
  .galtools{position:sticky;top:0;z-index:6;display:flex;justify-content:flex-end;
    margin:0 0 -4px;padding:12px 0 8px;background:var(--bg);}
  .foldbtn{font-family:var(--head);font-weight:600;font-size:.72rem;letter-spacing:.04em;
    color:var(--muted);background:var(--bg);border:1px solid var(--line);border-radius:7px;
    padding:5px 13px;cursor:pointer;box-shadow:var(--shadow);
    transition:color .15s,border-color .15s,background .15s;}
  .foldbtn:hover{color:var(--accent);border-color:var(--accent);background:var(--accent-tint);}
  .stream{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:16px;align-items:start;margin-top:6px;}
  /* section titles are card-sized tiles, so titles and figures align in one grid */
  .divider{cursor:pointer;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:10px;
           aspect-ratio:4 / 3;border:1px dashed var(--line);border-radius:10px;background:var(--bg-soft);
           text-align:center;padding:12px;transition:border-color .15s,background .15s;}
  .divider:hover{border-color:var(--accent);background:var(--accent-tint);}
  .divider .caret{color:var(--accent);font-size:1.9rem;line-height:1;transition:transform .15s ease;}
  .divider[aria-expanded="true"] .caret{transform:rotate(90deg);}
  .divider .d__label{font-family:var(--head);font-weight:600;font-size:1rem;color:var(--ink);}
  .divider:hover .d__label{color:var(--accent);}
  .divider .d__n{font-family:var(--head);font-weight:600;font-size:.64rem;color:var(--muted);
                 background:var(--bg);border:1px solid var(--line);border-radius:99px;padding:1px 8px;}
  .smallprint{color:var(--muted);font-size:.76rem;margin:24px 0 0;}
  .smallprint a{color:var(--accent);text-decoration:none;}
  .smallprint a:hover{text-decoration:underline;}
  .cell{position:relative;margin:0;cursor:pointer;}
  .cell.hidden{display:none;}
  /* the figure fills the card (object-fit: cover): its constraining side scales
     to the card edge, the other side overflows and is clipped. */
  .thumb{position:relative;border:1px solid var(--line);border-radius:10px;overflow:hidden;background:#f5f6f8;
         box-shadow:var(--shadow);transition:border-color .15s,transform .15s;aspect-ratio:4 / 3;}
  .thumb img{display:block;width:100%;height:100%;object-fit:cover;object-position:left top;background:#fff;}
  .cell:hover .thumb,.cell:focus-visible .thumb{border-color:var(--accent);transform:translateY(-2px);}
  .cell.on .thumb{border-color:var(--accent);}
  .zoom{position:absolute;top:8px;right:8px;z-index:2;width:26px;height:26px;padding:0;border:0;
        border-radius:6px;background:rgba(6,34,63,.72);color:#fff;cursor:zoom-in;
        display:flex;align-items:center;justify-content:center;opacity:0;
        -webkit-backdrop-filter:blur(2px);backdrop-filter:blur(2px);transition:opacity .15s,background .15s;}
  .zoom svg{width:15px;height:15px;}
  .cell:hover .zoom,.cell:focus-within .zoom,.zoom:focus-visible{opacity:1;}
  .zoom:hover{background:var(--accent);}
  /* lightbox — full-screen maximized figure viewer with prev/next */
  .lb{position:fixed;inset:0;z-index:100;background:rgba(6,18,32,.94);
      -webkit-backdrop-filter:blur(2px);backdrop-filter:blur(2px);
      display:none;align-items:center;justify-content:center;}
  .lb.on{display:flex;}
  .lb__bar{position:fixed;top:0;left:0;right:0;height:56px;display:flex;align-items:center;
           justify-content:space-between;padding:0 14px 0 22px;background:linear-gradient(rgba(0,0,0,.4),transparent);}
  .lb__count{color:#cdd9ea;font-family:var(--head);font-weight:600;font-size:.9rem;}
  .lb__tools{display:flex;align-items:center;gap:2px;}
  .lb__close{background:none;border:0;color:#fff;font-size:2rem;line-height:1;cursor:pointer;width:44px;height:44px;
             border-radius:9px;display:flex;align-items:center;justify-content:center;}
  .lb__zoom{background:none;border:0;color:#fff;cursor:pointer;width:40px;height:40px;border-radius:9px;
            display:flex;align-items:center;justify-content:center;}
  .lb__zoom svg{width:20px;height:20px;}
  .lb__close:hover,.lb__zoom:hover{background:rgba(255,255,255,.14);}
  .lb__fig{margin:0;display:flex;align-items:center;justify-content:center;padding:64px 7vw 88px;}
  .lb__img{max-width:86vw;max-height:76vh;object-fit:contain;border-radius:10px;background:#fff;
           box-shadow:0 24px 70px rgba(0,0,0,.55);cursor:zoom-in;}
  /* zoomed: image grows past fit and the figure area scrolls to pan */
  .lb.zoomed .lb__fig{overflow:auto;align-items:flex-start;justify-content:center;}
  .lb.zoomed .lb__img{max-width:none;max-height:none;width:165vw;cursor:zoom-out;}
  .lb__nav{position:fixed;top:50%;transform:translateY(-50%);background:rgba(255,255,255,.1);border:0;color:#fff;
           font-size:1.9rem;line-height:1;width:50px;height:50px;border-radius:50%;cursor:pointer;
           display:flex;align-items:center;justify-content:center;transition:background .15s;}
  .lb__nav:hover{background:rgba(255,255,255,.26);}
  .lb__prev{left:18px;}.lb__next{right:18px;}
  .lb__foot{position:fixed;left:0;right:0;bottom:0;padding:1rem 16px .95rem;text-align:center;
            background:linear-gradient(transparent,rgba(0,0,0,.5) 40%);}
  .lb__cap{color:#e4ecf7;font-family:var(--head);font-weight:500;font-size:.9rem;}
  @media (max-width:560px){.lb__img{max-width:94vw;max-height:66vh;}
                           .lb__nav{width:42px;height:42px;font-size:1.5rem;}.lb__prev{left:6px;}.lb__next{right:6px;}}
  .cell:focus-visible{outline:none;}
  .cell:focus-visible .thumb{outline:2px solid var(--accent);outline-offset:2px;}
  figcaption{font-family:var(--head);font-weight:500;font-size:.8rem;color:var(--ink);padding:8px 2px 0;}
  figcaption .num{color:var(--accent);font-weight:700;margin-right:6px;}
  /* click-to-open panel in PUSH mode: it sits in the normal flow just under the
     header and animates its height, so the grid slides down instead of being
     covered. (overlay mode would float on top; this reflows the page.) */
  .drawer{flex:0 0 auto;overflow:hidden;max-height:0;transition:max-height .28s cubic-bezier(.4,0,.2,1);}
  .drawer.open{max-height:52vh;}
  /* panel sizes to its content; 52vh is only the cap (body scrolls past it) */
  .drawer__panel{max-height:52vh;display:flex;flex-direction:column;color:var(--code-ink);
                 background:rgba(6,34,63,.96);-webkit-backdrop-filter:blur(8px);backdrop-filter:blur(8px);
                 border-bottom:1px solid rgba(255,255,255,.12);box-shadow:0 18px 40px rgba(6,18,32,.28);}
  .drawer__bar{display:flex;align-items:center;gap:10px;padding:13px 22px;
               border-bottom:1px solid rgba(255,255,255,.1);flex:0 0 auto;}
  .drawer__title{font-family:var(--head);font-weight:600;font-size:.95rem;color:#fff;}
  .drawer__hint{font-size:.72rem;color:#8ba1c2;}
  .drawer__close{margin-left:auto;background:none;border:0;color:#c2d3ea;font-size:24px;line-height:1;
                 cursor:pointer;padding:0 4px;}
  .drawer__close:hover{color:#fff;}
  .drawer__body{flex:1 1 auto;min-height:0;overflow:auto;padding:18px 22px;
                display:grid;grid-template-columns:1fr 1fr;gap:22px;align-content:start;}
  @media (max-width:760px){.drawer.open{max-height:62vh;}.drawer__panel{max-height:62vh;}
                           .drawer__body{grid-template-columns:1fr;}}
  .snip{position:relative;}
  .snip .k{display:inline-block;vertical-align:middle;font-family:var(--head);font-weight:600;font-size:.64rem;
           letter-spacing:.09em;text-transform:uppercase;color:var(--accent-bright);margin-bottom:7px;}
  .copy{margin-left:10px;vertical-align:middle;font-family:var(--head);font-size:.66rem;letter-spacing:.04em;
        color:var(--code-dim);background:rgba(127,168,216,.10);border:1px solid rgba(127,168,216,.28);
        border-radius:6px;padding:2px 9px;cursor:pointer;transition:color .15s,background .15s,border-color .15s;}
  .copy:hover{color:#fff;background:rgba(127,168,216,.20);border-color:rgba(127,168,216,.5);}
  .copy.done{color:#0d1522;background:var(--accent-bright);border-color:var(--accent-bright);}
  .snip pre{margin:0;font-family:var(--mono);font-size:12.5px;line-height:1.6;color:var(--code-ink);
            white-space:pre;overflow-x:auto;}
  footer{color:var(--muted);font-family:var(--head);font-size:.8rem;border-top:1px solid var(--line);
         margin-top:44px;padding-top:18px;}
  footer a{color:var(--accent);text-decoration:none;}
  @media (prefers-reduced-motion:reduce){.thumb,.drawer{transition:none;}}
</style>"""

def header(active):
    def link(href, label):
        cls = ' class="active"' if label.lower() == active else ""
        return f'<a href="{href}"{cls}>{label}</a>'
    return f"""<header class="site">
  <div class="wrap">
    <a class="mark" href="index.html"><span class="sq"></span>cinderplot <span class="sub">/ gallery</span></a>
    <nav class="nav">{link("index.html","Overview")}{link("gallery.html","Gallery")}<a href="https://github.com/zhou-lab/cinderplot">GitHub</a></nav>
  </div>
</header>"""

GALLERY_BODY = """
<div class="drawer" id="drawer" aria-hidden="true" role="region" aria-label="Figure code">
  <div class="drawer__panel">
    <div class="drawer__bar">
      <span class="drawer__title" id="drawerTitle"></span>
      <span class="drawer__hint">cinderplot command &amp; the R original</span>
      <button class="drawer__close" id="drawerClose" aria-label="Close code panel">&times;</button>
    </div>
    <div class="drawer__body" id="drawerBody"></div>
  </div>
</div>
<main class="scroll">
  <div class="wrap">
    <div class="galtools"><button id="foldAll" class="foldbtn" type="button">Fold all</button></div>
__SECTIONS__
    <p class="smallprint">Datasets used in these examples (mtcars, quakes, diamonds, …) are available from the
      <a href="https://github.com/zhou-lab/cinderplot-examples">cinderplot-examples</a> repository.</p>
    <footer>Every figure rendered by cinderplot straight from a CSV · click a figure for its code, or ⛶ to enlarge ·
      <a href="https://github.com/zhou-lab/cinderplot">github.com/zhou-lab/cinderplot</a></footer>
  </div>
</main>
<div class="lb" id="lb" aria-hidden="true" role="dialog" aria-label="Figure viewer">
  <div class="lb__bar">
    <span class="lb__count" id="lbCount"></span>
    <div class="lb__tools">
      <button class="lb__zoom" id="lbZoom" aria-label="Zoom in">__ZOOMICON__</button>
      <button class="lb__close" id="lbClose" aria-label="Close viewer">&times;</button>
    </div>
  </div>
  <figure class="lb__fig"><img class="lb__img" id="lbImg" alt=""></figure>
  <button class="lb__nav lb__prev" id="lbPrev" aria-label="Previous figure">&#8249;</button>
  <button class="lb__nav lb__next" id="lbNext" aria-label="Next figure">&#8250;</button>
  <div class="lb__foot"><span class="lb__cap" id="lbCap"></span></div>
</div>
<script>
(function () {
  var cells = [].slice.call(document.querySelectorAll('.cell'));

  // --- fold sections: a divider hides the cards that follow it (up to the
  //     next divider); a fold/unfold-all toggle mirrors the state ---
  var dividers = [].slice.call(document.querySelectorAll('.divider'));
  function sectionCards(div) {
    var els = [], n = div.nextElementSibling;
    while (n && !n.classList.contains('divider')) {
      if (n.classList.contains('cell')) els.push(n);
      n = n.nextElementSibling;
    }
    return els;
  }
  function setFold(div, folded) {
    div.setAttribute('aria-expanded', folded ? 'false' : 'true');
    sectionCards(div).forEach(function (c) { c.classList.toggle('hidden', folded); });
  }
  var foldBtn = document.getElementById('foldAll');
  function anyOpen() { return dividers.some(function (d) { return d.getAttribute('aria-expanded') !== 'false'; }); }
  function syncFoldBtn() { if (foldBtn) foldBtn.textContent = anyOpen() ? 'Fold all' : 'Unfold all'; }
  dividers.forEach(function (div) {
    function toggle() { setFold(div, div.getAttribute('aria-expanded') !== 'false'); syncFoldBtn(); }
    div.addEventListener('click', toggle);
    div.addEventListener('keydown', function (e) {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggle(); }
    });
  });
  if (foldBtn) foldBtn.addEventListener('click', function () {
    var fold = anyOpen();
    dividers.forEach(function (d) { setFold(d, fold); });
    syncFoldBtn();
  });
  syncFoldBtn();

  // --- code drop-down (push panel) ---
  var drawer = document.getElementById('drawer'),
      dBody  = document.getElementById('drawerBody'),
      dTitle = document.getElementById('drawerTitle'),
      current = null;
  function closeDrawer() {
    drawer.classList.remove('open');
    drawer.setAttribute('aria-hidden', 'true');
    if (current) current.classList.remove('on');
    current = null;
  }
  function openDrawer(cell) {
    if (current === cell) { closeDrawer(); return; }   // click same figure -> close
    if (current) current.classList.remove('on');
    dBody.innerHTML = '';
    dBody.appendChild(cell.querySelector('template.code').content.cloneNode(true));
    dTitle.textContent = cell.getAttribute('data-title') || '';
    cell.classList.add('on');
    current = cell;
    drawer.classList.add('open');
    drawer.setAttribute('aria-hidden', 'false');
  }
  cells.forEach(function (c) {
    c.addEventListener('click', function (e) {
      if (e.target.closest('.zoom')) return;           // zoom button handles itself
      openDrawer(c);
    });
    c.addEventListener('keydown', function (e) {
      if (e.target !== c) return;                       // ignore the inner zoom button
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); openDrawer(c); }
    });
  });
  document.getElementById('drawerClose').addEventListener('click', closeDrawer);

  // --- copy the cinderplot command to the clipboard ---
  function copyFallback(text) {
    var ta = document.createElement('textarea');
    ta.value = text; ta.style.position = 'fixed'; ta.style.opacity = '0';
    document.body.appendChild(ta); ta.select();
    try { document.execCommand('copy'); } catch (err) {}
    document.body.removeChild(ta);
  }
  dBody.addEventListener('click', function (e) {
    var btn = e.target.closest('.copy');
    if (!btn) return;
    var pre = btn.parentNode.querySelector('pre');
    if (!pre) return;
    function done() {
      btn.textContent = 'Copied!'; btn.classList.add('done');
      setTimeout(function () { btn.textContent = 'Copy'; btn.classList.remove('done'); }, 1500);
    }
    if (navigator.clipboard && navigator.clipboard.writeText)
      navigator.clipboard.writeText(pre.textContent).then(done, function () { copyFallback(pre.textContent); done(); });
    else { copyFallback(pre.textContent); done(); }
  });

  // --- lightbox (maximize) ---
  var figs = cells.map(function (c) {
    return { src: c.querySelector('.thumb img').src, cap: c.getAttribute('data-title') || '' };
  });
  var lb = document.getElementById('lb'), lbImg = document.getElementById('lbImg'),
      lbCap = document.getElementById('lbCap'), lbCount = document.getElementById('lbCount'),
      lbZoom = document.getElementById('lbZoom'), cur = 0;
  function setZoom(on) {
    lb.classList.toggle('zoomed', on);
    lbZoom.setAttribute('aria-label', on ? 'Zoom out' : 'Zoom in');
    if (!on && lb.querySelector('.lb__fig')) lb.querySelector('.lb__fig').scrollTo(0, 0);
  }
  function show(i) {
    cur = (i + figs.length) % figs.length;
    setZoom(false);                                    // reset zoom when navigating
    lbImg.src = figs[cur].src; lbImg.alt = figs[cur].cap;
    lbCap.textContent = figs[cur].cap;
    lbCount.textContent = (cur + 1) + ' / ' + figs.length;
  }
  function openLB(i) { show(i); lb.classList.add('on'); lb.setAttribute('aria-hidden', 'false'); }
  function closeLB() { lb.classList.remove('on'); lb.setAttribute('aria-hidden', 'true'); }
  [].slice.call(document.querySelectorAll('.zoom')).forEach(function (b) {
    b.addEventListener('click', function (e) { e.stopPropagation(); openLB(+b.getAttribute('data-i')); });
  });
  document.getElementById('lbPrev').addEventListener('click', function () { show(cur - 1); });
  document.getElementById('lbNext').addEventListener('click', function () { show(cur + 1); });
  document.getElementById('lbClose').addEventListener('click', closeLB);
  lbZoom.addEventListener('click', function (e) { e.stopPropagation(); setZoom(!lb.classList.contains('zoomed')); });
  lbImg.addEventListener('click', function (e) { e.stopPropagation(); setZoom(!lb.classList.contains('zoomed')); });
  lb.addEventListener('click', function (e) {
    if (e.target === lb) closeLB();
    else if (e.target.classList.contains('lb__fig') && !lb.classList.contains('zoomed')) closeLB();
  });

  document.addEventListener('keydown', function (e) {
    if (lb.classList.contains('on')) {
      if (e.key === 'Escape') closeLB();
      else if (e.key === 'ArrowLeft') show(cur - 1);
      else if (e.key === 'ArrowRight') show(cur + 1);
      return;
    }
    if (e.key === 'Escape') closeDrawer();
  });
})();
</script>"""

# Landing-page content (compact, info-rich, lab-website style). Only the
# landing uses this; the gallery keeps STYLE/header() above.
GEOMS = ["point", "line", "col", "bar", "histogram", "boxplot", "density", "rect", "segment",
         "raster", "text", "label", "text_repel", "hline", "vline", "abline"]
SCALES = ["x / y log10", "percent labels", "genome x", "colour hue", "colour gradient",
          "gradient2", "manual colours", "discrete x / y"]
POSITIONS = ["stack", "dodge", "dodge2"]
MODES = ["scatter / grammar", "heatmap + clustering", "genomic tracks"]
THEMES_CHIPS = ["gray", "bw", "minimal", "classic", "void", "linedraw",
                "light", "dark", "few"]

FONTS = ('<link rel="preconnect" href="https://fonts.googleapis.com">'
         '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>'
         '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?'
         'family=Inter:wght@400;500;600&family=Space+Grotesk:wght@500;600;700&display=swap">')

LANDING_STYLE = """<style>
  :root{
    --accent:#0a5bb5; --accent-hover:#084a96; --accent-bright:#2f86ff; --accent-deep:#06223f;
    --accent-soft:#dce8f7; --accent-tint:#eef4fb;
    --ink:#0b1220; --body:#3a4658; --muted:#6b7688; --line:#e6eaf0; --bg:#ffffff; --bg-soft:#eef1f6;
    --code-bg:#06223f; --code-ink:#e7eef7; --code-dim:#7fa8d8;
    --font:"Inter",system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
    --head:"Space Grotesk",var(--font); --radius:10px;
    --shadow:0 1px 2px rgba(11,18,32,.05),0 4px 14px rgba(11,18,32,.05);
  }
  *{box-sizing:border-box;}
  body{margin:0;font-family:var(--font);background:var(--bg-soft);color:var(--body);font-size:15px;
       line-height:1.5;-webkit-font-smoothing:antialiased;}
  h1,h2,h3{font-family:var(--head);color:var(--ink);line-height:1.14;margin:0;letter-spacing:-.02em;font-weight:600;}
  a{color:inherit;}
  .wrap{max-width:1000px;margin:0 auto;padding:0 24px;}
  main{padding:34px 0 56px;}
  .hbrand{display:flex;align-items:center;gap:8px;font-family:var(--head);font-weight:700;
          font-size:1.05rem;color:#fff;letter-spacing:-.02em;margin-bottom:15px;}
  .hbrand .sq{width:13px;height:13px;border-radius:3px;background:var(--accent-bright);}
  .panel{background:var(--bg);border:1px solid var(--line);border-radius:var(--radius);
         box-shadow:var(--shadow);margin-bottom:18px;overflow:hidden;}
  .panel__head{display:flex;align-items:baseline;gap:.5rem;padding:13px 18px;border-bottom:1px solid var(--line);}
  .panel__head h2{font-size:1rem;}
  .count{font-family:var(--head);font-size:.68rem;font-weight:600;color:var(--accent);background:var(--accent-tint);
         border:1px solid var(--accent-soft);padding:.08rem .45rem;border-radius:99px;}
  .panel__head .r{margin-left:auto;font-family:var(--head);font-weight:600;font-size:.78rem;color:var(--muted);text-decoration:none;}
  .panel__head .r:hover{color:var(--accent);}
  .panel__body{padding:16px 18px;}
  /* hero (dark navy, like the lab intro) */
  .hero{background:var(--accent-deep);border-color:transparent;padding:30px 26px 24px;color:#c2d3ea;}
  .hero .eyebrow{font-family:var(--head);font-size:.7rem;letter-spacing:.16em;text-transform:uppercase;
                 color:var(--accent-bright);font-weight:600;margin-bottom:.55rem;}
  .hero h1{color:#fff;font-size:clamp(1.55rem,3.4vw,2.05rem);margin-bottom:.55rem;max-width:22ch;}
  .hero p{color:#c2d3ea;max-width:58ch;margin:0 0 1.1rem;font-size:.97rem;}
  .cta{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:1.4rem;}
  .btn{font-family:var(--head);font-weight:600;font-size:.84rem;text-decoration:none;padding:.5rem 1rem;
       border-radius:8px;border:1px solid rgba(255,255,255,.2);color:#fff;transition:background .15s,border-color .15s;}
  .btn:hover{border-color:var(--accent-bright);}
  .btn.pri{background:var(--accent);border-color:var(--accent);}
  .btn.pri:hover{background:var(--accent-hover);}
  .hstats{display:flex;flex-wrap:wrap;gap:28px;border-top:1px solid rgba(255,255,255,.1);padding-top:16px;}
  .hstat b{font-family:var(--head);font-size:1.22rem;color:#fff;display:block;line-height:1;}
  .hstat span{font-size:.7rem;color:#8ba1c2;}
  .cols{display:grid;grid-template-columns:1.05fr .95fr;gap:18px;}
  .cols .panel{margin:0;}
  @media (max-width:860px){.cols{grid-template-columns:1fr;}}
  pre.code{margin:0;background:var(--code-bg);color:var(--code-ink);border-radius:9px;padding:14px 16px;
           font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:12.5px;line-height:1.6;overflow:auto;}
  pre.code .c{color:var(--code-dim);}
  .feat{list-style:none;margin:0;padding:0;display:flex;flex-direction:column;gap:12px;}
  .feat li{display:grid;grid-template-columns:auto 1fr;gap:10px;align-items:start;}
  .feat .d{width:8px;height:8px;border-radius:2px;background:var(--accent);margin-top:6px;}
  .feat b{font-family:var(--head);color:var(--ink);font-size:.9rem;}
  .feat span{color:var(--muted);font-size:.85rem;}
  .chips{display:flex;flex-wrap:wrap;gap:7px;}
  .chipset{margin-bottom:13px;}
  .chipset:last-child{margin-bottom:0;}
  .chipset .k{font-family:var(--head);font-size:.68rem;letter-spacing:.08em;text-transform:uppercase;
              color:var(--muted);font-weight:600;margin:0 0 7px;}
  .chip{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:.76rem;color:var(--ink);
        background:var(--accent-tint);border:1px solid var(--accent-soft);border-radius:6px;padding:.2rem .5rem;}
  .foot{color:var(--muted);font-size:.8rem;text-align:center;padding:8px 0 0;}
  .foot a{color:var(--accent);text-decoration:none;}
</style>"""

LANDING_BODY = """<main>
  <div class="wrap">
    <section class="panel hero">
      <div class="hbrand"><span class="sq"></span>cinderplot</div>
      <p class="eyebrow">Grammar-inspired plotting in C</p>
      <h1>Publication-ready graphics from one small, fast binary.</h1>
      <p>cinderplot turns a CSV into a PDF, SVG or PNG with a ggplot2-inspired grammar — panels, hue palettes,
         scales, legends — rendered by a single C binary with Cairo. No R, no Python at plot time.</p>
      <div class="cta">
        <a class="btn pri" href="gallery.html">See the gallery →</a>
        <a class="btn" href="https://github.com/zhou-lab/cinderplot">GitHub</a>
      </div>
      <div class="hstats">
        <div class="hstat"><b>__NGEOM__</b><span>geoms</span></div>
        <div class="hstat"><b>__NSCALE__</b><span>scales</span></div>
        <div class="hstat"><b>3</b><span>modes</span></div>
        <div class="hstat"><b>PDF·SVG·PNG</b><span>output</span></div>
        <div class="hstat"><b>C11</b><span>+ Cairo</span></div>
      </div>
    </section>
    <div class="cols">
      <section class="panel">
        <div class="panel__head"><h2>Install &amp; plot</h2></div>
        <div class="panel__body">
          <pre class="code"><span class="c"># from the zhou-lab conda channel</span>
conda install -c zhou-lab cinderplot

<span class="c"># plot straight from a CSV</span>
cinderplot 'data.csv
  + aes(hp, mpg, colour=factor(cyl))
  + geom_point()' -o plot.pdf</pre>
        </div>
      </section>
      <section class="panel">
        <div class="panel__head"><h2>Why cinderplot</h2></div>
        <div class="panel__body">
          <ul class="feat">
            <li><span class="d"></span><div><b>Small &amp; fast.</b> <span>One C11 binary, Cairo output. Milliseconds per figure, no runtime.</span></div></li>
            <li><span class="d"></span><div><b>Familiar grammar.</b> <span>aes, geoms, scales, legends and themes inspired by ggplot2 — reproduces <code>theme_gray</code> by default, with <code>theme_bw</code>/<code>minimal</code>/<code>classic</code>/<code>void</code> and more.</span></div></li>
            <li><span class="d"></span><div><b>Vector or raster.</b> <span>PDF, SVG or PNG chosen by the output file extension (<code>--dpi</code> for PNG). Publication-ready.</span></div></li>
          </ul>
        </div>
      </section>
    </div>
    <section class="panel">
      <div class="panel__head"><h2>Supported grammar</h2><span class="count">growing</span>
        <a class="r" href="gallery.html">see it drawn →</a></div>
      <div class="panel__body">
        <div class="chipset"><p class="k">Geoms</p><div class="chips">__GEOMS__</div></div>
        <div class="chipset"><p class="k">Scales</p><div class="chips">__SCALES__</div></div>
        <div class="chipset"><p class="k">Positions</p><div class="chips">__POS__</div></div>
        <div class="chipset"><p class="k">Modes</p><div class="chips">__MODES__</div></div>
        <div class="chipset"><p class="k">Themes</p><div class="chips">__THEMES__</div></div>
      </div>
    </section>
    <p class="foot"><a href="gallery.html">Gallery</a> ·
      <a href="https://github.com/zhou-lab/cinderplot">GitHub</a> ·
      <a href="https://github.com/zhou-lab/cinderplot-examples">Examples</a> ·
      MIT · <a href="https://github.com/zhou-lab">zhou-lab</a></p>
  </div>
</main>"""

def landing_html():
    def chips(items):
        return "".join(f'<span class="chip">{esc(x)}</span>' for x in items)
    body = (LANDING_BODY
            .replace("__NGEOM__", str(len(GEOMS)))
            .replace("__NSCALE__", str(len(SCALES)))
            .replace("__GEOMS__", chips(f"geom_{g}" for g in GEOMS))
            .replace("__SCALES__", chips(SCALES))
            .replace("__POS__", chips(f"position_{p}" for p in POSITIONS))
            .replace("__MODES__", chips(MODES))
            .replace("__THEMES__", chips(f"theme_{t}" for t in THEMES_CHIPS)))
    return ("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            "<title>cinderplot — fast grammar plotting in C</title>\n"
            + FONTS + "\n" + LANDING_STYLE + "\n</head>\n<body>\n" + body + "\n</body>\n</html>\n")

def page(title, active, body):
    return ("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            f"<title>{esc(title)}</title>\n" + FONTS + "\n" + STYLE + "\n</head>\n<body>\n"
            + header(active) + body + "\n</body>\n</html>\n")

def build_html():
    gbody = GALLERY_BODY.replace("__ZOOMICON__", ZOOM_SVG)
    # landing page (its own lab-style layout); gallery uses STYLE/header()
    (HERE / "index.html").write_text(landing_html())
    (HERE / "gallery.html").write_text(
        page("cinderplot gallery", "gallery",
             gbody.replace("__SECTIONS__", sections(lambda s, e: f"{FIG_BASE}/{s}-{e}"))))
    # self-contained preview: inline every asset as a data URI
    def datauri(slug, ext):
        b = base64.b64encode((FIGS / f"{slug}-{ext}").read_bytes()).decode()
        mime = "image/svg+xml" if ext.endswith("svg") else "image/png"
        return f"data:{mime};base64,{b}"
    pathlib.Path(PREVIEW).write_text(
        page("cinderplot gallery — preview", "gallery",
             gbody.replace("__SECTIONS__", sections(datauri))))
    print(f"wrote docs/index.html, docs/gallery.html, and {PREVIEW}")

USAGE = """usage: python3 docs/build.py [FIGURE ...] [--all] [--html]

  (no args)     render only figures whose output is MISSING, then rebuild HTML
  FIGURE ...    force-render just these figure slugs, then rebuild HTML
  --all         force-render every figure, then rebuild HTML
  --html        rebuild HTML only (no figure rendering)

  Editing one figure? re-render just it:  python3 docs/build.py k562cnv
"""

if __name__ == "__main__":
    import sys
    args = sys.argv[1:]
    if "-h" in args or "--help" in args:
        print(USAGE); raise SystemExit(0)
    html_only = "--html" in args
    force = "--all" in args
    slugs = [a for a in args if not a.startswith("-")]
    known = {v[0] for v in VARIANTS}
    unknown = [s for s in slugs if s not in known]
    if unknown:
        raise SystemExit(f"unknown figure slug(s): {', '.join(unknown)}\n"
                         f"known: {', '.join(sorted(known))}")
    if not html_only:
        render(only=slugs or None, force=force)
    build_html()
