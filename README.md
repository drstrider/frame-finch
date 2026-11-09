# Frame Finch 🐦

Turns a screen recording into one HTML report that says, to the millisecond, when the screen changed, where, and how,
so an agent can measure lag ("the tap at 37.817 got a screen 359 ms later, the history 10.35 s later") instead of guessing.

## Install

    curl -fsSL drstrider.github.io/frame-finch/install.sh | sh

That puts a prebuilt `frame-finch` in `~/.local/bin` (Linux x86_64/arm64, macOS on Apple silicon), the agent skill in
`~/.claude/skills/frame-finch`, and on Linux a static ffmpeg if you don't have one. Or build it: `make` (any C99 compiler; needs ffmpeg at run time).

## Use

    frame-finch VIDEO.mp4 [--log app.log] [--touches touches.txt] [--start HH:MM:SS.mmm] [--full] [-o OUT.html]

Or tell your agent "use frame-finch on this video".

- **In:** a video (anything ffmpeg can read; recorders that only write a frame when the screen changes are best, since a frame
  then means "the screen changed"), optionally an app log whose lines start `HH:MM:SS.mmm` and a touch list (`TIME down|move|up X Y`).
- **Out:** one self-contained `.finch.html`. The plain-text report is at the top (an agent can read the file directly), every
  frame's change boxes are in a JSON block (`<script id="data">`), and key frames are embedded (`<img id="fN">`) with the boxes drawn over them:
  540 px wide 64-colour PNGs, which keep text sharp and small, or a JPEG for a frame with too many shades (photos, video, gradients).
  `--full` embeds them at the video's own size.
- **Clock:** a small box that changes once a second (for example a status-line clock in the app) is found by itself; the frames where it ticks over
  give the phone time in milliseconds, and the file's creation time picks the second (Samsung stamps the end of recording).
  Other steady blinkers (a cursor) are left out of the changes.

## How it works

`src/ff.c` is the C99 core (no dependencies besides `stb_image_write.h`, public domain). Its opening comment walks through the
steps: frame differences in a grid of cells, boxes and scrolls, blinkers and the clock, bursts, the pictures, the report.
`src/main.c` is the desktop front end: it has no video decoder of its own, ffprobe and ffmpeg do the decoding through pipes.
The core takes frames, not files, so it can be linked into an Android app and fed by MediaCodec.

Releases: pushing a tag `vX.Y.Z` makes GitHub build static Linux binaries (x86_64, arm64) and a macOS arm64 one and attach them,
with `install.sh` and the skill, to the release (`.github/workflows/release.yml`).
