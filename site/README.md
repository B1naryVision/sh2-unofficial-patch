# Player site

The public-facing site for the patch, aimed at players rather than
developers: what it fixes, in plain English, and how to install it.

Static HTML, CSS and JavaScript. **No build step, no dependencies, no
Node.** Edit a file, save it, push it.

```
site/
  index.html            the page itself (structure only)
  .nojekyll             tells Pages to serve the files as-is
  data/patches.js       ALL the content -- this is the file you edit
  assets/css/site.css   the design system, ported from the tournament site
  assets/js/site.js     rendering: cards, filters, modal, slider
  assets/media/         screenshots and before/after pairs (see its README)
```

## Editing the content

Everything a visitor reads lives in [`data/patches.js`](data/patches.js),
in five lists:

| List | What it drives |
| --- | --- |
| `SITE` | Repository links, fallback version, optional hero image and install video |
| `PATCHES` | The feature cards. One object per fix or feature |
| `HISTORY` | The version history at the bottom of the page |
| `SETTINGS` | The plain-English settings table |
| `FAQ` | The questions section |

Adding a new fix is one object appended to `PATCHES`. The file's header
comment documents every field, including how to attach a screenshot, a
before/after slider or a YouTube clip.

### Keeping it in step with the changelog

`CHANGELOG.md` at the repository root is the source of truth for release
notes; `HISTORY` here is the player-facing echo of it. When you add a
changelog entry, add the matching line to `HISTORY`, and a card to
`PATCHES` if it is something a player would look for by name.

The two are deliberately not the same shape: the changelog is a complete
record in release order, while `PATCHES` describes the game **as it is
today** — several changelog entries about the statistics screen collapse
into one card, because a player wants to know what the feature does now,
not how it got there.

## Previewing locally

Double-clicking `index.html` mostly works. Two things do not, and neither
is a fault in the page:

- **YouTube embeds** refuse to load from `file://` (no origin), so the
  page offers an "Open on YouTube" link instead.
- **The live version number** is read from the GitHub API, which `file://`
  cannot call. `SITE.fallbackVer` is shown instead.

To see the real thing, serve the folder:

```bash
cd site && python3 -m http.server 8000
# then open http://localhost:8000
```

## Deployment

Pushing to `master` with any change under `site/` triggers
[`.github/workflows/pages.yml`](../.github/workflows/pages.yml), which
uploads this folder to GitHub Pages.

**One-time setup:** repository *Settings → Pages → Source =
**GitHub Actions***. Not "Deploy from a branch" — that mode can only
serve the repository root or `/docs`, and `/docs` holds the developer
write-ups.

## Design

Colours, type and components are ported from the Hidden Cup tournament
site so the two properties read as one family. Two themes ship: `night`
(default) and `parchment`, toggled in the header and remembered per
visitor. Every colour is a custom property defined in both themes at the
top of `site.css` — never hard-code one in a component.
