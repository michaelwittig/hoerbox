#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "filter_resample.h"

#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include <driver/i2c.h>
#include "board.h"
#include "periph_button.h"
#include "input_key_service.h"
#include "periph_adc_button.h"
#include "sdcard_scan.h"

#include "rc522.h"

static const char *TAG = "TONIE";

audio_pipeline_handle_t pipeline;
audio_element_handle_t fatfs_stream_reader;

static void rfid_task(void *arg) {
    ESP_ERROR_CHECK(rc522_init());
    char no[11];
    char previous_no[11];
    char sound_file[23];
    previous_no[0] = 0;
    while(true) {
        uint8_t* tag = rc522_get_tag();
        no[0] = 0;
        if(tag != NULL) {
            sprintf(no, "%02x%02x%02x%02x%02x", tag[0], tag[1], tag[2], tag[3], tag[4]);
            sprintf(sound_file, "/sdcard/%s.mp3", no);
            /*printf("serial: ");
            for(int i = 0; i < 5; i++) {
                printf("%#x ", tag[i]);
            }
            printf("\n");*/
            ESP_LOGI(TAG, "RFID tag found: %s", no);
            free(tag);
        } else {
            ESP_LOGI(TAG, "RFID tag not found");
        }
        if (strcmp(previous_no, no) != 0) {
            strcpy(previous_no, no);
            audio_pipeline_terminate(pipeline);
            if (no[0] == 0) {
                ESP_LOGI(TAG, "Stop");
            } else {
                ESP_LOGI(TAG, "Play: %s", sound_file);
                audio_element_set_uri(fatfs_stream_reader, sound_file);
                audio_pipeline_reset_ringbuffer(pipeline);
                audio_pipeline_reset_elements(pipeline);
                audio_pipeline_run(pipeline);
            }
        }
        vTaskDelay(2000 / portTICK_RATE_MS);
    }

    ESP_ERROR_CHECK(rc522_clear());

    vTaskDelete(NULL);
}

static void sound_task(void *arg) {
    audio_element_handle_t i2s_stream_writer, mp3_decoder;

    ESP_LOGI(TAG, "Initialize peripherals management");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    ESP_LOGI(TAG, "Initialize and start peripherals");
    audio_board_key_init(set);
    audio_board_sdcard_init(set);

    ESP_LOGI(TAG, "Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "Create fatfs stream to read data from sdcard");
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);

    ESP_LOGI(TAG, "Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "Create mp3 decoder to decode mp3 file");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder = mp3_decoder_init(&mp3_cfg);

    ESP_LOGI(TAG, "Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, fatfs_stream_reader, "file");
    audio_pipeline_register(pipeline, mp3_decoder, "mp3");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "Link it together [sdcard]-->fatfs_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
    audio_pipeline_link(pipeline, (const char *[]) {"file", "mp3", "i2s"}, 3);

    ESP_LOGI(TAG, "Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    xTaskCreate(rfid_task, "RFID", 2048, NULL, configMAX_PRIORITIES - 3, NULL);

    ESP_LOGI(TAG, "Listen for all pipeline events");
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Event interface error : %d", ret);
            continue;
        }
        ESP_LOGI(TAG, "Event received [source_type: %d, cmd: %d]", msg.source_type, msg.cmd);

        // volume down
        if ((int)msg.data == get_input_rec_id() && msg.cmd == PERIPH_BUTTON_RELEASE) {
            int player_volume;
            audio_hal_get_volume(board_handle->audio_hal, &player_volume);
            player_volume -= 3;
            if (player_volume < 0) {
                player_volume = 0;
            }
            audio_hal_set_volume(board_handle->audio_hal, player_volume);
            ESP_LOGI(TAG, "Volume decreased to %d %%", player_volume);
            continue;
        }

        // start: rewind
        if ((int)msg.data == get_input_rec_id() && msg.cmd == PERIPH_BUTTON_LONG_PRESSED) {

            ESP_LOGI(TAG, "rewind start");
            continue;
        }

        // stop: rewind
        if ((int)msg.data == get_input_rec_id() && msg.cmd == PERIPH_BUTTON_LONG_RELEASE) {

            ESP_LOGI(TAG, "rewind stop");
            continue;
        }

        // volume up
        if ((int)msg.data == get_input_mode_id() && msg.cmd == PERIPH_BUTTON_RELEASE) {
            int player_volume;
            audio_hal_get_volume(board_handle->audio_hal, &player_volume);
            player_volume += 3;
            if (player_volume > 100) {
                player_volume = 100;
            }
            audio_hal_set_volume(board_handle->audio_hal, player_volume);
            ESP_LOGI(TAG, "Volume increased to %d %%", player_volume);
            continue;
        }

        // start: fast-forward
        if ((int)msg.data == get_input_mode_id() && msg.cmd == PERIPH_BUTTON_LONG_PRESSED) {
            
            ESP_LOGI(TAG, "fast-forward start");
            continue;
        }

        // stop: fast-forward
        if ((int)msg.data == get_input_mode_id() && msg.cmd == PERIPH_BUTTON_LONG_RELEASE) {
            
            ESP_LOGI(TAG, "fast-forward stop");
            continue;
        }

        // start file
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) mp3_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder, &music_info);

            ESP_LOGI(TAG, "Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                    music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(i2s_stream_writer, &music_info);
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        // end of file
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "End of MP3");
            continue;
        }
    }

    ESP_LOGI(TAG, "Stop audio_pipeline");
    audio_pipeline_terminate(pipeline);

    audio_pipeline_unregister(pipeline, fatfs_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, mp3_decoder);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);

    /* Stop all periph before removing the listener */
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(fatfs_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(mp3_decoder);
    esp_periph_set_destroy(set);

    vTaskDelete(NULL);
}

static void i2cscanner_task(void *arg) {
    i2c_config_t conf;
    conf.mode = I2C_MODE_MASTER;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = 100000;
    ESP_ERROR_CHECK(get_i2c_pins(I2C_NUM_0, &conf));

    ESP_ERROR_CHECK(i2c_param_config(I2C_NUM_0, &conf));

    ESP_ERROR_CHECK(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0));

    int i;
    esp_err_t espRc;
    printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\n");
    printf("00:         ");
    for (i=3; i< 0x78; i++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (i << 1) | I2C_MASTER_WRITE, 1 /* expect ack */);
        i2c_master_stop(cmd);

        espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
        if (i%16 == 0) {
            printf("\n%.2x:", i);
        }
        if (espRc == 0) {
            printf(" %.2x", i);
        } else {
            printf(" --");
        }
        //ESP_LOGD(tag, "i=%d, rc=%d (0x%x)", i, espRc, espRc);
        i2c_cmd_link_delete(cmd);
    }
    printf("\n");
    vTaskDelete(NULL);
}

static void list_sdcard_db(void *user_data, char *url)
{
    printf("%s\n", url);
}

static void list_sdcard_task(void *arg) {
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    audio_board_sdcard_init(set);

    sdcard_scan(list_sdcard_db, "/sdcard", 5, (const char *[]) {"mp3"}, 1, NULL);

    esp_periph_set_stop_all(set);

    esp_periph_set_destroy(set);

    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_VERBOSE);
    esp_log_level_set("FATFS_STREAM", ESP_LOG_VERBOSE);
    esp_log_level_set("SDCARD", ESP_LOG_VERBOSE);
    esp_log_level_set("AUDIO_BOARD", ESP_LOG_VERBOSE);
    esp_log_level_set("PERIPH_BUTTON", ESP_LOG_VERBOSE);
    esp_log_level_set("PERIPH_TOUCH", ESP_LOG_VERBOSE);

    //xTaskCreate(i2cscanner_task, "I2CScanner", 2048, NULL, configMAX_PRIORITIES - 3, NULL);
    //xTaskCreate(list_sdcard_task, "ListSDCard", 2048, NULL, configMAX_PRIORITIES - 3, NULL);

    xTaskCreate(sound_task, "MP3", 4096, NULL, configMAX_PRIORITIES - 1, NULL);
} 
