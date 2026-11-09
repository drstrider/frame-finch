// Frame Finch: turns a screen recording (plus optional touches and log lines) into one HTML report
// that says, to the millisecond, when the screen changed, where, and how (scroll or repaint).
//
// The core is plain C99 with no dependencies. It never opens a video file itself: a front end
// decodes the video (main.c pipes it through ffmpeg; an Android app would use MediaCodec) and
// hands the frames over one by one.
//
// How a front end drives it:
//
//   1. ff_new             say how big the frames will be
//   2. ff_frame           every frame, in order, small (360 px wide is plenty for the analysis)
//      ff_touch, ff_mark  touches and log lines, whenever they are known
//      ff_clock_hint      what the file says about when it was recorded
//   3. ff_finish          works out the clock, the bursts of change and which frames to show
//   4. ff_picture         decode again, larger, and hand over the frames ff_is_key asks for
//   5. ff_write_html      the report
//   6. ff_free
//
// Times are video milliseconds (each frame's presentation time). Coordinates are the video's own pixels.

#ifndef FRAME_FINCH_H
#define FRAME_FINCH_H

#include <stdio.h>

typedef struct ff ff;

// w,h: the size of the frames fed to ff_frame (the analysis size); src_w,src_h: the video's own size.
ff *ff_new(int w, int h, int src_w, int src_h);
void ff_free(ff *f);

// One decoded frame, RGB24, w*h*3 bytes, in presentation order.
void ff_frame(ff *f, double t_ms, const unsigned char *rgb);

// A touch: kind 'd' down, 'm' move, 'u' up. wall=0: t is video ms; wall=1: t is epoch ms,
// placed on the video's timeline once the clock is known (in ff_finish).
void ff_touch(ff *f, double t, int wall, char kind, int x, int y);

// A line from an app log, placed on the timeline (same t/wall rule).
void ff_mark(ff *f, double t, int wall, const char *text);

// What the front end knows about the wall clock: the file's creation time (whole seconds, epoch),
// whether that is the end of the recording (Samsung's recorder) or the start, and the zone offset.
// Call before ff_finish. If an on-screen clock showing seconds is found, it fixes the milliseconds;
// the creation time only picks the second.
void ff_clock_hint(ff *f, long long creation_s, int creation_is_end, int utc_offset_s);

// Overrides everything: video time 0 is wall time wall0_ms (epoch ms).
void ff_set_wall(ff *f, double wall0_ms, const char *how);

void ff_finish(ff *f, double duration_ms);

// Key frames go into the report as pictures. After ff_finish, decode the video again (at any size;
// larger keeps small text readable) and hand over the frames ff_is_key asks for. Each becomes a
// 64-colour PNG, or a JPEG when it has too many shades for 64 colours.
int ff_is_key(const ff *f, int i);
void ff_picture(ff *f, int i, int w, int h, const unsigned char *rgb);

int ff_write_html(ff *f, FILE *out, const char *source_name);

#endif
