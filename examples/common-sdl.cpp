#include "common-sdl.h"

#include <iostream>

using namespace std;


audio_async::audio_async(int len_ms) {
    m_len_ms = len_ms;

    m_running = false;
}

audio_async::~audio_async() {
    if (m_dev_id_in) {
        SDL_CloseAudioDevice(m_dev_id_in);
    }
}

bool audio_async::init(int capture_id, int sample_rate) {
    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL: %s\n", SDL_GetError());
        return false;
    }

    SDL_SetHintWithPriority(SDL_HINT_AUDIO_RESAMPLING_MODE, "medium", SDL_HINT_OVERRIDE);

    {
        int nDevices = SDL_GetNumAudioDevices(SDL_TRUE);
        fprintf(stderr, "%s: found %d capture devices:\n", __func__, nDevices);
        for (int i = 0; i < nDevices; i++) {
            fprintf(stderr, "%s:    - Capture device #%d: '%s'\n", __func__, i, SDL_GetAudioDeviceName(i, SDL_TRUE));
        }
    }

    SDL_AudioSpec capture_spec_requested;
    SDL_AudioSpec capture_spec_obtained;

    SDL_zero(capture_spec_requested);
    SDL_zero(capture_spec_obtained);

    capture_spec_requested.freq     = sample_rate;
    capture_spec_requested.format   = AUDIO_F32;
    capture_spec_requested.channels = 1;
    capture_spec_requested.samples  = 1024;
    capture_spec_requested.callback = [](void * userdata, uint8_t * stream, int len) {
        audio_async * audio = (audio_async *) userdata;
        audio->callback(stream, len);
    };
    capture_spec_requested.userdata = this;

    if (capture_id >= 0) {
        fprintf(stderr, "%s: attempt to open capture device %d : '%s' ...\n", __func__, capture_id, SDL_GetAudioDeviceName(capture_id, SDL_TRUE));
        m_dev_id_in = SDL_OpenAudioDevice(SDL_GetAudioDeviceName(capture_id, SDL_TRUE), SDL_TRUE, &capture_spec_requested, &capture_spec_obtained, 0);
    } else {
        fprintf(stderr, "%s: attempt to open default capture device ...\n", __func__);
        m_dev_id_in = SDL_OpenAudioDevice(nullptr, SDL_TRUE, &capture_spec_requested, &capture_spec_obtained, 0);
    }

    if (!m_dev_id_in) {
        fprintf(stderr, "%s: couldn't open an audio device for capture: %s!\n", __func__, SDL_GetError());
        m_dev_id_in = 0;

        return false;
    } else {
        fprintf(stderr, "%s: obtained spec for input device (SDL Id = %d):\n", __func__, m_dev_id_in);
        fprintf(stderr, "%s:     - sample rate:       %d\n",                   __func__, capture_spec_obtained.freq);
        fprintf(stderr, "%s:     - format:            %d (required: %d)\n",    __func__, capture_spec_obtained.format,
                capture_spec_requested.format);
        fprintf(stderr, "%s:     - channels:          %d (required: %d)\n",    __func__, capture_spec_obtained.channels,
                capture_spec_requested.channels);
        fprintf(stderr, "%s:     - samples per frame: %d\n",                   __func__, capture_spec_obtained.samples);
    }

    m_sample_rate = capture_spec_obtained.freq;

    m_audio.resize((m_sample_rate*m_len_ms)/1000);

    bufferSize = (m_sample_rate*m_len_ms)/1000;
    m_buf_1.resize(bufferSize);
    m_buf_2.resize(bufferSize);

    return true;
}

bool audio_async::resume() {
    if (!m_dev_id_in) {
        fprintf(stderr, "%s: no audio device to resume!\n", __func__);
        return false;
    }

    if (m_running) {
        fprintf(stderr, "%s: already running!\n", __func__);
        return false;
    }

    SDL_PauseAudioDevice(m_dev_id_in, 0);

    m_running = true;

    return true;
}

bool audio_async::pause() {
    if (!m_dev_id_in) {
        fprintf(stderr, "%s: no audio device to pause!\n", __func__);
        return false;
    }

    if (!m_running) {
        fprintf(stderr, "%s: already paused!\n", __func__);
        return false;
    }

    SDL_PauseAudioDevice(m_dev_id_in, 1);

    m_running = false;

    return true;
}

bool audio_async::clear() {
    if (!m_dev_id_in) {
        fprintf(stderr, "%s: no audio device to clear!\n", __func__);
        return false;
    }

    if (!m_running) {
        fprintf(stderr, "%s: not running!\n", __func__);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_audio_pos = 0;
        m_audio_len = 0;
    }

    return true;
}

// callback to be called by SDL
void audio_async::callback(uint8_t * stream, int len) {
    if (!m_running) {
        return;
    }

    size_t n_samples = len / sizeof(float);

    if (n_samples > bufferSize) {
        n_samples = bufferSize;
        stream += (len - (n_samples * sizeof(float)));
    }

    if (!buffered.load()) {
        std::unique_lock<std::mutex> lock(m_mutex_2);
        memcpy(&m_buf_2[buf2WriteIndex], stream, n_samples * sizeof(float));
        buf2WriteIndex += n_samples;
        if (buf2WriteIndex >= bufferSize/2) {
            state2 = FULL;
            buffered.store(true);
            buf1Read.store(false);
            // cout << "eniac: buffered" << endl;
        } else {
            state2 = PARTIAL;
        }
        return;
    }

    if (buf1Write.load()) {
        std::unique_lock<std::mutex> lock(m_mutex_1);
        // cout << "eniac: writing to buffer 1" << endl;
        memcpy(&m_buf_1[buf1WriteIndex], stream, n_samples * sizeof(float));
        buf1WriteIndex += n_samples;
        if (buf1WriteIndex >= bufferSize) {
            state1 = FULL;
            // cout << "eniac: [WR] m_buf_1 is full switching!!! " << endl;
        } else {
            state1 = PARTIAL;
        }
    } else if (!buf1Write.load()) {
        std::unique_lock<std::mutex> lock(m_mutex_2);
        // cout << "eniac: writing to buffer 2" << endl;
        memcpy(&m_buf_2[buf2WriteIndex], stream, n_samples * sizeof(float));
        buf2WriteIndex += n_samples;
        if (buf2WriteIndex >= bufferSize) {
            state2 = FULL;
            // cout << "eniac: [WR] m_buf_2 is full switching!!! " << endl;
        } else {
            state2 = PARTIAL;
        }
    }
}

bool audio_async::get(int ms, std::vector<float> & result) {
    if (!m_dev_id_in) {
        fprintf(stderr, "%s: no audio device to get audio from!\n", __func__);
        return false;
    }

    if (!m_running) {
        fprintf(stderr, "%s: not running!\n", __func__);
        return false;
    }

    result.clear();

    if (ms <= 0) {
        ms = m_len_ms;
    }

    size_t n_samples = (m_sample_rate * ms) / 1000;
    if (n_samples > bufferSize) {
        n_samples = bufferSize;
    }

    if (!buffered.load()) {
        return false;
    }

    if (buf1Read.load())
    {
        std::unique_lock<std::mutex> lock(m_mutex_1);
        if (state1 != EMPTY && buf1WriteIndex >= n_samples) {
            buf1Read.store(false);
            buf1Write.store(false);
            result.resize(buf1WriteIndex);
            memcpy(result.data(), &m_buf_1[0], buf1WriteIndex * sizeof(float));
            state1 = EMPTY;
            // cout << "eniac: [RD] m_buf_1: read fully !!! writeIndex of m_buf_1 = " << buf1WriteIndex << endl;
            buf1WriteIndex = 0;
            return true;
        }
    } else if (!buf1Read.load()) {
        std::unique_lock<std::mutex> lock(m_mutex_2);
        if (state2 != EMPTY && buf2WriteIndex >= n_samples) {
            buf1Read.store(true);
            buf1Write.store(true);
            result.resize(buf2WriteIndex);
            memcpy(result.data(), &m_buf_2[0], buf2WriteIndex * sizeof(float));
            state2 = EMPTY;
            // cout << "eniac: [RD] m_buf_2: read fully !!! writeIndex of m_buf_2 = " << buf2WriteIndex << endl;
            buf2WriteIndex = 0;
            return true;
        }
    }

    return false;
}

bool sdl_poll_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                {
                    return false;
                }
            default:
                break;
        }
    }

    return true;
}
