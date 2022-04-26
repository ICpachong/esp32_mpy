/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2019 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdio.h>
#include <string.h>

#include "py/objstr.h"
#include "py/runtime.h"
#include "py/mphal.h"

#include "audio_hal.h"
#include "audio_pipeline.h"
#include "board.h"

#include "i2s_stream.h"
#include "vfs_stream.h"

#include "amrnb_encoder.h"

#include "esp_err.h"
#include "esp_log.h"
#include "audio_element.h"

static const char *TAG = "audio_recorder.c";

#define SYNC_RECORDER_MAX_TIMEOUT      30

typedef struct _audio_recorder_obj_t
{
    mp_obj_base_t base;

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream;
    audio_element_handle_t encoder;
    audio_element_handle_t out_stream;

    esp_timer_handle_t timer;
    mp_obj_t end_cb;
} audio_recorder_obj_t;


static bool esp_audio_recorder_running = false;

STATIC mp_obj_t audio_recorder_stop(mp_obj_t self_in);

STATIC void audio_recorder_init(audio_recorder_obj_t *self)
{
    // init audio board
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    // pipeline
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    self->pipeline = audio_pipeline_init(&pipeline_cfg);
    // I2S
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_cfg.uninstall_drv = false;
    i2s_cfg.i2s_config.sample_rate = 8000;
    i2s_cfg.i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT;
    i2s_cfg.task_core = 0;
    i2s_cfg.task_prio = 23;
    self->i2s_stream = i2s_stream_init(&i2s_cfg);

    // encoder
    amrnb_encoder_cfg_t amr_enc_cfg = DEFAULT_AMRNB_ENCODER_CONFIG();
    amr_enc_cfg.task_core = 0;
    amr_enc_cfg.out_rb_size = 2 * 1024;
    self->encoder = amrnb_encoder_init(&amr_enc_cfg);

    // out stream
    vfs_stream_cfg_t vfs_cfg = VFS_STREAM_CFG_DEFAULT();
    vfs_cfg.type = AUDIO_STREAM_WRITER;
    vfs_cfg.task_core = 0;
    vfs_cfg.task_prio = 4;
    self->out_stream = vfs_stream_init(&vfs_cfg);

    // register to pipeline
    audio_pipeline_register(self->pipeline, self->i2s_stream, "i2s");
    audio_pipeline_register(self->pipeline, self->encoder, "encoder");
    audio_pipeline_register(self->pipeline, self->out_stream, "out");

    ESP_LOGW(TAG, "audio_pipeline_init done");
}

STATIC mp_obj_t audio_recorder_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    audio_recorder_obj_t *self = m_new_obj_with_finaliser(audio_recorder_obj_t);
    self->base.type = type;
    audio_recorder_init(self);
    return MP_OBJ_FROM_PTR(self);
}

static void audio_recorder_maxtime_cb(void *arg)
{
    audio_recorder_stop(arg);
    audio_recorder_obj_t *self = (audio_recorder_obj_t *)arg;
    if (self->end_cb != mp_const_none)
    {
        mp_sched_schedule(self->end_cb, self);
    }
}

STATIC mp_obj_t audio_recorder_start(mp_uint_t n_args, const mp_obj_t *args_in, mp_map_t *kw_args)
{
    enum 
    {
        ARG_uri,
        ARG_maxtime,
        ARG_endcb
    };
    static const mp_arg_t allowed_args[] = 
    {
        { MP_QSTR_uri, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_maxtime, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_endcb, MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    audio_recorder_obj_t *self = args_in[0];
    i2s_stream_set_clk(self->i2s_stream, 8000, 16, 1);
    mp_arg_parse_all(n_args - 1, args_in + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    ESP_LOGW(TAG, "audio_recorder_start");
    const char *link_tag[3] = {"i2s", "encoder", "out"};
    audio_pipeline_link(self->pipeline, &link_tag[0], 3);
    audio_element_set_uri(self->out_stream, mp_obj_str_get_str(args[ARG_uri].u_obj));

    if (audio_pipeline_run(self->pipeline) == ESP_OK) 
    {
        esp_audio_recorder_running = true;
        if (args[ARG_maxtime].u_int > 0) 
        {
            esp_timer_create_args_t timer_conf = 
            {
                .callback = &audio_recorder_maxtime_cb,
                .name = "maxtime",
                .arg = self,
            };
            esp_timer_create(&timer_conf, &self->timer);
            esp_timer_start_once(self->timer, args[ARG_maxtime].u_int * 1000000);
            self->end_cb = args[ARG_endcb].u_obj;
        }
        return mp_obj_new_bool(true);
    } 
    else 
    {
        ESP_LOGW(TAG, "recorder audio_pipeline_run error");
        esp_audio_recorder_running = false;
        return mp_obj_new_bool(false);
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(audio_recorder_start_obj, 1, audio_recorder_start);

// static esp_err_t _priv_i2s_read(audio_element_handle_t el, char* buffer, size_t len, TickType_t ticks)
// {
//     return 0;
// }

STATIC mp_obj_t audio_recorder_stop(mp_obj_t self_in)
{
    audio_recorder_obj_t *self = self_in;
    int count_time = 0;
    int st_count = 0;
    if (self->timer) 
    {
        esp_timer_stop(self->timer);
        esp_timer_delete(self->timer);
        self->timer = NULL;
    }
    if (esp_audio_recorder_running == true) 
    {   
        stream_func i2s_default_cb = audio_element_get_read_cb(self->i2s_stream);
        //esp_err_t ret = audio_element_set_read_cb(self->i2s_stream,  _priv_i2s_read, 0);
        esp_err_t ret = audio_element_set_read_cb(self->i2s_stream, i2s_default_cb, 0);
        while (true)
        {
            int st = audio_element_size(self->i2s_stream, 1);
            ret = audio_pipeline_stop(self->pipeline);
            ESP_LOGW(TAG, "recorder audio_pipeline_stop = %d\n", ret);
            ret = audio_pipeline_wait_for_stop(self->pipeline);
            ESP_LOGW(TAG, "recorder audio_pipeline_wait_for_stop = %d\n", ret);
            ret = audio_pipeline_terminate(self->pipeline);
            ESP_LOGW(TAG, "recorder audio_pipeline_terminate = %d\n", ret);
            ret = audio_pipeline_unlink(self->pipeline);
            ESP_LOGW(TAG, "recorder audio_pipeline_unlink = %d\n", ret);
            break;
            if(st == 0)
            {
                if(st_count > 5)
                {
                    ret = audio_pipeline_stop(self->pipeline);
                    ESP_LOGW(TAG, "recorder audio_pipeline_stop = %d\n", ret);
                    ret = audio_pipeline_wait_for_stop(self->pipeline);
                    ESP_LOGW(TAG, "recorder audio_pipeline_wait_for_stop = %d\n", ret);
                    ret = audio_pipeline_terminate(self->pipeline);
                    ESP_LOGW(TAG, "recorder audio_pipeline_terminate = %d\n", ret);
                    ret = audio_pipeline_unlink(self->pipeline);
                    ESP_LOGW(TAG, "recorder audio_pipeline_unlink = %d\n", ret);
                    break;
                }
                st_count++;
            }
            if((count_time / 20)  > SYNC_RECORDER_MAX_TIMEOUT)
            {
                ret = audio_pipeline_stop(self->pipeline);
                ESP_LOGW(TAG, "audio_recorder_stop \n");
                ESP_LOGW(TAG, "recorder audio_pipeline_stop = %d\n", ret);
                ret = audio_pipeline_wait_for_stop(self->pipeline);
                ESP_LOGW(TAG, "recorder audio_pipeline_wait_for_stop = %d\n", ret);
                ret = audio_pipeline_terminate(self->pipeline);
                ESP_LOGW(TAG, "recorder audio_pipeline_terminate = %d\n", ret);
                ret = audio_pipeline_unlink(self->pipeline);
                ESP_LOGW(TAG, "recorder audio_pipeline_unlink = %d\n", ret);      
                break;
            }
            mp_hal_delay_ms(50);
            count_time++;
        }
        i2s_stream_set_clk(self->i2s_stream, 48000, 32, 2);
        esp_audio_recorder_running = false; 
    }
    return mp_obj_new_bool(true);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(audio_recorder_stop_obj, audio_recorder_stop);

STATIC mp_obj_t audio_recorder_is_running(mp_obj_t self_in)
{
    return mp_obj_new_bool(esp_audio_recorder_running);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(audio_recorder_is_running_obj, audio_recorder_is_running);

STATIC const mp_rom_map_elem_t recorder_locals_dict_table[] =
{
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&audio_recorder_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audio_recorder_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_running), MP_ROM_PTR(&audio_recorder_is_running_obj) },
};

STATIC MP_DEFINE_CONST_DICT(recorder_locals_dict, recorder_locals_dict_table);

const mp_obj_type_t audio_recorder_type =
{
    { &mp_type_type },
    .name = MP_QSTR_recorder,
    .make_new = audio_recorder_make_new,
    .locals_dict = (mp_obj_dict_t *)&recorder_locals_dict,
};
