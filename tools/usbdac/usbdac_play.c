/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * usbdac-play: play a known signal to one Core Audio device (the H2 in USB
 * DAC mode), at a given rate, without touching the Mac's default output.
 * Writes the samples it sends to REF.raw (24-bit little endian, interleaved
 * stereo), for check.py to find in what the H2's DAC received.
 *
 *   cc -O2 -o usbdac-play usbdac_play.c -framework CoreAudio -framework CoreFoundation
 *   ./usbdac-play "Fathom" 96000 5 ref.raw      # name part, rate, seconds, reference out
 *   ./usbdac-play "Fathom" 44100 5 ref.raw 16   # optional depth: 16 (low 8 bits zero) or
 *                                                # 32 (half an LSB below 24 bits, as a float
 *                                                # app sends; the reference is then not exact)
 *   ./usbdac-play -l                             # list output devices and their formats
 *
 * The signal is pseudo-random 24-bit samples at about -30 dB: every one of
 * the low 18 bits and the sign change all the time, and it isn't loud.
 * A 24-bit integer sent as a 32-bit float is exact, and Core Audio's float
 * to integer conversion for the device is exact for it too (no dither, no
 * volume: the H2 has no volume control on the USB side).
 */
#include <CoreAudio/CoreAudio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector s, AudioObjectPropertyScope sc)
{
    AudioObjectPropertyAddress a = {s, sc, kAudioObjectPropertyElementMain};
    return a;
}

static void dev_name(AudioDeviceID d, char *out, size_t cap)
{
    CFStringRef name = NULL;
    UInt32 sz = sizeof name;
    AudioObjectPropertyAddress a = addr(kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal);
    out[0] = '\0';
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, &name) == noErr && name) {
        CFStringGetCString(name, out, (CFIndex)cap, kCFStringEncodingUTF8);
        CFRelease(name);
    }
}

static int out_channels(AudioDeviceID d)
{
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &sz) != noErr || !sz) return 0;
    AudioBufferList *bl = malloc(sz);
    int n = 0;
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, bl) == noErr)
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++) n += (int)bl->mBuffers[i].mNumberChannels;
    free(bl);
    return n;
}

static Float64 dev_rate(AudioDeviceID d)
{
    Float64 r = 0;
    UInt32 sz = sizeof r;
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
    AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, &r);
    return r;
}

static void physical_format(AudioDeviceID d, char *out, size_t cap)
{
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
    AudioStreamID s[8];
    UInt32 sz = sizeof s;
    snprintf(out, cap, "?");
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, s) != noErr || !sz) return;
    AudioStreamBasicDescription f;
    sz = sizeof f;
    a = addr(kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal);
    if (AudioObjectGetPropertyData(s[0], &a, 0, NULL, &sz, &f) != noErr) return;
    snprintf(out, cap, "%u ch, %u-bit %s%s, %.0f Hz", f.mChannelsPerFrame, f.mBitsPerChannel,
             f.mFormatFlags & kAudioFormatFlagIsFloat ? "float" : "int",
             f.mFormatFlags & kAudioFormatFlagIsNonInterleaved ? " non-interleaved" : "", f.mSampleRate);
}

static int devices(AudioDeviceID *ids, int cap)
{
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    UInt32 sz = (UInt32)(sizeof *ids * (size_t)cap);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids) != noErr) return 0;
    return (int)(sz / sizeof *ids);
}

/* ---- the signal ---- */

static uint32_t g_x = 0x2545F491;
static int32_t next_sample(void)              /* 24-bit, within +-2^18 */
{
    g_x ^= g_x << 13;
    g_x ^= g_x >> 17;
    g_x ^= g_x << 5;
    return (int32_t)(g_x & 0x7FFFF) - 0x40000;
}

static int32_t *g_sig;                         /* interleaved stereo */
static float g_frac;                           /* added below the 24th bit (depth 32) */
static long g_frames, g_pos, g_tail;            /* tail: silent frames sent after */
static long g_lead;                             /* silent frames first: the H2 opens its side */

static OSStatus io(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in,
                   const AudioTimeStamp *in_time, AudioBufferList *out, const AudioTimeStamp *out_time, void *ctx)
{
    (void)dev; (void)now; (void)in; (void)in_time; (void)out_time; (void)ctx;
    AudioBuffer *b = &out->mBuffers[0];
    float *f = b->mData;
    UInt32 ch = b->mNumberChannels, n = b->mDataByteSize / (sizeof(float) * ch);
    for (UInt32 i = 0; i < n; i++) {
        if (g_lead > 0) { g_lead--; memset(f + i * ch, 0, sizeof *f * ch); continue; }
        for (UInt32 c = 0; c < ch; c++)
            f[i * ch + c] = g_pos < g_frames && c < 2 ? ((float)g_sig[g_pos * 2 + c] + g_frac) / 8388608.0f : 0.0f;
        if (g_pos < g_frames) g_pos++;
        else g_tail++;
    }
    return noErr;
}

int main(int argc, char **argv)
{
    AudioDeviceID ids[64];
    int n = devices(ids, 64);
    if (argc == 2 && !strcmp(argv[1], "-l")) {
        for (int i = 0; i < n; i++) {
            char name[128], fmt[96];
            if (!out_channels(ids[i])) continue;
            dev_name(ids[i], name, sizeof name);
            physical_format(ids[i], fmt, sizeof fmt);
            printf("%-40s %s\n", name, fmt);
        }
        return 0;
    }
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: %s NAME RATE SECONDS REF.raw [16|24|32] | -l\n", argv[0]);
        return 2;
    }
    AudioDeviceID dev = 0;
    for (int i = 0; i < n && !dev; i++) {
        char name[128];
        dev_name(ids[i], name, sizeof name);
        if (strstr(name, argv[1]) && out_channels(ids[i]) >= 2) dev = ids[i];
    }
    if (!dev) { fprintf(stderr, "no output device named like \"%s\"\n", argv[1]); return 1; }

    Float64 rate = atof(argv[2]);
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
    if (AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof rate, &rate) != noErr) {
        fprintf(stderr, "cannot set %.0f Hz\n", rate);
        return 1;
    }
    for (int i = 0; i < 50 && dev_rate(dev) != rate; i++) usleep(100000);
    char fmt[96];
    physical_format(dev, fmt, sizeof fmt);
    printf("device at %.0f Hz; physical format %s\n", dev_rate(dev), fmt);

    g_frames = (long)(rate * atof(argv[3]));
    g_sig = malloc((size_t)g_frames * 2 * sizeof *g_sig);
    int bits = argc == 6 ? atoi(argv[5]) : 24;
    if (bits == 32) g_frac = 0.5f;
    FILE *ref = fopen(argv[4], "wb");
    if (!g_sig || !ref) { perror(argv[4]); return 1; }
    for (long i = 0; i < g_frames * 2; i++) {
        g_sig[i] = next_sample();
        if (bits == 16) g_sig[i] &= ~0xff;
        uint8_t b[3] = {(uint8_t)g_sig[i], (uint8_t)(g_sig[i] >> 8), (uint8_t)(g_sig[i] >> 16)};
        fwrite(b, 1, 3, ref);
    }
    fclose(ref);

    g_lead = (long)rate;                         /* 1 s */
    AudioDeviceIOProcID proc;
    if (AudioDeviceCreateIOProcID(dev, io, NULL, &proc) != noErr || AudioDeviceStart(dev, proc) != noErr) {
        fprintf(stderr, "cannot start the device\n");
        return 1;
    }
    while (g_pos < g_frames || g_tail < (long)rate / 2) usleep(20000);
    AudioDeviceStop(dev, proc);
    AudioDeviceDestroyIOProcID(dev, proc);
    printf("sent %ld frames\n", g_frames);
    return 0;
}
