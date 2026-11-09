---
name: frame-finch
description: Measure UI lag from a screen recording - when the screen changed after a tap, how long a load or scroll took, flicker. Use when the user says "use frame-finch", sends a screen recording of a slow or glitchy UI, or asks how long something on screen took.
---

# Frame Finch

`frame-finch VIDEO.mp4 [--log APP.log] [--touches FILE] [--start HH:MM:SS.mmm] [-o OUT.html]` (source: `~/coding/frame-finch`, `make` to rebuild)
writes one self-contained `VIDEO.finch.html`.

1. Run it. If the app writes a log with `HH:MM:SS.mmm` at the start of each line, pass it with `--log APP.log` (its lines land on the timeline in phone time).
2. Read the `<pre id="report">` block at the top of the output: the clock line, then the TIMELINE of CHANGE bursts (start, how long the screen was still before, where, steps with % of screen changed, frame numbers), TOUCH and LOG lines.
3. Look at the frames that matter before concluding what they show. The report prints a one-line python command that pulls `fN` out as a JPEG. Key frames are embedded 540 px wide (rerun with `--full` when small print matters); for any other frame, `ffmpeg -i VIDEO -vf "select=eq(n\,N)" -frames:v 1 fN.png`.
4. Answer in milliseconds, naming the frames: "tap ripple at f98 (37.817), chat screen f123 (+359 ms), history f173 (+10.35 s)".

Notes:
- A frame is only recorded when the screen changes (Samsung/Android recorders), so "still for N ms before" is real stillness.
- Times are frame capture times; read a change as +0..17 ms. Phone time comes from an on-screen clock showing seconds, if the app has one (about ±25 ms), and the file's creation time; without one, use `--start` or read times as video ms.
- Without a touch log, the touch is the first small change at the tapped control (a ripple or highlight); say so. In other apps, turn on Developer options → "Show taps" before recording so every touch shows as a dot.
- Touch list format for `--touches`: one per line, `TIME down|move|up X Y`; TIME is `HH:MM:SS.mmm` phone time, epoch ms, or video ms written `v1234.5`.
