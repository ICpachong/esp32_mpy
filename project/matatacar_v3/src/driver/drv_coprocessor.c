#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "string.h"
#include "driver/gpio.h"
#include "drv_coprocessor.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_err.h"
#include "nvs.h"
#include "drv_nvs.h"

static const char *TAG = "DRV_COPROCESSOR";

#define RX_BUF_SIZE    128
#define TXD_PIN (GPIO_NUM_43)
#define RXD_PIN (GPIO_NUM_44)


sensor_data_type sensor_value = {0};

uint8_t frame_buffer[RX_BUF_SIZE];

void drv_coprpcessor_init(void)
{
    const uart_config_t uart_config =
    {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_driver_install(UART_NUM_0, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_config);
    uart_set_pin(UART_NUM_0, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    sensor_value.is_auto_color = true;
    sensor_value.ir_threshold = 400;
}

void drv_coprpcessor_printf(char *fmt,...)
{
    va_list ap;
    char string[64]; 
    va_start(ap,fmt);
    vsprintf(string,fmt,ap);
    uart_write_bytes(UART_NUM_0, (uint8_t*)string, strlen(string));
    va_end(ap);
}

uint16_t read_uint16_t_data(int idx)
{
    val2byte.byteVal[0] = frame_buffer[2 + idx];
    val2byte.byteVal[1] = frame_buffer[3 + idx];
    return val2byte.shortVal;
}

void parse_command(void)
{
    sensor_value.left_ir = read_uint16_t_data(0);
    sensor_value.right_ir = read_uint16_t_data(2);
    sensor_value.color_sensor_off = read_uint16_t_data(4);
    sensor_value.r_value = read_uint16_t_data(6);
    sensor_value.g_value = read_uint16_t_data(8);
    sensor_value.b_value = read_uint16_t_data(10);
}

void drv_coprpcessor_task(void *arg)
{
    static bool is_frame_start = false;
    static uint8_t prevdata = 0;
    static uint8_t frame_index = 0;
    uint8_t *data = (uint8_t*) malloc(RX_BUF_SIZE + 1);
    unsigned int  size = 0;
    // drv_coprpcessor_init();
    while (1)
    {
        uart_get_buffered_data_len(UART_NUM_0, &size);
        if(size > 0)
        {
            const int rxBytes = uart_read_bytes(UART_NUM_0, data, RX_BUF_SIZE, 5 / portTICK_RATE_MS);
            if (rxBytes > 0) 
            {
                for(uint8_t i = 0; i < rxBytes; i++)
                {
                    if((data[i] == 0xfe) && (is_frame_start == false))
                    {
                        if(prevdata == 0xff)
                        {
                            frame_buffer[frame_index++] = 0xff;
                            frame_buffer[frame_index++] = 0xfe;
                            is_frame_start = true;
                        }
                    }
                    else
                    {
                        prevdata = data[i];
                        if(is_frame_start)
                        {
                            frame_buffer[frame_index++] = data[i];
                            if(frame_index > 64)
                            {
                                frame_index = 0;
                                prevdata = 0;
                                is_frame_start = false;
                            }
                        }
                    }
                }
            }
        }
        else
        {
            vTaskDelay(5 / portTICK_PERIOD_MS);
        }
        if(frame_index >= 14)
        {
            parse_command();
            frame_index = 0;
            prevdata = 0;
            is_frame_start = false;
        }
    }
}

esp_err_t get_color_sensor_calibration_value()
{
    esp_err_t ret = ESP_OK;
    nvs_handle_t my_handle;
    size_t len = NVS_STRING_LENGTH_MAX;
    char *namespace = "user_config";

    char value_buffer[NVS_STRING_LENGTH_MAX];
    memset(value_buffer, 0, NVS_STRING_LENGTH_MAX);
    if(!is_nvs_initialized())
    {
        nvs_init();
    }
    //Open   
    ret = nvs_open(namespace, NVS_READONLY, &my_handle);
    if(ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(ret));
        return ret;
    } 
    else 
    {
        size_t len = NVS_STRING_LENGTH_MAX;
        ret = nvs_get_str(my_handle, "color_red", value_buffer, &len);
        color_red_cali = atoi(value_buffer);

        memset(value_buffer, 0, NVS_STRING_LENGTH_MAX);
        ret = nvs_get_str(my_handle, "color_green", value_buffer, &len);
        color_green_cali = atoi(value_buffer);

        memset(value_buffer, 0, NVS_STRING_LENGTH_MAX);
        ret = nvs_get_str(my_handle, "color_blue", value_buffer, &len);
        color_blue_cali = atoi(value_buffer);
        memset(value_buffer, 0, NVS_STRING_LENGTH_MAX);
        ret = nvs_get_str(my_handle, "bri_ratio", value_buffer, &len);
        color_bri_ratio_cali = atoi(value_buffer);
        if(ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Error (%s) read to nv flash!\n", esp_err_to_name(ret));
            nvs_close(my_handle);
            return ret;
        }
        else
        {
            nvs_close(my_handle);
            return ret;
        }
    }
}

void get_color_offset(int16_t *red, int16_t *green, int16_t *blue)
{
    *red = sensor_value.r_value - 220;
    *green = sensor_value.g_value - 160;
    *blue = sensor_value.b_value - 160;
    if(*red < 0)
    {
        red = 0;
    }
    if(*green < 0)
    {
        *green = 0;
    }
    if(*blue < 0)
    {
        *blue = 0;
    }
}

void light_driver_rgb2hsv(uint16_t red, uint16_t green, uint16_t blue,
                                 uint16_t *h, uint16_t *s, uint16_t *v)
{
    float r, g, b;
    float h_value, s_value, v_value;
    h_value = 0;
    uint16_t max_rgb =  MAX(red, MAX(green, blue));
    r = red * 1.0f / max_rgb;
    g = green * 1.0f / max_rgb;
    b = blue * 1.0f / max_rgb;
    float mx =  MAX(r, MAX(g, b));
    float mn =  MIN(r, MIN(g, b));
    float m = mx - mn;
    if(mx == mn)
    {
        h_value = 0;
    
    }
    else if(mx == r)
    {
        if (g >= b)
        {
            h_value = ((g - b) / m) * 60;
        }
        else
        {
            h_value = ((g-b)/m) * 60 + 360;
        } 
    }
    else if(mx == g)
    {
        h_value = ((b-r)/m) * 60 + 120;
    }
    else if(mx == b)
    {
        h_value = ((r-g)/m) * 60 + 240;
    }

    if(mx == 0)
    {
        s_value = 0;
    }
    else
    {
        s_value = m/mx;
    }
    v_value = mx;

    *h = (int)(h_value / 2);
    *s = (int)(s_value * 255.0);
    if(max_rgb > 255)
    {
        max_rgb = 255;
    }
    *v = max_rgb;
}

void get_color(uint16_t *red, uint16_t *green, uint16_t *blue)
{
    int16_t red_value;
    int16_t green_value;
    int16_t blue_value;
    get_color_offset(&red_value, &green_value, &blue_value);
    *red = (red_value * color_red_cali * 1.0) / color_bri_ratio_cali;
    *green = (green_value * color_green_cali * 1.0) / color_bri_ratio_cali;
    *blue = (blue_value * color_blue_cali * 1.0) / color_bri_ratio_cali;

}

uint8_t drv_get_color_id()
{
    uint16_t red_value;
    uint16_t green_value;
    uint16_t blue_value;
    get_color(&red_value, &green_value, &blue_value);
    
    if((red_value == 0) && (green_value == 0) && (blue_value == 0))
    {
        return COLOR_BLACK;
    }
    else
    {
        uint16_t h;
        uint16_t s;
        uint16_t v;
        light_driver_rgb2hsv(red_value, green_value, blue_value, &h, &s, &v);
        if((v > 220) && (s < 30) && (h < 180))
        {
            return COLOR_WHITE;
        }
        else if((v < 46) && (s < 255) && (h < 180))
        {
            return COLOR_BLACK;
        }
        else if(((v > 46) && (v < 220) && (s < 43) && (h < 180)))
        {
            return COLOR_GREY;
        }
        else if((v > 46) && (s > 43) && ((h < 10) || ((h > 156) && (h < 180))))
        {
            return COLOR_RED;
        }
        // else if((v > 46) && (s > 43) && (h > 11) && (h < 25))
        // {
        //     return COLOR_ORANGE;
        // }
        else if((v > 46) && (s > 43) && (h > 11) && (h < 34))
        {
            return COLOR_YELLOW;
        }
        else if((v > 46) && (s > 43) && (h > 35) && (h < 77))
        {
            return COLOR_GREEN;
        }
        // else if((v > 46) && (s > 43) && (h > 78) && (h < 99))
        // {
        //     return COLOR_CYAN;
        // }
        else if((v > 46) && (s > 43) && (h > 78) && (h < 124))
        {
            return COLOR_BLUE;
        }
        else if((v > 46) && (s > 43) && (h > 125) && (h < 155))
        {
            return COLOR_PURPLE;
        }
        else
        {
            return COLOR_UNKNOWN;
        }
    }
}