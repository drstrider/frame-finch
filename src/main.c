// frame-finch: the desktop front end. It has no video decoder of its own: ffprobe reports the video's
// size, length, creation time and every frame's timestamp, and ffmpeg decodes the frames and pipes them
// in as raw RGB. The core (ff.c) does the rest.
//
//   frame-finch VIDEO [-o OUT.html] [--touches FILE] [--log FILE] [--start HH:MM:SS.mmm] [--full | --pic-width N]
//
// --touches: one touch per line, "TIME down|move|up X Y"; TIME is HH:MM:SS.mmm (phone time), epoch ms, or video ms ("v1234.5").
// --log: any log whose lines start with HH:MM:SS.mmm (an app's own debug log); lines inside the video's time span go on the timeline.
// --start: the phone time at video 0, when you know it better than the file does.
// Key frame pictures are 540 px wide (--pic-width N, or --full for the video's own size); the analysis runs at 360.
//
// The steps: probe the file, decode it small and feed every frame to the core, add touches and log
// lines, let the core finish, decode it again larger for the key frame pictures, write the report.

#define _POSIX_C_SOURCE 200809L
#include "ff.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define USAGE "usage: frame-finch VIDEO [-o OUT.html] [--touches FILE] [--log FILE] [--start HH:MM:SS.mmm] [--full | --pic-width N]"

static void die(const char *m)
{
    fprintf(stderr, "frame-finch: %s\n", m);
    exit(1);
}

// Single-quotes a path for the shell, so any file name is safe inside the ffmpeg command line.
static void quote(char *out, size_t n, const char *s)
{
    size_t k = 0;
    out[k++] = '\'';
    for (; *s && k + 5 < n; s++)
    {
        if (*s == '\'')
        {
            memcpy(out + k, "'\\''", 4);
            k += 4;
        }
        else
        {
            out[k++] = *s;
        }
    }
    out[k++] = '\'';
    out[k] = 0;
}

// Days since 1970-01-01 for a calendar date (Howard Hinnant's algorithm), to read creation_time as UTC.
static long long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

// "HH:MM:SS.mmm" as phone-local time on the day nearest to near_ms (epoch ms). Logs carry no date,
// so the day is the one that puts the time closest to the recording. Returns epoch ms, or -1.
static double hms_to_epoch(const char *s, double near_ms, int utc_off)
{
    int h, m;
    double sec;
    if (sscanf(s, "%d:%d:%lf", &h, &m, &sec) != 3)
        return -1;
    double local_near = near_ms + utc_off * 1000.0;
    double day0 = floor(local_near / 86400000.0) * 86400000.0;
    double best = -1;
    for (int k = -1; k <= 1; k++)
    {
        double t = day0 + k * 86400000.0 + (h * 3600 + m * 60 + sec) * 1000 - utc_off * 1000.0;
        if (best < 0 || fabs(t - near_ms) < fabs(best - near_ms))
            best = t;
    }
    return best;
}

int main(int argc, char **argv)
{
    const char *video = NULL, *out = NULL, *touches = NULL, *log = NULL, *start = NULL;
    int aw = 360, pw = 540;     // analysis width, picture width
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--touches") && i + 1 < argc)
            touches = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            log = argv[++i];
        else if (!strcmp(argv[i], "--start") && i + 1 < argc)
            start = argv[++i];
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)
            aw = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pic-width") && i + 1 < argc)
            pw = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--full"))
            pw = 1 << 30;
        else if (argv[i][0] == '-')
            die(USAGE);
        else
            video = argv[i];
    }
    if (!video)
        die(USAGE);
    char q[4096], cmd[8192], line[4096];
    quote(q, sizeof q, video);

    // size, length, and when the file was made
    int sw = 0, sh = 0, utc_off = 0, have_off = 0;
    double dur = 0;
    long long created = 0;
    snprintf(cmd, sizeof cmd,
             "ffprobe -v error -select_streams v:0 -show_entries stream=width,height:format=duration:format_tags=creation_time,com.samsung.android.utc_offset -of default=nw=1 %s",
             q);
    FILE *p = popen(cmd, "r");
    if (!p)
        die("can't run ffprobe");
    while (fgets(line, sizeof line, p))
    {
        int Y, M, D, h, m, s;
        if (sscanf(line, "width=%d", &sw) == 1 || sscanf(line, "height=%d", &sh) == 1 || sscanf(line, "duration=%lf", &dur) == 1)
            continue;
        if (sscanf(line, "TAG:creation_time=%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6)
            created = days_from_civil(Y, M, D) * 86400 + h * 3600 + m * 60 + s;
        char sign;
        int oh, om;
        if (sscanf(line, "TAG:com.samsung.android.utc_offset=%c%2d%2d", &sign, &oh, &om) == 3)
        {
            utc_off = (sign == '-' ? -1 : 1) * (oh * 3600 + om * 60);
            have_off = 1;
        }
    }
    pclose(p);
    if (!sw || !sh)
        die("no video stream");
    if (!have_off)
    {
        // no zone in the file: assume the phone is in this computer's zone
        time_t now = time(NULL);
        struct tm lt = *localtime(&now);
        utc_off = (int)lt.tm_gmtoff;
    }
    int samsung = have_off;     // Samsung's recorder stamps the file when it finishes

    // every frame's time (a recorder that only writes a frame when the screen changes makes these uneven)
    snprintf(cmd, sizeof cmd, "ffprobe -v error -select_streams v:0 -show_entries frame=pts_time -of csv=p=0 %s", q);
    p = popen(cmd, "r");
    double *pts = NULL;
    int npts = 0, cap = 0;
    while (fgets(line, sizeof line, p))
    {
        if (npts == cap)
        {
            cap = cap ? cap * 2 : 1024;
            pts = realloc(pts, sizeof(double) * cap);
        }
        pts[npts++] = atof(line) * 1000;
    }
    pclose(p);
    if (!npts)
        die("no frames");

    int ah = (int)((double)sh * aw / sw / 2 + 0.5) * 2;
    ff *f = ff_new(aw, ah, sw, sh);
    if (created)
        ff_clock_hint(f, created, samsung, utc_off);
    double near = created ? created * 1000.0 : time(NULL) * 1000.0;
    if (start)
    {
        double w0 = hms_to_epoch(start, samsung ? near - dur * 1000 : near, utc_off);
        if (w0 < 0)
            die("--start wants HH:MM:SS.mmm");
        ff_set_wall(f, w0, "given with --start");
    }

    // first pass: every frame, small, for the analysis
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -i %s -vf scale=%d:%d:flags=area -fps_mode passthrough -f rawvideo -pix_fmt rgb24 -", q, aw, ah);
    p = popen(cmd, "r");
    size_t fs = (size_t)aw * ah * 3;
    unsigned char *rgb = malloc(fs);
    int n = 0;
    while (fread(rgb, 1, fs, p) == fs)
    {
        ff_frame(f, n < npts ? pts[n] : pts[npts - 1] + (n - npts + 1), rgb);
        n++;
    }
    pclose(p);
    if (n != npts)
        fprintf(stderr, "frame-finch: %d frames decoded, %d timestamps\n", n, npts);

    if (touches)
    {
        FILE *t = fopen(touches, "r");
        if (!t)
            die("can't read --touches");
        while (fgets(line, sizeof line, t))
        {
            char ts[64], kind[16];
            int x, y;
            if (sscanf(line, "%63s %15s %d %d", ts, kind, &x, &y) != 4)
                continue;
            char k = kind[0] == 'd' ? 'd' : kind[0] == 'u' ? 'u' : 'm';
            if (ts[0] == 'v')
                ff_touch(f, atof(ts + 1), 0, k, x, y);
            else if (strchr(ts, ':'))
                ff_touch(f, hms_to_epoch(ts, near, utc_off), 1, k, x, y);
            else
                ff_touch(f, atof(ts), 1, k, x, y);
        }
        fclose(t);
    }
    if (log)
    {
        FILE *l = fopen(log, "r");
        if (!l)
            die("can't read --log");
        while (fgets(line, sizeof line, l))
        {
            int h, m, s, ms;
            if (sscanf(line, "%2d:%2d:%2d.%3d", &h, &m, &s, &ms) != 4)
                continue;
            line[strcspn(line, "\r\n")] = 0;
            char *text = line + 12;
            while (*text == ' ')
                text++;
            if (strlen(text) > 300)
            {
                // shorten long lines without cutting a UTF-8 character in half
                int cut = 297;
                while (cut > 0 && (text[cut] & 0xC0) == 0x80)
                    cut--;
                strcpy(text + cut, "...");
            }
            ff_mark(f, hms_to_epoch(line, near, utc_off), 1, text);
        }
        fclose(l);
    }
    ff_finish(f, dur * 1000);

    // second pass: the key frames, bigger, so small text stays readable
    if (pw > sw)
        pw = sw;
    int ph = (int)((double)sh * pw / sw / 2 + 0.5) * 2;
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -i %s -vf scale=%d:%d:flags=area -fps_mode passthrough -f rawvideo -pix_fmt rgb24 -", q, pw, ph);
    p = popen(cmd, "r");
    size_t ps = (size_t)pw * ph * 3;
    unsigned char *big = malloc(ps);
    for (int i = 0; fread(big, 1, ps, p) == ps; i++)
        ff_picture(f, i, pw, ph, big);
    pclose(p);
    free(big);

    // the report goes next to the video unless -o says otherwise
    char def[4096];
    if (!out)
    {
        snprintf(def, sizeof def, "%s", video);
        char *dot = strrchr(def, '.'), *slash = strrchr(def, '/');
        if (dot && (!slash || dot > slash))
            *dot = 0;
        strncat(def, ".finch.html", sizeof def - strlen(def) - 1);
        out = def;
    }
    FILE *o = fopen(out, "w");
    if (!o)
        die("can't write the report");
    const char *base = strrchr(video, '/');
    ff_write_html(f, o, base ? base + 1 : video);
    fclose(o);
    printf("%s\n", out);
    ff_free(f);
    free(rgb);
    free(pts);
    return 0;
}
