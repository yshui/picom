# Build and test Wobbly Windows

Wobbly Windows is an opt-in effect for picom's `glx` and `egl` backends. It
deforms a window as its position or size changes, then lets it settle. You can
enable it with `--wobbly-windows` or `wobbly-windows = true;` in a picom config.

## Tune the effect

Five Wobbly Windows presets are available through
`--wobbly-windows-level 0` through `--wobbly-windows-level 4` (or
`wobbly-windows-level = 0;` through `4;` in a config). Level 0 is the default
and matches this port's original spring values. Higher levels wobble more.

For custom tuning, add values to your picom config, for example:

```conf
wobbly-windows = true;
wobbly-windows-level = 0;
wobbly-windows-stiffness = 0.15;
wobbly-windows-drag = 0.80;
wobbly-windows-move-factor = 0.10;
wobbly-windows-x-tessellation = 20;
wobbly-windows-y-tessellation = 20;
```

Stiffness, drag, and move factor override the selected preset individually.
The equivalent CLI flags use the same names with a `--` prefix. You can also
set `wobbly-windows-move` and `wobbly-windows-resize` to `false` independently;
the CLI equivalents are `--no-wobbly-windows-move` and
`--no-wobbly-windows-resize`. Use the matching positive CLI flags to enable
either one again.

For finer tuning, `wobbly-windows-min-velocity`,
`wobbly-windows-max-velocity`, and `wobbly-windows-stop-velocity` control the
velocity dead zone, limit, and settle threshold. The corresponding
`min-acceleration`, `max-acceleration`, and `stop-acceleration` settings do
the same for acceleration. The default values are 0, 1000, and 0.5 for each.
Mesh spacing defaults to 20 pixels; smaller spacing draws more geometry and
uses more GPU time.
