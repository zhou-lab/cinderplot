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

Run from anywhere:  python3 docs/build.py
"""
import base64, html, os, pathlib, subprocess

HERE     = pathlib.Path(__file__).resolve().parent      # <repo>/docs
REPO     = HERE.parent                                  # <repo>
FIGS     = HERE / "figs"
EXAMPLES = pathlib.Path(os.environ.get(
    "CINDERPLOT_EXAMPLES", REPO.parent / "cinderplot-examples")).resolve()
BIN      = os.environ.get("CINDERPLOT", str(REPO / "cinderplot"))
PREVIEW  = os.environ.get("CINDERPLOT_PREVIEW", str(HERE / "_preview.html"))

# slug, title, cinderplot DSL expr, ggplot2 expression (runs *and* displays)
VARIANTS = [
    ("basic", "Basic",
     "data/mtcars.csv + aes(wt, mpg) + geom_point()",
     "ggplot(mtcars, aes(wt, mpg)) + geom_point()"),
    ("custom", "Custom colour",
     'data/mtcars.csv + aes(wt, mpg) + geom_point(color="#69b3a2")',
     'ggplot(mtcars, aes(wt, mpg)) + geom_point(color = "#69b3a2")'),
    ("group-cyl", "Colour by group",
     "data/mtcars.csv + aes(wt, mpg, colour=factor(cyl)) + geom_point()",
     "ggplot(mtcars, aes(wt, mpg, colour = factor(cyl))) + geom_point()"),
    ("group-gear", "Groups, other axes",
     "data/mtcars.csv + aes(hp, mpg, colour=factor(gear)) + geom_point()",
     "ggplot(mtcars, aes(hp, mpg, colour = factor(gear))) + geom_point()"),
    ("continuous", "Continuous colour",
     'data/mtcars.csv + aes(wt, mpg, colour=hp) + geom_point() + scale_colour_gradient(low="#132b43", high="#56b1f7")',
     'ggplot(mtcars, aes(wt, mpg, colour = hp)) + geom_point() +\n  scale_colour_gradient(low = "#132b43", high = "#56b1f7")'),
    ("logx", "Log x-axis",
     "data/mtcars.csv + aes(hp, mpg) + geom_point() + scale_x_log10()",
     "ggplot(mtcars, aes(hp, mpg)) + geom_point() + scale_x_log10()"),
    ("iris-sepal", "Iris · sepals",
     "data/iris.csv + aes(Sepal.Length, Sepal.Width, colour=Species) + geom_point()",
     "ggplot(iris, aes(Sepal.Length, Sepal.Width, colour = Species)) + geom_point()"),
    ("iris-petal", "Iris · petals",
     "data/iris.csv + aes(Petal.Length, Petal.Width, colour=Species) + geom_point()",
     "ggplot(iris, aes(Petal.Length, Petal.Width, colour = Species)) + geom_point()"),
    ("faithful", "Old Faithful",
     "data/faithful.csv + aes(waiting, eruptions) + geom_point()",
     "ggplot(faithful, aes(waiting, eruptions)) + geom_point()"),
    ("quakes", "Fiji earthquakes",
     'data/quakes.csv + aes(long, lat, colour=mag) + geom_point() + scale_colour_gradient(low="#132b43", high="#56b1f7")',
     'ggplot(quakes, aes(long, lat, colour = mag)) + geom_point() +\n  scale_colour_gradient(low = "#132b43", high = "#56b1f7")'),
    ("diamonds", "Diamonds (2k sample)",
     "data/diamonds_sample.csv + aes(carat, price, colour=cut) + geom_point()",
     "# diamonds, a 2000-row sample\nggplot(diamonds, aes(carat, price, colour = cut)) + geom_point()"),
    ("cars", "Cars: speed vs dist",
     "data/cars.csv + aes(speed, dist) + geom_point()",
     "ggplot(cars, aes(speed, dist)) + geom_point()"),
]

# R datasets to export to <examples>/data/ so cinderplot and ggplot read
# identical rows. varname (used in ggplot code), csv path, R expr (None = exists)
DATASETS = [
    ("mtcars",   "data/mtcars.csv",          None),
    ("iris",     "data/iris.csv",            None),
    ("faithful", "data/faithful.csv",        "faithful"),
    ("cars",     "data/cars.csv",            "cars"),
    ("quakes",   "data/quakes.csv",          "quakes"),
    ("diamonds", "data/diamonds_sample.csv", "{set.seed(42); diamonds[sample(nrow(diamonds), 2000), ]}"),
]

def render():
    if not EXAMPLES.is_dir():
        raise SystemExit(f"example datasets not found at {EXAMPLES}\n"
                         f"clone zhou-lab/cinderplot-examples beside this repo, "
                         f"or set CINDERPLOT_EXAMPLES.")
    FIGS.mkdir(parents=True, exist_ok=True)
    tmp = HERE / "_gen"
    tmp.mkdir(exist_ok=True)
    # 0. export any missing R datasets to <examples>/data/
    exp = ["suppressMessages(library(ggplot2))"]
    for _v, csv, expr in DATASETS:
        if expr and not (EXAMPLES / csv).exists():
            exp.append(f"write.csv({expr}, '{csv}', row.names=FALSE)")
    (tmp / "export.R").write_text("\n".join(exp) + "\n")
    subprocess.run(["Rscript", str(tmp / "export.R")], cwd=EXAMPLES, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # 1. ggplot references via one R script (pdf device; no cairo/X11 needed)
    r = ["suppressMessages(library(ggplot2))"]
    for var, csv, _e in DATASETS:
        r.append(f"{var} <- read.csv('{csv}')")
    for slug, _t, _cp, rc in VARIANTS:
        r.append(f"ggsave('{FIGS / (slug + '-gg.pdf')}', {rc}, width=6, height=4)")
    (tmp / "gen.R").write_text("\n".join(r) + "\n")
    subprocess.run(["Rscript", str(tmp / "gen.R")], cwd=EXAMPLES, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # 2. rasterise ggplot PDFs, render cinderplot SVGs (both read <examples>/data)
    for slug, _t, cp, _rc in VARIANTS:
        subprocess.run(["pdftoppm", "-r", "150", "-png", "-singlefile",
                        str(FIGS / f"{slug}-gg.pdf"), str(FIGS / f"{slug}-gg")],
                       check=True)
        (FIGS / f"{slug}-gg.pdf").unlink(missing_ok=True)   # keep only PNG
        subprocess.run([BIN, cp, "-o", str(FIGS / f"{slug}-cp.svg")],
                       cwd=EXAMPLES, check=True, stdout=subprocess.DEVNULL)

def esc(s):
    return html.escape(s)

def cards(src):
    """src(slug, ext) -> the value for an <img src>. Returns cards HTML."""
    out = []
    for slug, title, cp, rc in VARIANTS:
        cp_cmd = "cinderplot '" + cp.replace(" + ", "\n  + ") + "' \\\n  -o out.svg"
        out.append(f'''      <figure class="card">
        <div class="stage" tabindex="0" aria-label="{esc(title)} scatterplot; hover or focus for code">
          <img data-v="cp" src="{src(slug,'cp.svg')}" alt="{esc(title)} — cinderplot">
          <img data-v="r" src="{src(slug,'gg.png')}" alt="{esc(title)} — ggplot2" hidden>
          <div class="overlay">
            <pre data-v="cp"><span class="c"># cinderplot</span>
{esc(cp_cmd)}</pre>
            <pre data-v="r"><span class="c"># R + ggplot2</span>
{esc(rc)}</pre>
          </div>
        </div>
        <figcaption>{esc(title)}</figcaption>
      </figure>''')
    return "\n".join(out)

STYLE = """<style>
  :root {
    --ground:#FBFAF8; --card:#F4F0EA; --ink:#221D1A; --muted:#6E645D;
    --line:#E7E1D9; --ember:#D6532A; --ember-soft:rgba(214,83,42,.12);
    --code-ink:#EFE9E2; --code-dim:#C9A98F;
    --sans: system-ui,-apple-system,"Segoe UI",Helvetica,Arial,sans-serif;
    --mono: ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  }
  @media (prefers-color-scheme: dark) { :root {
    --ground:#16120F; --card:#201A16; --ink:#F1EBE3; --muted:#A89C93;
    --line:#302822; --ember:#F0693B; --ember-soft:rgba(240,105,59,.16); --code-dim:#C9A98F; } }
  :root[data-theme="light"] {
    --ground:#FBFAF8; --card:#F4F0EA; --ink:#221D1A; --muted:#6E645D;
    --line:#E7E1D9; --ember:#D6532A; --ember-soft:rgba(214,83,42,.12); --code-dim:#C9A98F; }
  :root[data-theme="dark"] {
    --ground:#16120F; --card:#201A16; --ink:#F1EBE3; --muted:#A89C93;
    --line:#302822; --ember:#F0693B; --ember-soft:rgba(240,105,59,.16); --code-dim:#C9A98F; }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--ground); color:var(--ink); font-family:var(--sans);
         font-size:16px; line-height:1.6; -webkit-font-smoothing:antialiased; }
  .wrap { max-width:1080px; margin:0 auto; padding:0 24px; }
  header.site { border-bottom:1px solid var(--line); }
  header.site .wrap { display:flex; align-items:center; gap:10px; height:56px; }
  .mark { font-family:var(--mono); font-weight:600; font-size:15px; display:flex; align-items:center; gap:8px;
          color:var(--ink); text-decoration:none; }
  .ember { width:10px; height:10px; border-radius:2px; background:var(--ember); box-shadow:0 0 10px var(--ember); }
  .mark .sub { color:var(--muted); font-weight:400; }
  .nav { margin-left:auto; display:flex; gap:20px; font-family:var(--mono); font-size:13px; }
  .nav a { color:var(--muted); text-decoration:none; }
  .nav a:hover, .nav a.active { color:var(--ember); }
  main { padding:44px 0 96px; }
  .eyebrow { font-family:var(--mono); font-size:12px; letter-spacing:.14em; text-transform:uppercase;
             color:var(--ember); margin:0 0 14px; }
  h1 { font-size:38px; line-height:1.08; letter-spacing:-.025em; margin:0 0 14px; text-wrap:balance; }
  .lede { color:var(--muted); font-size:19px; line-height:1.5; margin:0 0 26px; max-width:66ch; }
  .lede b { color:var(--ink); font-weight:600; }
  .toolbar { display:flex; align-items:center; gap:12px; margin:0 0 22px; }
  .toolbar .lbl { font-family:var(--mono); font-size:12px; letter-spacing:.03em; color:var(--muted); }
  .tabs { display:inline-flex; gap:3px; padding:3px; background:var(--card);
          border:1px solid var(--line); border-radius:9px; }
  .tab { font-family:var(--mono); font-size:12px; color:var(--muted); background:transparent; border:0;
         padding:5px 13px; border-radius:6px; cursor:pointer; transition:background .15s ease, color .15s ease; }
  .tab:hover { color:var(--ink); }
  .tab.active { background:var(--ember); color:#fff; }
  .cards { display:grid; grid-template-columns:repeat(4,1fr); gap:16px; }
  @media (max-width:900px){ .cards{ grid-template-columns:repeat(2,1fr); } }
  @media (max-width:520px){ .cards{ grid-template-columns:1fr; } }
  .card { margin:0; border:1px solid var(--line); border-radius:12px; overflow:hidden; background:var(--card);
          transition:border-color .18s ease, transform .18s ease; }
  .card:hover { border-color:var(--ember); transform:translateY(-2px); }
  .stage { position:relative; background:#fff; outline:none; }
  .stage img { display:block; width:100%; height:auto; }
  .stage img[hidden] { display:none; }
  .overlay { position:absolute; inset:0; padding:12px; background:rgba(24,18,14,.9);
             opacity:0; transition:opacity .16s ease; pointer-events:none;
             display:flex; align-items:center; }
  .stage:hover .overlay, .stage:focus-visible .overlay { opacity:1; pointer-events:auto; }
  .overlay pre { margin:0; width:100%; color:var(--code-ink); font-family:var(--mono);
                 font-size:10.5px; line-height:1.5; white-space:pre-wrap; word-break:break-word; }
  .overlay pre[hidden] { display:none; }
  .overlay .c { color:var(--code-dim); }
  figcaption { font-family:var(--mono); font-size:12px; color:var(--ink); padding:9px 12px;
               border-top:1px solid var(--line); background:var(--ground); }
  .note { border-left:3px solid var(--ember); background:var(--card); padding:14px 18px;
          border-radius:0 10px 10px 0; margin:34px 0 0; }
  .note b { color:var(--ember); }
  .hero-cta { display:flex; gap:12px; flex-wrap:wrap; margin:6px 0 8px; }
  .btn { font-family:var(--mono); font-size:13px; text-decoration:none; padding:9px 16px; border-radius:8px;
         border:1px solid var(--line); color:var(--ink); background:var(--card); transition:border-color .15s ease; }
  .btn:hover { border-color:var(--ember); }
  .btn.primary { background:var(--ember); color:#fff; border-color:var(--ember); }
  pre.code { background:#16120F; color:var(--code-ink); font-family:var(--mono); font-size:13px;
             padding:16px 18px; border-radius:10px; overflow:auto; margin:22px 0; }
  pre.code .c { color:var(--code-dim); }
  .feature-grid { display:grid; grid-template-columns:repeat(3,1fr); gap:16px; margin:30px 0 0; }
  @media (max-width:760px){ .feature-grid{ grid-template-columns:1fr; } }
  .feature { border:1px solid var(--line); border-radius:12px; padding:18px 20px; background:var(--card); }
  .feature h3 { margin:0 0 6px; font-size:15px; }
  .feature p { margin:0; color:var(--muted); font-size:14px; }
  footer { color:var(--muted); font-family:var(--mono); font-size:12.5px; border-top:1px solid var(--line);
           margin-top:40px; padding-top:20px; }
  footer a { color:var(--ember); text-decoration:none; }
  .stage:focus-visible { outline:2px solid var(--ember); outline-offset:-2px; }
  @media (prefers-reduced-motion: reduce){ .card{ transition:none; } .card:hover{ transform:none; } }
</style>"""

def header(active):
    def link(href, label):
        cls = ' class="active"' if label.lower() == active else ""
        return f'<a href="{href}"{cls}>{label}</a>'
    return f"""<header class="site">
  <div class="wrap">
    <a class="mark" href="index.html"><span class="ember"></span>cinderplot</a>
    <nav class="nav">{link("index.html","Home")}{link("gallery.html","Gallery")}<a href="https://github.com/zhou-lab/cinderplot">GitHub</a></nav>
  </div>
</header>"""

GALLERY_BODY = """
<main>
  <div class="wrap">
    <p class="eyebrow">Gallery › Scatterplot</p>
    <h1>Scatterplot</h1>
    <p class="lede">
      The scatterplot, drawn __N__ ways — grouped, coloured, log-scaled, across datasets. Every figure is
      rendered by <b>cinderplot</b> straight from a CSV, no R at plot time. <b>Hover any plot for the command;</b>
      flip the toggle to see the R original it reproduces.
    </p>
    <div class="toolbar">
      <span class="lbl">drawn with</span>
      <div class="tabs" role="tablist" aria-label="Drawn with">
        <button class="tab active" data-t="cp" role="tab" aria-selected="true">cinderplot</button>
        <button class="tab" data-t="r" role="tab" aria-selected="false">R · ggplot2</button>
      </div>
    </div>
    <div class="cards">
__CARDS__
    </div>
    <div class="note"><b>Same figures, no R.</b> cinderplot reproduces ggplot2's <code>theme_gray</code> —
      panel, gridlines, hue palette, axis breaks, legends — from one C binary that reads the CSV and writes
      PDF or SVG in milliseconds.</div>
    <footer>data: cinderplot-examples/data &nbsp;·&nbsp; rebuild: docs/build.py &nbsp;·&nbsp;
      <a href="https://github.com/zhou-lab/cinderplot">github.com/zhou-lab/cinderplot</a></footer>
  </div>
</main>
<script>
(function () {
  var tabs = [].slice.call(document.querySelectorAll('.tab'));
  var items = [].slice.call(document.querySelectorAll('[data-v]'));
  function set(v) {
    items.forEach(function (el) { el.hidden = (el.dataset.v !== v); });
    tabs.forEach(function (b) { var on = b.dataset.t === v;
      b.classList.toggle('active', on); b.setAttribute('aria-selected', on ? 'true' : 'false'); });
  }
  tabs.forEach(function (b) { b.addEventListener('click', function () { set(b.dataset.t); }); });
})();
</script>"""

INDEX_BODY = """
<main>
  <div class="wrap">
    <p class="eyebrow">Grammar-inspired plotting in C</p>
    <h1>Publication-ready graphics<br>from one small, fast binary.</h1>
    <p class="lede">
      <b>cinderplot</b> turns a CSV into a PDF or SVG with a ggplot2-inspired grammar — panels, hue palettes,
      scales, legends — rendered by a single C binary with Cairo. No R, no Python, no runtime at plot time.
    </p>
    <div class="hero-cta">
      <a class="btn primary" href="gallery.html">See the gallery</a>
      <a class="btn" href="https://github.com/zhou-lab/cinderplot">GitHub</a>
    </div>
    <pre class="code"><span class="c"># install from the zhou-lab conda channel</span>
conda install -c zhou-lab cinderplot

<span class="c"># plot straight from a CSV</span>
cinderplot 'data.csv + aes(hp, mpg, colour=factor(cyl)) + geom_point()' -o plot.pdf</pre>
    <div class="feature-grid">
      <div class="feature"><h3>Small &amp; fast</h3><p>One C11 binary, Cairo for output. Milliseconds per figure, minimal dependencies.</p></div>
      <div class="feature"><h3>Familiar grammar</h3><p>aes, geoms, scales, and legends inspired by ggplot2 — reproduces <code>theme_gray</code> closely.</p></div>
      <div class="feature"><h3>PDF &amp; SVG</h3><p>Vector output chosen by file extension. Publication-ready, no rasterisation.</p></div>
    </div>
    <footer>rebuild this site: docs/build.py &nbsp;·&nbsp;
      <a href="https://github.com/zhou-lab/cinderplot">github.com/zhou-lab/cinderplot</a></footer>
  </div>
</main>"""

def page(title, active, body):
    return ("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            f"<title>{esc(title)}</title>\n" + STYLE + "\n</head>\n<body>\n"
            + header(active) + body + "\n</body>\n</html>\n")

def build_html():
    n_words = {5:"five",6:"six",7:"seven",8:"eight",9:"nine",10:"ten",
               11:"eleven",12:"a dozen"}.get(len(VARIANTS), str(len(VARIANTS)))
    gbody = GALLERY_BODY.replace("__N__", n_words)
    # hosted pages: relative asset paths
    (HERE / "index.html").write_text(page("cinderplot — fast grammar plotting in C", "home", INDEX_BODY))
    (HERE / "gallery.html").write_text(
        page("Scatterplot — cinderplot gallery", "gallery",
             gbody.replace("__CARDS__", cards(lambda s, e: f"figs/{s}-{e}"))))
    # self-contained preview: inline every asset as a data URI
    def datauri(slug, ext):
        b = base64.b64encode((FIGS / f"{slug}-{ext}").read_bytes()).decode()
        mime = "image/svg+xml" if ext.endswith("svg") else "image/png"
        return f"data:{mime};base64,{b}"
    pathlib.Path(PREVIEW).write_text(
        page("cinderplot gallery — preview", "gallery",
             gbody.replace("__CARDS__", cards(datauri))))
    print(f"wrote docs/index.html, docs/gallery.html, and {PREVIEW}")

if __name__ == "__main__":
    render()
    build_html()
