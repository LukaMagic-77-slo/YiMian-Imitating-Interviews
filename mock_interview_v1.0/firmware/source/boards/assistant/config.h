#ifndef _AGENT_LINK_BOARD_CONFIG_H_
#define _AGENT_LINK_BOARD_CONFIG_H_

#include <driver/gpio.h>

// ============ 音频：ES8311 codec（麦克风 + 喇叭） ============
#define AUDIO_I2S_MCLK          GPIO_NUM_46
#define AUDIO_I2S_BCLK          GPIO_NUM_39
#define AUDIO_I2S_WS            GPIO_NUM_2
#define AUDIO_I2S_DOUT          GPIO_NUM_38   // 喇叭
#define AUDIO_I2S_DIN           GPIO_NUM_40   // 麦克风
#define AUDIO_PA_EN             GPIO_NUM_NC
#define AUDIO_I2C_SDA           GPIO_NUM_41
#define AUDIO_I2C_SCL           GPIO_NUM_42
#define ES8311_ADDR             0x18
#define AUDIO_SAMPLE_RATE       16000

// ============ 按键 ============
#define BUTTON_BOOT_PIN         GPIO_NUM_0    // BOOT 键：按住说话（低电平=按下）
#define BUTTON_INTERVIEW_PIN    GPIO_NUM_3    // 轻触按键：进入面试官模式（低电平=按下）

// ============ LCD：ST7789 240x240 ============
#define DISPLAY_SPI_HOST        SPI2_HOST
#define DISPLAY_SCK_PIN         GPIO_NUM_21
#define DISPLAY_MOSI_PIN        GPIO_NUM_47
#define DISPLAY_DC_PIN          GPIO_NUM_43
#define DISPLAY_CS_PIN          GPIO_NUM_44
#define DISPLAY_RST_PIN         (-1)
#define DISPLAY_BL_PIN          (-1)
#define DISPLAY_WIDTH           240
#define DISPLAY_HEIGHT          240
#define DISPLAY_SPI_CLK_HZ      (40 * 1000 * 1000)
#define DISPLAY_SPI_MODE        0

// ============ 摄像头：GC2145 DVP ============
#define CAM_PIN_PWDN            -1
#define CAM_PIN_RESET           -1
#define CAM_PIN_XCLK            GPIO_NUM_15
#define CAM_PIN_SIOD            GPIO_NUM_4
#define CAM_PIN_SIOC            GPIO_NUM_5
#define CAM_PIN_D7              GPIO_NUM_16
#define CAM_PIN_D6              GPIO_NUM_17
#define CAM_PIN_D5              GPIO_NUM_18
#define CAM_PIN_D4              GPIO_NUM_12
#define CAM_PIN_D3              GPIO_NUM_10
#define CAM_PIN_D2              GPIO_NUM_8
#define CAM_PIN_D1              GPIO_NUM_9
#define CAM_PIN_D0              GPIO_NUM_11
#define CAM_PIN_VSYNC           GPIO_NUM_6
#define CAM_PIN_HREF            GPIO_NUM_7
#define CAM_PIN_PCLK            GPIO_NUM_13
#define CAM_XCLK_FREQ_HZ        (20 * 1000 * 1000)
#define CAMERA_MASK_TOP_ROWS    8

// ============ WS2812 彩灯 ============
#define WS2812_LED_PIN          GPIO_NUM_48

#endif  // _AGENT_LINK_BOARD_CONFIG_H_
