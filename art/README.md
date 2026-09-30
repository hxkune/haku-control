# haku control artwork

The haku name and logo are not covered by the project's license (see the main README).

- **Mark** (the h inside the C-shaped orbit): `ui/mark.svg` is the one source of its shape. The h was drawn by hand,
  the orbit laid through it, and the final touches (the torn ends where they cross) were done in After Effects;
  `haku-mark-source.png` is that final picture, and `ui/mark.svg` is its outline traced into curves.
- **Wordmark** (the word haku in its orbit): `ui/haku.svg`, traced from `haku-logo-source.webp` and cleaned up.
- **Icons**: `powershell -File art\gen-icons.ps1 res` renders `res/app.ico`, `res/tray-dark.ico`, `res/tray-light.ico`
  and `icon-256.png` (copy it to `ui/`) from `ui/mark.svg`, bolder at the small sizes.
