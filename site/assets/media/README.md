# Pictures and video for the site

Drop image and video files in this folder, then point at them from
`site/data/patches.js`. Nothing else is needed — no build, no resizing
step, no index to update.

## Naming

Use the feature's `id` from `patches.js` so it is obvious what a file
belongs to:

```
zoom-limit-before.jpg
zoom-limit-after.jpg
auto-market.jpg
endgame-stats.jpg
```

## What to capture, per kind of change

| Kind of change | Best medium | Why |
| --- | --- | --- |
| A visible difference in one still frame | before/after slider | The reader drags and sees it instantly |
| Something that happens over time | short YouTube clip | A crash or an animation cannot be a still |
| A keyboard or mouse shortcut | nothing, or one still | The text already says which key |
| Something invisible (a crash that no longer happens) | leave it empty | Two identical screenshots prove nothing |

A card with no media shows a tidy "screenshot on the way" panel, so the
page never looks broken while you are still capturing.

## Before/after pairs

Both shots must be taken **from the same camera position**, changing only
the one thing being demonstrated. Same resolution, same time of day, same
castle. If the two frames differ in any other way, the slider reads as two
unrelated pictures rather than one change.

Good candidates in this patch:

- `zoom-limit` — the stock zoom-out limit vs. `ZoomOutLimit=Auto`
- `ui-scale` — a clipped panel vs. one that fits
- `in-progress-lobbies` — the game list before and after the filtering

## Format and size

- **Images**: JPEG, 1600px wide is plenty. Keep each under ~400 KB.
- **Video**: upload to YouTube and reference the id. Do not commit video
  files — the repository is for source, and Pages is not a video host.

To get a YouTube id, take the part after `v=` in the address bar:
`https://www.youtube.com/watch?v=dQw4w9WgXcQ` → `dQw4w9WgXcQ`
