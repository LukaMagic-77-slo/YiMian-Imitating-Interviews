// 桌面语音 AI 助手（A 板：ESP32-S3 AIoT BasicV2）
// 合并能力：语音对话(ES8311) + 摄像头实时预览(GC2145 -> ST7789) + 拍照上传 + WS2812 状态彩灯
//   - 按住 BOOT 键(GPIO0，低电平)说话：PTT 语音对话，和云端 AI 实时聊天
//   - 屏幕实时显示摄像头画面（AI 看到的画面，你也能看到）
//   - 云端触发 camera0 端点：拍照编码 JPEG 上传，AI 即可"看图"（A 板无物理拍照键）
//   - WS2812 彩灯状态：就绪=蓝，聆听=红，回复=绿，拍照=白闪

#include "board.h"
#include "config.h"
#include "es8311_audio.h"
#include "st7789_lcd.h"
#include "ws2812_led.h"

#include "esp_camera.h"
#include "img_converters.h"
#include "agent_link.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#define TAG "Assistant"

class AssistantBoard : public Board {
public:
    AssistantBoard() {
        InitLed();
        InitLcd();
        InitCamera();
        InitCodec();
        InitPttButton();
        InitInterviewButton();
        RegisterCaptureEndpoint();
        if (codec_ok_) {
            xTaskCreate(&AssistantBoard::PlayTaskEntry, "spk_play", 4096, this, 5, &play_task_);
            xTaskCreate(&AssistantBoard::PttTaskEntry, "ptt_voice", 4096, this, 5, &ptt_task_);
        }
        xTaskCreate(&AssistantBoard::PreviewTaskEntry, "cam_preview", 4096, this, 5, &preview_task_);
        SetLedColor(0, 0, 10);   // 蓝=就绪
    }

    const char* Name() const override { return "AI_ASSISTANT"; }

    uint32_t Capabilities() const override {
        return AGENT_CAP_MIC | AGENT_CAP_SPEAKER | AGENT_CAP_SCREEN |
               AGENT_CAP_BUTTON | AGENT_CAP_RECORDING | AGENT_CAP_CAMERA | AGENT_CAP_LED;
    }

    // Agent 回复的 TTS 音频 → 缓冲播放；彩灯变绿表示"正在回复"
    void PlayAudio(const uint8_t* pcm16, size_t bytes) override {
        if (!play_buf_ || !pcm16 || bytes == 0) return;
        const size_t sent = xStreamBufferSend(play_buf_, pcm16, bytes, 0);
        if (!speaking_shown_) { speaking_shown_ = true; SetLedColor(0, 20, 0); }
        if (sent < bytes) {
            ESP_LOGW(TAG, "play buffer full — dropped %u/%u bytes", (unsigned)(bytes - sent), (unsigned)bytes);
        }
    }
    void AudioEnd() override {
        speaking_shown_ = false;
        SetLedColor(0, 0, 10);
    }
    // Agent 下发的文字（若 App 支持下发，会显示在屏幕；当前屏幕被摄像头预览占用，先打日志）
    void ShowText(const char* utf8) override { ESP_LOGI(TAG, "agent text: %s", utf8 ? utf8 : ""); }
    // 云端 led0 端点 → WS2812
    void SetLed(uint32_t rgb) override { (void)led_.SetRgb(rgb); }

    void BeginInterview() override {
        topic_captured_.store(false, std::memory_order_release);
        bool expected_schedule = false;
        if (!interview_start_scheduled_.compare_exchange_strong(expected_schedule, true)) return;
        xTaskCreate(&AssistantBoard::InterviewStartTaskEntry, "interview_start", 3072, this, 4, nullptr);
    }

    static void InterviewStartTaskEntry(void* arg) {
        auto* self = static_cast<AssistantBoard*>(arg);
        // READY is reported before the App subscribes to 0xFFC4. Give the
        // control/event channel time to become notify-ready before sending.
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (agent_link_state() == AGENT_STATE_READY) self->SendInterviewStart();
        else ESP_LOGW(TAG, "interview start cancelled: Agent not ready");
        vTaskDelete(nullptr);
    }

    void SendInterviewStart() {
        interview_mode_.store(true, std::memory_order_release);
        static constexpr char kPrompt[] =
            "{\"type\":\"interview_system_prompt\",\"prompt\":\"You are the interviewer's agent. Stay in interviewer mode throughout this session. Ask one question at a time, wait for the candidate's voice answer, then evaluate the answer content, relevance, completeness, clarity, filler words (um/uh), and duration. Give a concise score from 0 to 100 and 2-3 actionable improvement suggestions. Speak the question and feedback in Chinese. Do not answer the interview question yourself.\"}";
        static constexpr char kStart[] =
            "{\"type\":\"interview_start\",\"topic\":null,\"rubric\":[\"relevance\",\"completeness\",\"clarity\",\"filler_words\",\"duration\"]}";
        const esp_err_t p = agent_link_push_event(AGENT_EVT_CUSTOM,
            reinterpret_cast<const uint8_t*>(kPrompt), sizeof(kPrompt) - 1);
        const esp_err_t a = agent_link_push_event(AGENT_EVT_CUSTOM,
            reinterpret_cast<const uint8_t*>(kStart), sizeof(kStart) - 1);
        ESP_LOGI(TAG, "interview MVP started: prompt=%s start=%s (speak topic with BOOT)",
                 esp_err_to_name(p), esp_err_to_name(a));
        if (lcd_ok_) DrawInterviewScreen();
        SetLedColor(0, 0, 20);
    }

    static uint8_t Glyph(char c, int row) {
        static const uint8_t* unused = nullptr; (void)unused;
        switch (c) {
            case 'A': { static const uint8_t g[7]={14,17,17,31,17,17,17}; return g[row]; }
            case 'B': { static const uint8_t g[7]={30,17,17,30,17,17,30}; return g[row]; }
            case 'C': { static const uint8_t g[7]={14,17,16,16,16,17,14}; return g[row]; }
            case 'E': { static const uint8_t g[7]={31,16,16,30,16,16,31}; return g[row]; }
            case 'I': { static const uint8_t g[7]={31,4,4,4,4,4,31}; return g[row]; }
            case 'K': { static const uint8_t g[7]={17,18,20,24,20,18,17}; return g[row]; }
            case 'M': { static const uint8_t g[7]={17,27,21,17,17,17,17}; return g[row]; }
            case 'N': { static const uint8_t g[7]={17,25,21,19,17,17,17}; return g[row]; }
            case 'O': { static const uint8_t g[7]={14,17,17,17,17,17,14}; return g[row]; }
            case 'P': { static const uint8_t g[7]={30,17,17,30,16,16,16}; return g[row]; }
            case 'R': { static const uint8_t g[7]={30,17,17,30,20,18,17}; return g[row]; }
            case 'S': { static const uint8_t g[7]={15,16,16,14,1,1,30}; return g[row]; }
            case 'T': { static const uint8_t g[7]={31,4,4,4,4,4,4}; return g[row]; }
            case 'U': { static const uint8_t g[7]={17,17,17,17,17,17,14}; return g[row]; }
            case 'Y': { static const uint8_t g[7]={17,17,10,4,4,4,4}; return g[row]; }
            default: return 0;
        }
    }

    void DrawInterviewScreen() {
        constexpr uint16_t w = DISPLAY_WIDTH, h = DISPLAY_HEIGHT;
        auto* px = static_cast<uint16_t*>(heap_caps_malloc(static_cast<size_t>(w) * h * 2,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!px) { (void)lcd_.FillSolid(rgb565::kBlue); return; }
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) px[i] = rgb565::kBlue;
        auto draw = [&](const char* text, int y, int scale) {
            const int len = static_cast<int>(strlen(text));
            const int cw = 6 * scale;
            int x = (static_cast<int>(w) - len * cw) / 2;
            for (int i = 0; i < len; ++i, x += cw) {
                for (int r = 0; r < 7; ++r) for (int col = 0; col < 5; ++col)
                    if (Glyph(text[i], r) & (1 << (4 - col)))
                        for (int dy = 0; dy < scale; ++dy) for (int dx = 0; dx < scale; ++dx) {
                            const int xx = x + col * scale + dx, yy = y + r * scale + dy;
                            if (xx >= 0 && xx < w && yy >= 0 && yy < h) px[yy * w + xx] = rgb565::kWhite;
                        }
            }
        };
        draw("MOCK INTERVIEW", 70, 3);
        draw("SAY YOUR TOPIC", 130, 2);
        (void)lcd_.DrawBitmap(0, 0, w, h, px);
        heap_caps_free(px);
    }

private:
    // ---------- WS2812 状态彩灯 ----------
    void InitLed() {
        if (led_.Init(WS2812_LED_PIN) != ESP_OK) { ESP_LOGE(TAG, "WS2812 init failed"); return; }
        (void)led_.Off();
        ESP_LOGI(TAG, "WS2812 LED ready on GPIO%d", (int)WS2812_LED_PIN);
    }
    void SetLedColor(uint8_t r, uint8_t g, uint8_t b) { if (led_.Ready()) (void)led_.SetColor(r, g, b); }

    // ---------- LCD ----------
    void InitLcd() {
        St7789LcdConfig c = {};
        c.spi_host     = DISPLAY_SPI_HOST;
        c.pin_sck      = DISPLAY_SCK_PIN;
        c.pin_mosi     = DISPLAY_MOSI_PIN;
        c.pin_cs       = DISPLAY_CS_PIN;
        c.pin_dc       = DISPLAY_DC_PIN;
        c.pin_rst      = DISPLAY_RST_PIN;
        c.pin_bl       = DISPLAY_BL_PIN;
        c.width        = DISPLAY_WIDTH;
        c.height       = DISPLAY_HEIGHT;
        c.pclk_hz      = DISPLAY_SPI_CLK_HZ;
        c.spi_mode     = DISPLAY_SPI_MODE;
        c.invert_color = true;
        if (lcd_.Init(c) != ESP_OK) { ESP_LOGE(TAG, "LCD init failed"); return; }
        lcd_ok_ = true;
        lcd_.FillSolid(rgb565::kBlack);
        ESP_LOGI(TAG, "LCD ready (240x240)");
    }

    // ---------- 音频：ES8311 ----------
    void InitCodec() {
        Es8311Config c = {};
        c.i2c_port    = I2C_NUM_0;
        c.pin_sda     = AUDIO_I2C_SDA;
        c.pin_scl     = AUDIO_I2C_SCL;
        c.pin_mclk    = AUDIO_I2S_MCLK;
        c.pin_bclk    = AUDIO_I2S_BCLK;
        c.pin_ws      = AUDIO_I2S_WS;
        c.pin_din     = AUDIO_I2S_DIN;
        c.pin_dout    = AUDIO_I2S_DOUT;
        c.pin_pa_en   = AUDIO_PA_EN;
        c.es8311_addr = ES8311_ADDR;
        c.sample_rate = AUDIO_SAMPLE_RATE;
        c.mic_gain    = 30;
        c.out_volume  = 80;
        if (codec_.Init(c) != ESP_OK) { ESP_LOGE(TAG, "codec init failed"); return; }
        codec_ok_ = true;
        play_buf_ = xStreamBufferCreate(kPlayBufBytes, 1);
        if (!play_buf_) { ESP_LOGE(TAG, "play buffer alloc failed"); return; }
        ESP_LOGI(TAG, "ES8311 codec ready");
    }

    static void PlayTaskEntry(void* arg) { static_cast<AssistantBoard*>(arg)->PlayLoop(); }
    void PlayLoop() {
        int16_t buf[256];
        while (true) {
            const size_t n = xStreamBufferReceive(play_buf_, buf, sizeof(buf), portMAX_DELAY);
            if (n >= sizeof(int16_t)) codec_.WritePcm(buf, n / sizeof(int16_t));
        }
    }

    // ---------- 摄像头：GC2145 ----------
    esp_err_t InitCamera() {
        camera_config_t cc = {};
        cc.pin_pwdn     = CAM_PIN_PWDN;
        cc.pin_reset    = CAM_PIN_RESET;
        cc.pin_xclk     = CAM_PIN_XCLK;
        cc.pin_sccb_sda = CAM_PIN_SIOD;
        cc.pin_sccb_scl = CAM_PIN_SIOC;
        cc.pin_d7       = CAM_PIN_D7;
        cc.pin_d6       = CAM_PIN_D6;
        cc.pin_d5       = CAM_PIN_D5;
        cc.pin_d4       = CAM_PIN_D4;
        cc.pin_d3       = CAM_PIN_D3;
        cc.pin_d2       = CAM_PIN_D2;
        cc.pin_d1       = CAM_PIN_D1;
        cc.pin_d0       = CAM_PIN_D0;
        cc.pin_vsync    = CAM_PIN_VSYNC;
        cc.pin_href     = CAM_PIN_HREF;
        cc.pin_pclk     = CAM_PIN_PCLK;
        cc.xclk_freq_hz = CAM_XCLK_FREQ_HZ;
        cc.ledc_timer   = LEDC_TIMER_0;
        cc.ledc_channel = LEDC_CHANNEL_0;
        cc.pixel_format = PIXFORMAT_RGB565;
        cc.frame_size   = FRAMESIZE_240X240;
        cc.fb_count     = 2;
        cc.fb_location  = CAMERA_FB_IN_PSRAM;
        cc.grab_mode    = CAMERA_GRAB_LATEST;
        esp_err_t r = esp_camera_init(&cc);
        if (r != ESP_OK) { ESP_LOGE(TAG, "esp_camera_init: %s", esp_err_to_name(r)); return r; }
        sensor_t* s = esp_camera_sensor_get();
        if (s) {
            ESP_LOGI(TAG, "camera sensor PID=0x%04x", s->id.PID);
            s->set_vflip(s, 0);
            s->set_hmirror(s, 0);
            RestartCaptureEngine(s);
        }
        cam_ok_ = true;
        ESP_LOGI(TAG, "camera ready (RGB565 240x240)");
        return ESP_OK;
    }

    static void RestartCaptureEngine(sensor_t* s) {
        if (!s->set_reg) return;
        s->set_reg(s, 0xfe, 0xff, 0x00);
        vTaskDelay(pdMS_TO_TICKS(20));
        s->set_reg(s, 0xfe, 0xff, 0x10);
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_LOGI(TAG, "GC2145 capture engine restarted");
    }

    static void LockExposureWhiteBalance(sensor_t* s) {
        if (!s->set_reg) return;
        s->set_reg(s, 0xfe, 0xff, 0x00);
        s->set_reg(s, 0xb6, 0x01, 0x00);
        s->set_reg(s, 0x82, 0x02, 0x00);
        ESP_LOGI(TAG, "AEC/AWB locked");
    }

    // ---------- 拍照上传 ----------
    void RegisterCaptureEndpoint() {
        static agent_link_io_desc_t desc = {};
        desc.id           = "camera0";
        desc.dir          = AGENT_IO_OUT;
        desc.kind         = "camera.capture";
        desc.value        = AGENT_VAL_BOOL;
        desc.desc         = "Capture a still image and upload it to the app";
        desc.display_name = "Camera Snapshot";
        agent_link_register_io(&desc, &AssistantBoard::OnCaptureCmd, this);
    }

    static void OnCaptureCmd(const char* /*id*/, const uint8_t* /*args*/, size_t /*len*/, void* ctx) {
        auto* self = static_cast<AssistantBoard*>(ctx);
        if (self) { self->capture_req_.store(true, std::memory_order_release); ESP_LOGI(TAG, "snapshot requested (App)"); }
    }


    void SendSnapshot(camera_fb_t* fb) {
        uint8_t* jpg = nullptr; size_t jpg_len = 0;
        if (!frame2jpg(fb, 80, &jpg, &jpg_len)) { ESP_LOGE(TAG, "snapshot: frame2jpg failed"); return; }
        SetLedColor(20, 20, 20);   // 白闪 = 拍照中
        esp_err_t r = agent_link_send_image(jpg, jpg_len, AGENT_IMG_JPEG,
                                            static_cast<uint16_t>(fb->width), static_cast<uint16_t>(fb->height));
        ESP_LOGI(TAG, "snapshot: %ux%u -> %uB jpeg, send=%s",
                 (unsigned)fb->width, (unsigned)fb->height, (unsigned)jpg_len, esp_err_to_name(r));
        free(jpg);
        SetLedColor(0, 0, 10);
    }

    static void PreviewTaskEntry(void* arg) { static_cast<AssistantBoard*>(arg)->PreviewLoop(); }

    // 摄像头实时预览：每帧显示到 LCD，并处理拍照请求
    void PreviewLoop() {
        ESP_LOGI(TAG, "preview loop started");
        uint32_t frames = 0;
        bool ae_locked = false;
        while (true) {
            camera_fb_t* fb = esp_camera_fb_get();
            if (!fb) { ESP_LOGW(TAG, "fb_get failed"); vTaskDelay(pdMS_TO_TICKS(10)); continue; }

#if CAMERA_MASK_TOP_ROWS > 0
            {
                uint8_t* b = static_cast<uint8_t*>(fb->buf);
                const size_t rb = static_cast<size_t>(fb->width) * 2u;
                for (int r = 0; r < CAMERA_MASK_TOP_ROWS; ++r)
                    memcpy(b + static_cast<size_t>(r) * rb, b + static_cast<size_t>(CAMERA_MASK_TOP_ROWS) * rb, rb);
            }
#endif

            // 拍照由云端 camera0 指令触发（A 板没有物理拍照键）
            if (capture_req_.exchange(false, std::memory_order_acq_rel)) SendSnapshot(fb);

            // Keep the live GC2145 preview visible during the interview as well.
            // The cloud still receives separate JPEG snapshots for trend analysis.
            (void)lcd_.DrawBitmap(0, 0, static_cast<uint16_t>(fb->width), static_cast<uint16_t>(fb->height), fb->buf);
            esp_camera_fb_return(fb);

            if (!ae_locked && ++frames >= 50) {
                sensor_t* s = esp_camera_sensor_get();
                if (s) { LockExposureWhiteBalance(s); ae_locked = true; }
            }
        }
    }

    // ---------- PTT：按住 BOOT 说话 ----------
    void InitPttButton() {
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << BUTTON_BOOT_PIN;
        c.mode         = GPIO_MODE_INPUT;
        c.pull_up_en   = GPIO_PULLUP_ENABLE;
        c.pull_down_en = GPIO_PULLDOWN_DISABLE;
        c.intr_type    = GPIO_INTR_DISABLE;
        gpio_config(&c);
        ESP_LOGI(TAG, "PTT ready: hold GPIO%d to talk", (int)BUTTON_BOOT_PIN);
    }

    static void PttTaskEntry(void* arg) { static_cast<AssistantBoard*>(arg)->PttLoop(); }

    void PttLoop() {
        constexpr size_t kFrame = AUDIO_SAMPLE_RATE / 50;
        int16_t buf[kFrame];
        bool talking = false;
        TickType_t talk_started_tick = 0;
        while (true) {
            const int pressed = (gpio_get_level(BUTTON_BOOT_PIN) == 0);
            if (!talking) {
                if (!pressed) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
                vTaskDelay(pdMS_TO_TICKS(20));
                if (gpio_get_level(BUTTON_BOOT_PIN) != 0) continue;
                if (agent_link_state() != AGENT_STATE_READY) {
                    ESP_LOGW(TAG, "PTT pressed but Agent not connected, ignoring");
                    while (gpio_get_level(BUTTON_BOOT_PIN) == 0) vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                if (codec_.StartMic() != ESP_OK) { ESP_LOGE(TAG, "mic start failed"); continue; }
                talking = true;
                talk_started_tick = xTaskGetTickCount();
                SetLedColor(20, 0, 0);   // 红=聆听
                ESP_LOGI(TAG, "PTT pressed, starting voice uplink");
            } else {
                if (agent_link_state() != AGENT_STATE_READY) {
                    agent_link_voice_end(); codec_.StopMic(); talking = false;
                    SetLedColor(0, 0, 10);
                    ESP_LOGW(TAG, "connection lost, ending voice");
                    continue;
                }
                size_t got = 0;
                if (codec_.ReadPcm(buf, kFrame, &got) == ESP_OK && got > 0) {
                    const esp_err_t r = agent_link_push_voice(
                        reinterpret_cast<const uint8_t*>(buf), got * sizeof(int16_t));
                    if (r != ESP_OK) {
                        ESP_LOGW(TAG, "voice upload stopped: %s", esp_err_to_name(r));
                        agent_link_voice_end();
                        codec_.StopMic();
                        talking = false;
                        SetLedColor(0, 0, 10);
                    }
                }
                if (gpio_get_level(BUTTON_BOOT_PIN) != 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    if (gpio_get_level(BUTTON_BOOT_PIN) != 0) {
                        agent_link_voice_end(); codec_.StopMic(); talking = false;
                        SetLedColor(0, 0, 10);
                        const uint32_t duration_ms = talk_started_tick > 0
                            ? static_cast<uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount() - talk_started_tick))
                            : 0;
                        char meta[192];
                        const bool is_topic = !topic_captured_.exchange(true, std::memory_order_acq_rel);
                        const int n = snprintf(meta, sizeof(meta),
                            "{\"type\":\"%s\",\"question_index\":1,\"duration_ms\":%u,\"quality_hint\":\"%s\"}",
                            is_topic ? "interview_topic" : "interview_answer",
                            static_cast<unsigned>(duration_ms),
                            duration_ms < 1500 ? "too_short" : (duration_ms > 120000 ? "too_long" : "normal"));
                        if (interview_mode_.load(std::memory_order_acquire) && n > 0)
                            (void)agent_link_push_event(AGENT_EVT_CUSTOM,
                                reinterpret_cast<const uint8_t*>(meta), static_cast<size_t>(n));
                        ESP_LOGI(TAG, "interview %s ended: duration=%ums",
                                 is_topic ? "topic" : "answer", (unsigned)duration_ms);
                        ESP_LOGI(TAG, "PTT released, ending voice");
                    }
                }
            }
        }
    }

    void InitInterviewButton() {
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << BUTTON_INTERVIEW_PIN;
        c.mode = GPIO_MODE_INPUT;
        c.pull_up_en = GPIO_PULLUP_DISABLE;
        c.pull_down_en = GPIO_PULLDOWN_DISABLE;
        c.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&c);
        xTaskCreate(&AssistantBoard::InterviewButtonTaskEntry, "interview_btn", 2048, this, 5, nullptr);
        ESP_LOGI(TAG, "interview button ready: press GPIO%d", (int)BUTTON_INTERVIEW_PIN);
    }

    static void InterviewButtonTaskEntry(void* arg) {
        auto* self = static_cast<AssistantBoard*>(arg);
        int idle_level = gpio_get_level(BUTTON_INTERVIEW_PIN);
        if (idle_level < 0) idle_level = 1;
        bool changed = false;
        while (true) {
            const int level = gpio_get_level(BUTTON_INTERVIEW_PIN);
            const bool pressed = level >= 0 && level != idle_level;
            if (pressed && !changed) {
                vTaskDelay(pdMS_TO_TICKS(30));
                if (gpio_get_level(BUTTON_INTERVIEW_PIN) != idle_level) {
                    if (agent_link_state() == AGENT_STATE_READY) self->BeginInterview();
                    else ESP_LOGW(TAG, "interview button ignored: Agent not ready");
                }
            }
            changed = pressed;
            if (!pressed) changed = false;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    static constexpr size_t kPlayBufBytes = 16 * 1024;

    Es8311Codec          codec_;
    St7789Lcd            lcd_;
    Ws2812Led            led_;
    bool                 codec_ok_  = false;
    bool                 lcd_ok_    = false;
    bool                 cam_ok_    = false;
    bool                 speaking_shown_ = false;
    StreamBufferHandle_t play_buf_  = nullptr;
    TaskHandle_t         play_task_ = nullptr;
    TaskHandle_t         ptt_task_  = nullptr;
    TaskHandle_t         preview_task_ = nullptr;
    std::atomic<bool>    capture_req_{false};
    std::atomic<bool>    interview_mode_{false};
    std::atomic<bool>    topic_captured_{false};
    std::atomic<bool>    interview_start_scheduled_{false};
};

DECLARE_BOARD(AssistantBoard);
