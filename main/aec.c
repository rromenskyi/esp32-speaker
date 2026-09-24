#include "aec.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"

static const char *TAG = "aec";

// Echo tail the canceller models (64 ms). The speaker sits centimetres from the
// mics, so the direct path dominates; 2048 (128 ms) cost 73% of a core.
#define FILTER_LEN   1024
#define RATE         16000

// speexdsp allocates from PSRAM unless this is set (see speex_alloc_psram.h).
// Measured on the device: the preprocessor in internal RAM saves ~1.5 ms per
// frame but costs 46 KB of it, which Wi-Fi/TLS need more; so both stay in PSRAM.
int speex_alloc_internal;

static SpeexEchoState *s_echo;
static SpeexPreprocessState *s_pre;
static volatile bool s_on = true;
static uint32_t s_last_us;
static aec_stats_t s_st;

static double msq(const int16_t *x)
{
    double a = 0;
    for (int i = 0; i < AEC_FRAME; i++) a += (double)x[i] * x[i];
    return a / AEC_FRAME;
}

esp_err_t aec_init(void)
{
    s_echo = speex_echo_state_init(AEC_FRAME, FILTER_LEN);
    s_pre = speex_preprocess_state_init(AEC_FRAME, RATE);
    if (!s_echo || !s_pre) return ESP_ERR_NO_MEM;
    int rate = RATE, on = 1, agc_level = 12000, noise_db = -30, echo_db = -45, echo_active_db = -15;
    speex_echo_ctl(s_echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    // Residual echo suppression needs the canceller's state.
    speex_preprocess_ctl(s_pre, SPEEX_PREPROCESS_SET_ECHO_STATE, s_echo);
    speex_preprocess_ctl(s_pre, SPEEX_PREPROCESS_SET_DENOISE, &on);
    speex_preprocess_ctl(s_pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_db);
    speex_preprocess_ctl(s_pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &echo_db);
    speex_preprocess_ctl(s_pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &echo_active_db);
    // AGC: far-field speech arrives quiet; bring it to a steady level for STT.
    float level = agc_level;
    speex_preprocess_ctl(s_pre, SPEEX_PREPROCESS_SET_AGC, &on);
    speex_preprocess_ctl(s_pre, SPEEX_PREPROCESS_SET_AGC_LEVEL, &level);
    // Cap the AGC so it can't pump residual echo back up during playback.
    int max_gain_db = 15;
    speex_preprocess_ctl(s_pre, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &max_gain_db);
    ESP_LOGI(TAG, "speexdsp AEC %d-sample tail + denoise + AGC", FILTER_LEN);
    return ESP_OK;
}

void aec_process(const int16_t *mic, const int16_t *ref, int16_t *out)
{
    if (!s_on) {
        if (out != mic) memcpy(out, mic, AEC_FRAME * sizeof(int16_t));
        return;
    }
    int64_t t0 = esp_timer_get_time();
    int16_t tmp[AEC_FRAME];
    double m = msq(mic), r = msq(ref);
    speex_echo_cancellation(s_echo, mic, ref, tmp);
    uint32_t t_echo = (uint32_t)(esp_timer_get_time() - t0);
    double c = msq(tmp);
    speex_preprocess_run(s_pre, tmp);
    memcpy(out, tmp, sizeof(tmp));
    s_last_us = (uint32_t)(esp_timer_get_time() - t0);
    s_st.mic += m; s_st.ref += r; s_st.cancelled += c; s_st.out += msq(tmp);
    s_st.frames++;
    s_st.echo_us += t_echo;
    if (s_last_us > s_st.max_us) s_st.max_us = s_last_us;
}

void aec_reset(void)
{
    speex_echo_state_reset(s_echo);
}

void aec_set_enabled(bool on) { s_on = on; }
bool aec_enabled(void) { return s_on; }
uint32_t aec_last_us(void) { return s_last_us; }
void aec_stats_reset(void) { memset(&s_st, 0, sizeof(s_st)); }
aec_stats_t aec_stats_get(void) { return s_st; }
