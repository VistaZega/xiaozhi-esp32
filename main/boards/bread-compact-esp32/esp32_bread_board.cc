#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "display/oled_display.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_sh1106.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_random.h>
#include <esp_http_client.h>
#include <esp_wifi.h>

#define TAG "ESP32-MarsbearSupport"

class CompactWifiBoard : public WifiBoard {
private:
    Button boot_button_;
    Button touch_button_;
    Button asr_button_;

    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    TaskHandle_t anim_task_handle_ = nullptr;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = GPIO_NUM_21,
            .scl_io_num = GPIO_NUM_22,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SH1106 / I2C config (100kHz standard speed for stable communication)
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 100 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SH1106 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SH1106 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    // Auto-blink & Dynamic Face Animation Loop Task
    void StartFaceAnimationTask() {
        xTaskCreate([](void* arg) {
            CompactWifiBoard* board = static_cast<CompactWifiBoard*>(arg);
            uint32_t next_blink_tick = xTaskGetTickCount() + pdMS_TO_TICKS(2000 + (esp_random() % 3000));
            
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(100));
                
                if (board->display_ == nullptr) continue;
                
                auto& app = Application::GetInstance();
                DeviceState state = app.GetDeviceState();
                
                // Kedipan Alami saat Robot dalam status Idle / Standby
                if (state == kDeviceStateIdle) {
                    uint32_t current_tick = xTaskGetTickCount();
                    if (current_tick >= next_blink_tick) {
                        board->display_->SetEmotion("blink");
                        vTaskDelay(pdMS_TO_TICKS(150));
                        board->display_->SetEmotion("neutral");
                        next_blink_tick = current_tick + pdMS_TO_TICKS(2500 + (esp_random() % 3500));
                    }
                }
            }
        }, "face_anim", 4096, this, 1, &anim_task_handle_);
    }

    void InitializeButtons() {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << BUILTIN_LED_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io_conf);

        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            gpio_set_level(BUILTIN_LED_GPIO, 1);
            app.ToggleChatState();
        });

        asr_button_.OnClick([this]() {
            std::string wake_word="你好小智";
            Application::GetInstance().WakeWordInvoke(wake_word);
        });

        touch_button_.OnPressDown([this]() {
            gpio_set_level(BUILTIN_LED_GPIO, 1);
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            gpio_set_level(BUILTIN_LED_GPIO, 0);
            Application::GetInstance().StopListening();
        });
    }

    // Fungsi C++ mengambil data langsung dari perguruanpembda.com/api_xiaozhi.php
    static std::string FetchPembdaApi() {
        esp_http_client_config_t config = {};
        config.url = "http://perguruanpembda.com/api_xiaozhi.php";
        config.timeout_ms = 4000;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        std::string response_data = "";

        if (esp_http_client_perform(client) == ESP_OK) {
            char buffer[512];
            int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
            if (read_len > 0) {
                buffer[read_len] = 0;
                response_data = std::string(buffer);
            }
        }
        esp_http_client_cleanup(client);
        
        if (response_data.empty()) {
            return "Data resmi dari Perguruan PEMBDA Nias: Pendaftaran siswa baru telah dibuka di perguruanpembda.com.";
        }
        return response_data;
    }

    // Mendaftarkan Custom Tool ke MCP Server internal ESP32
    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
        
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.pembda.get_info", "Ambil data resmi realtime dari PembdaHUB Yayasan Perguruan PEMBDA Nias (perguruanpembda.com)", PropertyList(), 
        [](const PropertyList& properties) -> ReturnValue {
            return FetchPembdaApi();
        });
    }

public:
    CompactWifiBoard() : WifiBoard(), boot_button_(BOOT_BUTTON_GPIO), touch_button_(TOUCH_BUTTON_GPIO), asr_button_(ASR_BUTTON_GPIO)
    {
        // Paksa Wi-Fi selalu aktif 100% tanpa sleep saat standby
        esp_wifi_set_ps(WIFI_PS_NONE);

        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeTools();
        StartFaceAnimationTask();
    }

    virtual AudioCodec* GetAudioCodec() override 
    {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

};

DECLARE_BOARD(CompactWifiBoard);
