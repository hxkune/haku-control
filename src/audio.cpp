// SPDX-License-Identifier: GPL-3.0-only
// Sound level for the audio effect: WASAPI loopback of the default output device (what you hear).
// The capture thread runs only while some zone shows the audio effect (sensors.c starts / stops it).
// Output: level (whole signal) and bass (below ~150 Hz), 0..1, with an automatic gain so quiet and loud
// music both fill the range.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <math.h>
#include <process.h>

extern "C" void logf_(const char *fmt, ...);

static HANDLE th;
static volatile LONG run;
static volatile LONG level_x1000, bass_x1000;

static unsigned __stdcall capture(void *) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL; IAudioClient *ac = NULL; IAudioCaptureClient *cc = NULL;
    WAVEFORMATEX *wf = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en);
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void **)&ac);
    if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&wf);
    if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000 /* 20 ms */, 0, wf, NULL);
    if (SUCCEEDED(hr)) hr = ac->GetService(__uuidof(IAudioCaptureClient), (void **)&cc);
    if (SUCCEEDED(hr)) hr = ac->Start();

    // sample format: float32 (the usual mix format) or 16-bit PCM
    bool is_float = false;
    if (wf) {
        if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) is_float = true;
        else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE *x = (WAVEFORMATEXTENSIBLE *)wf;
            is_float = !!IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }
    }
    if (SUCCEEDED(hr) && !is_float && wf->wBitsPerSample != 16) hr = E_FAIL;
    if (FAILED(hr)) logf_("audio: loopback capture unavailable (0x%08lX)", (unsigned long)hr);
    else logf_("audio: capturing %lu Hz, %d ch", wf->nSamplesPerSec, wf->nChannels);

    const int ch = wf ? wf->nChannels : 2;
    const float rate = wf ? (float)wf->nSamplesPerSec : 48000.0f;
    const float lp_a = 1.0f - expf(-2.0f * 3.14159265f * 150.0f / rate);   // one-pole low-pass for the bass
    float lp = 0, sum2 = 0, bsum2 = 0; int nwin = 0;
    const int win = (int)(rate / 100);                                        // 10 ms windows
    float env = 0.05f, benv = 0.02f;                                          // automatic gain envelopes
    DWORD last_data = GetTickCount();

    while (run && SUCCEEDED(hr)) {
        Sleep(8);
        UINT32 packet = 0;
        while (SUCCEEDED(cc->GetNextPacketSize(&packet)) && packet) {
            BYTE *data; UINT32 frames; DWORD flags;
            if (FAILED(cc->GetBuffer(&data, &frames, &flags, NULL, NULL))) break;
            last_data = GetTickCount();
            for (UINT32 i = 0; i < frames; i++) {
                float m = 0;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    for (int c = 0; c < ch; c++)
                        m += is_float ? ((float *)data)[i * ch + c] : ((short *)data)[i * ch + c] / 32768.0f;
                    m /= ch;
                }
                lp += (m - lp) * lp_a;
                sum2 += m * m; bsum2 += lp * lp;
                if (++nwin >= win) {
                    float rms = sqrtf(sum2 / nwin), brms = sqrtf(bsum2 / nwin);
                    sum2 = bsum2 = 0; nwin = 0;
                    // envelopes follow peaks at once and fall back over ~8 s, never below a noise floor
                    env = rms > env ? rms : fmaxf(env * 0.9988f, 0.02f);
                    benv = brms > benv ? brms : fmaxf(benv * 0.9988f, 0.01f);
                    float lv = fminf(rms / env, 1.0f), bs = fminf(brms / benv, 1.0f);
                    if (rms < 0.002f) lv = 0;            // silence
                    if (brms < 0.002f) bs = 0;
                    InterlockedExchange(&level_x1000, (LONG)(lv * 1000));
                    InterlockedExchange(&bass_x1000, (LONG)(bs * 1000));
                }
            }
            cc->ReleaseBuffer(frames);
        }
        // nothing playing: the loopback stream delivers no packets at all
        if (GetTickCount() - last_data > 100) { InterlockedExchange(&level_x1000, 0); InterlockedExchange(&bass_x1000, 0); }
    }
    if (ac) ac->Stop();
    if (cc) cc->Release();
    if (ac) ac->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    if (wf) CoTaskMemFree(wf);
    CoUninitialize();
    InterlockedExchange(&level_x1000, 0); InterlockedExchange(&bass_x1000, 0);
    return 0;
}

extern "C" void audio_start(void) {
    if (th) return;
    run = 1;
    th = (HANDLE)_beginthreadex(NULL, 0, capture, NULL, 0, NULL);
}

extern "C" void audio_stop(void) {
    if (!th) return;
    run = 0;
    WaitForSingleObject(th, 2000);
    CloseHandle(th); th = NULL;
    logf_("audio: capture stopped (not in use)");
}

extern "C" void audio_read(float *level, float *bass) {
    *level = level_x1000 / 1000.0f;
    *bass = bass_x1000 / 1000.0f;
}
