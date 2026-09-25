#pragma once
#include "esp_err.h"

// Device state shown on the case LEDs. Network states are derived from Wi-Fi
// automatically; `status_preview` forces a pattern for a few seconds (testing).
typedef enum {
    STATUS_AUTO = -1,
    STATUS_OFF,          // idle: faint green double blink every 8 s (heartbeat)
    STATUS_PORTAL,       // provisioning AP up: green blink, 1 Hz
    STATUS_CONNECTING,   // joining the saved network: slow blue breathing
    STATUS_CONNECTED,    // just joined: solid green for 2 s, then off
    // Voice states (set by the voice client), shown while Wi-Fi is up:
    STATUS_LISTENING,    // solid blue
    STATUS_THINKING,     // purple breathing
    STATUS_SPEAKING,     // soft green
    STATUS_ERROR,        // red, briefly
    STATUS_SERVER_DOWN,  // short red pulse every 5 s
    STATUS_HOLD,         // k1 held since boot: amber blink, faster as it nears the reset
    STATUS_AUTO_IDLE,    // auto (wake word) mode, idle: faint slow cyan pulse
} status_t;

esp_err_t status_start(void);
// Voice-layer state; STATUS_OFF = idle. Network states take precedence.
void status_set_voice(status_t s);
void status_set_hold(int progress_percent);   // 0..100; <0 ends the hold display
void status_preview(status_t s, int seconds);
