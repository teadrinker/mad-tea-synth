// The win32 player for an exported song: a window, a DIB, and a waveOut sink
// on its own thread. Links user32/gdi32/kernel32/winmm only. Not generated;
// per-export values are in song_config.h.
#include <windows.h>
#include <mmsystem.h>

#include "song_config.h"   // generated: SONG_SAMPLE_RATE, VSCREEN_W/H
#include "song/song.h"
#include "vscreen.h"

// Audio: waveOut ring, ~370 ms at 44.1 kHz, refilled by a pump thread.
#ifndef AUDIO_BUFFER_COUNT
#define AUDIO_BUFFER_COUNT 8
#endif

// Whole batches, so the generator is never asked for a partial one.
#ifndef AUDIO_BATCHES_PER_BUFFER
#define AUDIO_BATCHES_PER_BUFFER 64
#endif

#define AUDIO_BUFFER_SAMPLES (SONG_BATCH_SAMPLES * AUDIO_BATCHES_PER_BUFFER)

// Drawing frame interval; audio doesn't use it.
#ifndef FRAME_MS
#define FRAME_MS 16
#endif

static HWAVEOUT   g_wave;
static WAVEHDR    g_hdr[AUDIO_BUFFER_COUNT];
// frames x channels; sizeof feeds dwBufferLength.
static short      g_buf[AUDIO_BUFFER_COUNT][AUDIO_BUFFER_SAMPLES * SONG_OUTPUT_CHANNELS];

// The visual pass keeps a separate state so drawing can't advance audio note phases.
static int            g_song_sample_id;
static song_SongState g_song_state;

static song_SongState g_visual_state;

static int g_total_samples;
static int g_finished;

static void audio_fill(int which)
{
    short *dst = g_buf[which];
    for (int b = 0; b < AUDIO_BATCHES_PER_BUFFER; b++)
    {
        if (g_song_sample_id >= g_total_samples)
        {
            // Past the end: silence. The sequencer keeps channels latched, so calling on
            // would sustain the last events.
            for (int i = 0; i < SONG_BATCH_SAMPLES * SONG_OUTPUT_CHANNELS; i++) dst[i] = 0;
            g_finished = 1;
        }
        else
        {
            g_song_sample_id = song_render_audio_batch(g_song_sample_id, &g_song_state, dst);
        }
        dst += SONG_BATCH_SAMPLES * SONG_OUTPUT_CHANNELS;
    }
}

static int audio_start(void)
{
    WAVEFORMATEX fmt;
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    // 2 when some sound reads `pan`; song.c already interleaves L,R.
    fmt.nChannels       = SONG_OUTPUT_CHANNELS;
    fmt.nSamplesPerSec  = SONG_SAMPLE_RATE;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize          = 0;

    if (waveOutOpen(&g_wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return 0;

    for (int i = 0; i < AUDIO_BUFFER_COUNT; i++)
    {
        audio_fill(i);
        g_hdr[i].lpData         = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = sizeof(g_buf[i]);
        g_hdr[i].dwFlags        = 0;
        waveOutPrepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
        waveOutWrite(g_wave, &g_hdr[i], sizeof(WAVEHDR));
    }
    return 1;
}

// Returns 0 once the song ended and every buffer drained.
static int audio_pump(void)
{
    int inflight = 0;
    for (int i = 0; i < AUDIO_BUFFER_COUNT; i++)
    {
        if (!(g_hdr[i].dwFlags & WHDR_DONE)) { inflight++; continue; }
        if (g_finished) continue;

        waveOutUnprepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
        audio_fill(i);
        g_hdr[i].dwFlags        = 0;
        g_hdr[i].dwBufferLength = sizeof(g_buf[i]);
        waveOutPrepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
        waveOutWrite(g_wave, &g_hdr[i], sizeof(WAVEHDR));
        inflight++;
    }
    return !g_finished || inflight > 0;
}

// Where the speaker is, read from the device: a clock, so slow frames never
// move the song. On failure or the 32-bit wrap the caller holds the last
// position, since song_visual_render only walks forward.
static int audio_played_samples(void)
{
    static int last;

    MMTIME t;
    t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(g_wave, &t, sizeof(t)) == MMSYSERR_NOERROR &&
        t.wType == TIME_SAMPLES)
    {
        int now = (int)t.u.sample;
        if (now > last) last = now;
    }
    return last;
}

// The pump runs on its own thread: refilling from the message loop would let
// one slow frame run the ring dry, and waveOut then stretches the song in time.
// The halves share only two single-word flags. Not a waveOut callback: those
// run under restrictions.
static volatile long g_quit_audio;  // render -> audio: stop
static volatile long g_audio_done;  // audio -> render: song finished and drained

static DWORD WINAPI audio_thread(LPVOID unused)
{
    (void)unused;
    while (!g_quit_audio)
    {
        if (!audio_pump()) { g_audio_done = 1; break; }
        // Tops up long before it matters, at negligible cost.
        Sleep(2);
    }
    return 0;
}

// ---- Window + framebuffer ----
static HWND     g_hwnd;
static HDC      g_memdc;
static void    *g_bits;      // 32bpp, VSCREEN_W x VSCREEN_H, top-down

static void make_dib(void)
{
    // BI_BITFIELDS, red in the low byte, as vscreen_expand_rgba writes; BI_RGB
    // would swap red and blue.
    struct { BITMAPINFOHEADER h; DWORD mask[3]; } bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.h.biSize        = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth       = VSCREEN_W;
    bi.h.biHeight      = -VSCREEN_H;   // negative = top-down, matching vscreen
    bi.h.biPlanes      = 1;
    bi.h.biBitCount    = 32;
    bi.h.biCompression = BI_BITFIELDS;
    bi.mask[0] = 0x000000FF;  // red
    bi.mask[1] = 0x0000FF00;  // green
    bi.mask[2] = 0x00FF0000;  // blue

    g_memdc = CreateCompatibleDC(0);
    HBITMAP bmp = CreateDIBSection(g_memdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
                                   &g_bits, 0, 0);
    SelectObject(g_memdc, bmp);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY || (m == WM_KEYDOWN && w == VK_ESCAPE)) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

// Plain main(): the tiny build reaches it via crt_stub_win32.c's mainCRTStartup.
int main(void)
{
    HINSTANCE inst = GetModuleHandleA(0);

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(0, IDC_ARROW);
    wc.lpszClassName = "codesynth_song";
    RegisterClassA(&wc);

    // Client area = framebuffer, so the blit is 1:1.
    RECT r = { 0, 0, VSCREEN_W, VSCREEN_H };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, 0);
    g_hwnd = CreateWindowA("codesynth_song", SONG_WINDOW_TITLE,
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, 0, 0, inst, 0);
    make_dib();

    g_total_samples = song_total_samples();
    if (!audio_start()) { MessageBoxA(g_hwnd, "no audio device", "codesynth", MB_OK); return 1; }

    HANDLE audio = CreateThread(0, 0, audio_thread, 0, 0, 0);
    // Above normal: it must not wait behind a busy render thread.
    if (audio) SetThreadPriority(audio, THREAD_PRIORITY_ABOVE_NORMAL);

    // This loop only draws; audio is the pump thread's.
    DWORD next_frame = GetTickCount();
    for (;;)
    {
        MSG msg;
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) goto quit;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (g_audio_done) goto quit;

        DWORD now = GetTickCount();
        if ((int)(now - next_frame) >= 0)
        {
            next_frame += FRAME_MS;
            // Behind: skip ahead rather than burst catch-up frames.
            if ((int)(now - next_frame) > 0) next_frame = now + FRAME_MS;

#ifdef SONG_VISUALS
            // Driven off the speaker position with its own sequencer state.
            vscreen_images_reset();
            song_visual_render(audio_played_samples(), &g_visual_state);
            vscreen_expand_rgba((unsigned int *)g_bits, VSCREEN_W);
#endif
            HDC dc = GetDC(g_hwnd);
            BitBlt(dc, 0, 0, VSCREEN_W, VSCREEN_H, g_memdc, 0, 0, SRCCOPY);
            ReleaseDC(g_hwnd, dc);
        }

        Sleep(1);
    }

quit:
    // Stop the pump before waveOutReset, which would race an in-flight waveOutWrite.
    g_quit_audio = 1;
    if (audio) { WaitForSingleObject(audio, 2000); CloseHandle(audio); }

    waveOutReset(g_wave);
    for (int i = 0; i < AUDIO_BUFFER_COUNT; i++)
        waveOutUnprepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
    waveOutClose(g_wave);
    ExitProcess(0);
}
