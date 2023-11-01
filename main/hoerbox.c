#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs.h"
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

static const char *TAG = "BOX";
static const char *TAG_SOUND = "SOUND";
static const char *TAG_BEEP = "BEEP";
static const char *TAG_RFID = "RFID";

#define SLEEP_IN_MICRO_SECONDS 90000000
#define SHUTDOWN_IF_NO_TAGS_CONSECUTIVELY 300
#define VOLUME_MAX 70

static void beep_task(void *arg) { // do noot execute togehter with sound_task!
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream_writer, fatfs_stream_reader, mp3_decoder;

    ESP_LOGD(TAG_BEEP, "Initialize peripherals management");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    ESP_LOGD(TAG_BEEP, "Initialize and start peripherals");
    audio_board_key_init(set);
    audio_board_sdcard_init(set, SD_MODE_1_LINE);

    ESP_LOGD(TAG_BEEP, "Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGD(TAG_BEEP, "Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGD(TAG_BEEP, "Create fatfs stream to read data from sdcard");
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);

    ESP_LOGD(TAG_BEEP, "Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGD(TAG_BEEP, "Create mp3 decoder to decode mp3 file");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder = mp3_decoder_init(&mp3_cfg);

    ESP_LOGD(TAG_BEEP, "Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, fatfs_stream_reader, "file");
    audio_pipeline_register(pipeline, mp3_decoder, "mp3");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGD(TAG_BEEP, "Link it together [sdcard]-->fatfs_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
    audio_pipeline_link(pipeline, (const char *[]) {"file", "mp3", "i2s"}, 3);

    ESP_LOGD(TAG_BEEP, "Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGD(TAG_BEEP, "Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGD(TAG_BEEP, "Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    // read volume from NVS
    nvs_handle nvs_config;
    ESP_ERROR_CHECK(nvs_open("config", NVS_READWRITE, &nvs_config));
    int volume;
    esp_err_t ret = nvs_get_i32(nvs_config, "volume", &volume);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG_SOUND, "No previous volume found to restore");
    } else if (ret == ESP_OK) {
        ESP_LOGI(TAG_SOUND, "Restore previous volume: %d", volume);
        audio_hal_set_volume(board_handle->audio_hal, volume);
    } else {
        ESP_ERROR_CHECK(ret);
    }
    nvs_close(nvs_config);

    audio_element_set_uri(fatfs_stream_reader, "/sdcard/system_beep.mp3");
    audio_pipeline_reset_ringbuffer(pipeline);
    audio_pipeline_reset_elements(pipeline);

    // start mp3
    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG_BEEP, "Listen for all pipeline events");
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_BEEP, "Event interface error : %d", ret);
            continue;
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_UNKNOW) {
            ESP_LOGD(TAG_BEEP, "Event received [source_type: AUDIO_ELEMENT_TYPE_UNKNOW, cmd: %d]", msg.cmd);
        } else if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) {
            ESP_LOGD(TAG_BEEP, "Event received [source_type: AUDIO_ELEMENT_TYPE_ELEMENT, cmd: %d]", msg.cmd);
        } else if (msg.source_type == AUDIO_ELEMENT_TYPE_PLAYER) {
            ESP_LOGD(TAG_BEEP, "Event received [source_type: AUDIO_ELEMENT_TYPE_PLAYER, cmd: %d]", msg.cmd);
        } else if (msg.source_type == AUDIO_ELEMENT_TYPE_SERVICE) {
            ESP_LOGD(TAG_BEEP, "Event received [source_type: AUDIO_ELEMENT_TYPE_SERVICE, cmd: %d]", msg.cmd);
        }else if (msg.source_type == AUDIO_ELEMENT_TYPE_PERIPH) {
            ESP_LOGD(TAG_BEEP, "Event received [source_type: AUDIO_ELEMENT_TYPE_PERIPH, cmd: %d]", msg.cmd);
        } else {
            ESP_LOGD(TAG_BEEP, "Event received [source_type: %d, cmd: %d]", msg.source_type, msg.cmd);
        }

        // start file
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) mp3_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder, &music_info);
            ESP_LOGI(TAG_BEEP, "Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                    music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(i2s_stream_writer, &music_info);
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        // end of file
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && ((int)msg.data == AEL_STATUS_STATE_FINISHED)) {
            ESP_LOGI(TAG_BEEP, "End of MP3");
            break;
        }
    }

    ESP_LOGD(TAG_BEEP, "Terminate");
    audio_pipeline_terminate(pipeline);

    ESP_LOGD(TAG_BEEP, "Unregister");
    audio_pipeline_unregister(pipeline, fatfs_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, mp3_decoder);

    ESP_LOGD(TAG_BEEP, "Remove listener");
    audio_pipeline_remove_listener(pipeline);

    ESP_LOGD(TAG_BEEP, "Stop periph");
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGD(TAG_BEEP, "Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface");
    audio_event_iface_destroy(evt);

    ESP_LOGD(TAG_BEEP, "Release all resources");
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(fatfs_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(mp3_decoder);
    //esp_periph_set_destroy(set); // TODO causes panic

    ESP_LOGI(TAG_BEEP, "Sleep");
    // go into deep sleep to save energy
    // you have to turn off/on the box to exit the sleep loop
    esp_deep_sleep(SLEEP_IN_MICRO_SECONDS);
    // not reached vTaskDelete(NULL);
}

audio_pipeline_handle_t pipeline;
audio_element_handle_t fatfs_stream_reader;
int no_tags_consecutively = 0;

static void rfid_task(void *arg) { // requires sound_task!
    ESP_ERROR_CHECK(rc522_init());

    char no[11];
    char previous_no[11];
    char sound_file[23];
    char missing_sound_file[28];
    previous_no[0] = 0;
    while(no_tags_consecutively < SHUTDOWN_IF_NO_TAGS_CONSECUTIVELY) {
        uint8_t* tag = rc522_get_tag();
        no[0] = 0;
        if (tag != NULL) {
            sprintf(no, "%02x%02x%02x%02x%02x", tag[0], tag[1], tag[2], tag[3], tag[4]);
            sprintf(sound_file, "/sdcard/%s.mp3", no);
            sprintf(missing_sound_file, "%s_miss", sound_file);
            /*printf("serial: ");
            for(int i = 0; i < 5; i++) {
                printf("%#x ", tag[i]);
            }
            printf("\n");*/
            ESP_LOGD(TAG_RFID, "RFID tag found: %s", no);
            free(tag);
            no_tags_consecutively = 0;
        } else {
            no_tags_consecutively++;
            ESP_LOGD(TAG_RFID, "RFID tag not found, no_tags_consecutively=%d", no_tags_consecutively);
        }

        if (strcmp(previous_no, no) != 0) { // tag changed
            if (previous_no[0] != 0) { // stop pipeline
                audio_pipeline_stop(pipeline);
                audio_pipeline_wait_for_stop(pipeline);
                audio_pipeline_terminate(pipeline);
            }
            if (no[0] == 0) {
                ESP_LOGI(TAG_RFID, "Stop");
            } else {
                // check if file extsist
                FILE *file = fopen(sound_file, "r");
                if (file != NULL) { // file exists
                    fclose(file);
                    ESP_LOGI(TAG_RFID, "Play %s: %s", no, sound_file);

                    // prepare pipeline
                    audio_element_set_uri(fatfs_stream_reader, sound_file);
                    audio_pipeline_reset_ringbuffer(pipeline);
                    audio_pipeline_reset_elements(pipeline);

                    // fetch offset
                    nvs_handle nvs_position;
                    esp_err_t ret1 = nvs_open("position", NVS_READONLY, &nvs_position);
                    if (ret1 == ESP_ERR_NVS_NOT_FOUND) {
                        ESP_LOGI(TAG_RFID, "No previous position found for %s", no);
                    } else if (ret1 == ESP_OK) {
                        int64_t position;
                        esp_err_t ret2 = nvs_get_i64(nvs_position, no, &position);
                        if (ret2 == ESP_ERR_NVS_NOT_FOUND) {
                            ESP_LOGI(TAG_RFID, "No previous position found for %s", no);
                        } else if (ret2 == ESP_OK) {
                            ESP_LOGI(TAG_RFID, "Previous position found for %s: %" PRId64, no, position);
                            audio_element_info_t info = {0};
                            audio_element_getinfo(fatfs_stream_reader, &info);
                            info.byte_pos = position;
                            audio_element_setinfo(fatfs_stream_reader, &info);
                        } else {
                            ESP_ERROR_CHECK(ret2);
                        }
                    } else {
                        ESP_ERROR_CHECK(ret1);
                    }
                    nvs_close(nvs_position);
                    
                    // start mp3
                    audio_pipeline_run(pipeline);
                } else { // file does not exist
                    ESP_LOGI(TAG_RFID, "Not found %s: %s", no, sound_file);

                    file = fopen(missing_sound_file, "w");
                    fclose(file);
                    
                    // prepare pipeline
                    audio_element_set_uri(fatfs_stream_reader, "/sdcard/system_not_found.mp3");
                    audio_pipeline_reset_ringbuffer(pipeline);
                    audio_pipeline_reset_elements(pipeline);

                    // start mp3
                    audio_pipeline_run(pipeline);
                }
            }

            strcpy(previous_no, no);
        }

        vTaskDelay(2000 / portTICK_RATE_MS);
    }

    ESP_ERROR_CHECK(rc522_clear());

    ESP_LOGI(TAG_RFID, "Bye");

    vTaskDelete(NULL);
}

static void sound_task(void *arg) { // controlled by rfid_task
    audio_element_handle_t i2s_stream_writer, mp3_decoder;

    ESP_LOGD(TAG_SOUND, "Initialize peripherals management");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    ESP_LOGD(TAG_SOUND, "Initialize and start peripherals");
    audio_board_key_init(set);
    audio_board_sdcard_init(set, SD_MODE_1_LINE);

    ESP_LOGD(TAG_SOUND, "Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGD(TAG_SOUND, "Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGD(TAG_SOUND, "Create fatfs stream to read data from sdcard");
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);

    ESP_LOGD(TAG_SOUND, "Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGD(TAG_SOUND, "Create mp3 decoder to decode mp3 file");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder = mp3_decoder_init(&mp3_cfg);

    ESP_LOGD(TAG_SOUND, "Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, fatfs_stream_reader, "file");
    audio_pipeline_register(pipeline, mp3_decoder, "mp3");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGD(TAG_SOUND, "Link it together [sdcard]-->fatfs_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
    audio_pipeline_link(pipeline, (const char *[]) {"file", "mp3", "i2s"}, 3);

    ESP_LOGD(TAG_SOUND, "Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGD(TAG_SOUND, "Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGD(TAG_SOUND, "Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    TaskHandle_t rfid_handle = NULL;
    xTaskCreate(rfid_task, "RFID", 4096, NULL, configMAX_PRIORITIES - 3, &rfid_handle);

    // read volume from NVS
    nvs_handle nvs_config;
    ESP_ERROR_CHECK(nvs_open("config", NVS_READWRITE, &nvs_config));
    int volume;
    esp_err_t ret = nvs_get_i32(nvs_config, "volume", &volume);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG_SOUND, "No previous volume found to restore");
    } else if (ret == ESP_OK) {
        ESP_LOGI(TAG_SOUND, "Restore previous volume: %d", volume);
        audio_hal_set_volume(board_handle->audio_hal, volume);
    } else {
        ESP_ERROR_CHECK(ret);
    }
    nvs_close(nvs_config);

    char playing_file[23];
    playing_file[0] = 0;
    char playing_no[11];
    playing_no[0] = 0;

    time_t rewind_start = time(NULL);
    bool rewind = false;
    time_t fastforward_start = time(NULL);
    bool fastforward = false;

    bool shutdown = false;

    ESP_LOGI(TAG_SOUND, "Listen for all pipeline events");
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, 2000 / portTICK_RATE_MS);
        if (ret == ESP_OK) { // no event, timeout, do some global checks
            if (msg.source_type == AUDIO_ELEMENT_TYPE_UNKNOW) {
                ESP_LOGD(TAG_SOUND, "Event received [source_type: AUDIO_ELEMENT_TYPE_UNKNOW, cmd: %d]", msg.cmd);
            } else if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) {
                ESP_LOGD(TAG_SOUND, "Event received [source_type: AUDIO_ELEMENT_TYPE_ELEMENT, cmd: %d]", msg.cmd);
            } else if (msg.source_type == AUDIO_ELEMENT_TYPE_PLAYER) {
                ESP_LOGD(TAG_SOUND, "Event received [source_type: AUDIO_ELEMENT_TYPE_PLAYER, cmd: %d]", msg.cmd);
            } else if (msg.source_type == AUDIO_ELEMENT_TYPE_SERVICE) {
                ESP_LOGD(TAG_SOUND, "Event received [source_type: AUDIO_ELEMENT_TYPE_SERVICE, cmd: %d]", msg.cmd);
            }else if (msg.source_type == AUDIO_ELEMENT_TYPE_PERIPH) {
                ESP_LOGD(TAG_SOUND, "Event received [source_type: AUDIO_ELEMENT_TYPE_PERIPH, cmd: %d]", msg.cmd);
            } else {
                ESP_LOGD(TAG_SOUND, "Event received [source_type: %d, cmd: %d]", msg.source_type, msg.cmd);
            }

            // volume down
            if ((int)msg.data == get_input_rec_id() && msg.cmd == PERIPH_BUTTON_RELEASE) {
                int player_volume;
                audio_hal_get_volume(board_handle->audio_hal, &player_volume);
                player_volume -= 3;
                if (player_volume < 3) {
                    player_volume = 3;
                }
                audio_hal_set_volume(board_handle->audio_hal, player_volume);
                ESP_LOGI(TAG_SOUND, "Volume decreased to %d %%", player_volume);

                nvs_handle nvs_config;
                ESP_ERROR_CHECK(nvs_open("config", NVS_READWRITE, &nvs_config));
                ESP_ERROR_CHECK(nvs_set_i32(nvs_config, "volume", player_volume));
                ESP_ERROR_CHECK(nvs_commit(nvs_config));
                nvs_close(nvs_config);
                continue;
            }

            // start: rewind
            if ((int)msg.data == get_input_rec_id() && msg.cmd == PERIPH_BUTTON_LONG_PRESSED) {
                ESP_LOGI(TAG_SOUND, "rewind, start");
                audio_pipeline_pause(pipeline);
                rewind = true;
                rewind_start = time(NULL);
                continue;
            }

            // stop: rewind
            if ((int)msg.data == get_input_rec_id() && msg.cmd == PERIPH_BUTTON_LONG_RELEASE) {
                double seconds = difftime(time(NULL), rewind_start);
                ESP_LOGI(TAG_SOUND, "rewind, stop, seconds=%f", seconds);
                audio_element_info_t info = {0};
                audio_element_getinfo(fatfs_stream_reader, &info);
                int64_t byte_offset = ((int64_t) (((double) (info.total_bytes / 100)) * seconds));
                ESP_LOGI(TAG_SOUND, "rewind, current byte_pos=%" PRId64 ", byte_offset=%" PRId64 ", bytes=%" PRId64, info.byte_pos, byte_offset, info.total_bytes);
                if (fastforward == true && rewind == true) { // both button pushed, reset to begin
                    info.byte_pos = 0;
                } else {
                    if ((info.byte_pos - byte_offset) >= 0) {
                        info.byte_pos -= byte_offset;
                    } else {
                        info.byte_pos = 0;
                    }
                }
                audio_element_setinfo(fatfs_stream_reader, &info);
                audio_pipeline_resume(pipeline);
                rewind = false;
                continue;
            }

            // volume up
            if ((int)msg.data == get_input_mode_id() && msg.cmd == PERIPH_BUTTON_RELEASE) {
                int player_volume;
                audio_hal_get_volume(board_handle->audio_hal, &player_volume);
                player_volume += 3;
                if (player_volume > VOLUME_MAX) {
                    player_volume = VOLUME_MAX;
                }
                audio_hal_set_volume(board_handle->audio_hal, player_volume);
                ESP_LOGI(TAG_SOUND, "Volume increased to %d %%", player_volume);

                nvs_handle nvs_config;
                ESP_ERROR_CHECK(nvs_open("config", NVS_READWRITE, &nvs_config));
                ESP_ERROR_CHECK(nvs_set_i32(nvs_config, "volume", player_volume));
                ESP_ERROR_CHECK(nvs_commit(nvs_config));
                nvs_close(nvs_config);
                continue;
            }

            // start: fast-forward
            if ((int)msg.data == get_input_mode_id() && msg.cmd == PERIPH_BUTTON_LONG_PRESSED) {
                ESP_LOGI(TAG_SOUND, "fast-forward, start");
                audio_pipeline_pause(pipeline);
                fastforward = true;
                fastforward_start = time(NULL);
                continue;
            }

            // stop: fast-forward
            if ((int)msg.data == get_input_mode_id() && msg.cmd == PERIPH_BUTTON_LONG_RELEASE) {
                double seconds = difftime(time(NULL), fastforward_start);
                ESP_LOGI(TAG_SOUND, "fast-forward, stop, seconds=%f", seconds);
                audio_element_info_t info = {0};
                audio_element_getinfo(fatfs_stream_reader, &info);
                int64_t byte_offset = ((int64_t) (((double) (info.total_bytes / 100)) * seconds));
                ESP_LOGI(TAG_SOUND, "fast-forward, current byte_pos=%" PRId64 ", byte_offset=%" PRId64 ", bytes=%" PRId64, info.byte_pos, byte_offset, info.total_bytes);
                if (fastforward == true && rewind == true) { // both button pushed, reset to begin
                    info.byte_pos = 0;
                } else {
                    if ((info.byte_pos + byte_offset) < info.total_bytes) {
                        info.byte_pos += byte_offset;
                    } else {
                        info.byte_pos = info.total_bytes - 100000;
                    }
                }
                fastforward = false;
                audio_element_setinfo(fatfs_stream_reader, &info);
                audio_pipeline_resume(pipeline);
                continue;
            }

            // start file
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) mp3_decoder
                && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                if (strncmp("/sdcard/system_", audio_element_get_uri(fatfs_stream_reader), 15) == 0) {
                    strcpy(playing_file, audio_element_get_uri(fatfs_stream_reader));
                    playing_no[0] = 0;
                } else {
                    strcpy(playing_file, audio_element_get_uri(fatfs_stream_reader));
                    memcpy(playing_no, &playing_file[8], 10);
                    playing_no[10] = 0;
                }

                audio_element_info_t music_info = {0};
                audio_element_getinfo(mp3_decoder, &music_info);

                ESP_LOGI(TAG_SOUND, "Receive music info from mp3 decoder, %s, sample_rates=%d, bits=%d, ch=%d",
                        playing_file, music_info.sample_rates, music_info.bits, music_info.channels);

                audio_element_setinfo(i2s_stream_writer, &music_info);
                i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
                continue;
            }

            // stop MP3
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
                && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                && ((int)msg.data == AEL_STATUS_STATE_STOPPED)) {
                ESP_LOGI(TAG_SOUND, "Stop MP3: %s", playing_file);
                if (strncmp("/sdcard/system_", audio_element_get_uri(fatfs_stream_reader), 15) == 0) {
                    playing_file[0] = 0;
                } else {
                    playing_file[0] = 0;
                    playing_no[0] = 0;
                }
                continue;
            }

            // end of file
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
                && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
                && ((int)msg.data == AEL_STATUS_STATE_FINISHED)) {
                if (strncmp("/sdcard/system_", audio_element_get_uri(fatfs_stream_reader), 15) == 0) {
                    ESP_LOGI(TAG_SOUND, "End of MP3: %s, shutdown=%d)", playing_file, shutdown);
                    playing_file[0] = 0;
                    if (shutdown == true) {
                        break;
                    }
                } else {
                    ESP_LOGI(TAG_SOUND, "End of MP3: %s, erase last position for %s", playing_file, playing_no);
                    nvs_handle nvs_position;
                    ESP_ERROR_CHECK(nvs_open("position", NVS_READWRITE, &nvs_position));
                    esp_err_t ret1 = nvs_erase_key(nvs_position, playing_no);
                    if (ret1 == ESP_ERR_NVS_NOT_FOUND) {
                        // already deleted
                    } else {
                        ESP_ERROR_CHECK(ret1);
                    }
                    nvs_close(nvs_position);
                    playing_file[0] = 0;
                    playing_no[0] = 0;
                }
                continue;
            }
        } else {
            if (no_tags_consecutively >= SHUTDOWN_IF_NO_TAGS_CONSECUTIVELY) {
                if (shutdown == false) {
                    shutdown = true;

                    // prepare pipeline
                    audio_element_set_uri(fatfs_stream_reader, "/sdcard/system_beep.mp3");
                    audio_pipeline_reset_ringbuffer(pipeline);
                    audio_pipeline_reset_elements(pipeline);

                    // start mp3
                    audio_pipeline_run(pipeline);
                }
                continue;
            }
        }

        if (playing_no[0] != 0) {
            // store position
            audio_element_info_t playing_info = {0};
            audio_element_getinfo(fatfs_stream_reader, &playing_info);
            if (playing_info.byte_pos > 30000) { // when the pipeline is stopped we receive file sizes of 0 bytes
                ESP_LOGI(TAG_SOUND, "Save last position for %s: %" PRId64, playing_no, playing_info.byte_pos);
                nvs_handle nvs_position;
                ESP_ERROR_CHECK(nvs_open("position", NVS_READWRITE, &nvs_position));
                esp_err_t ret1 = nvs_set_i64(nvs_position, playing_no, playing_info.byte_pos);
                if (ret1 == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
                    ESP_ERROR_CHECK(nvs_flash_erase());
                    ESP_ERROR_CHECK(nvs_flash_init());
                    ret1 = nvs_set_i64(nvs_position, playing_no, playing_info.byte_pos);
                } 
                ESP_ERROR_CHECK(ret1);
                nvs_close(nvs_position);
            } else {
                ESP_LOGD(TAG_SOUND, "Skip last position for %s: %" PRId64, playing_no, playing_info.byte_pos);
            }
        }
    }

    ESP_LOGD(TAG_SOUND, "Terminate");
    audio_pipeline_terminate(pipeline);

    ESP_LOGD(TAG_SOUND, "Unregister");
    audio_pipeline_unregister(pipeline, fatfs_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, mp3_decoder);

    ESP_LOGD(TAG_SOUND, "Remove listener");
    audio_pipeline_remove_listener(pipeline);

    ESP_LOGD(TAG_SOUND, "Stop periph");
    esp_periph_set_stop_all(set);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGD(TAG_SOUND, "Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface");
    audio_event_iface_destroy(evt);

    ESP_LOGD(TAG_SOUND, "Release all resources");
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(fatfs_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(mp3_decoder);
    //esp_periph_set_destroy(set); // TODO causes panic

    ESP_LOGI(TAG_SOUND, "Sleep");
    // go into deep sleep to save energy
    // you have to turn off/on the box to exit the sleep loop
    esp_deep_sleep(SLEEP_IN_MICRO_SECONDS);
    // not reached vTaskDelete(NULL);
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

    audio_board_sdcard_init(set, SD_MODE_1_LINE);

    sdcard_scan(list_sdcard_db, "/sdcard", 5, (const char *[]) {"mp3"}, 1, NULL);

    esp_periph_set_stop_all(set);

    //esp_periph_set_destroy(set); // TODO causes panic

    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set(TAG_RFID, ESP_LOG_INFO);
    esp_log_level_set(TAG_SOUND, ESP_LOG_INFO);
    esp_log_level_set(TAG_BEEP, ESP_LOG_INFO);
    //esp_log_level_set("FATFS_STREAM", ESP_LOG_VERBOSE);
    //esp_log_level_set("SDCARD", ESP_LOG_VERBOSE);
    //esp_log_level_set("AUDIO_BOARD", ESP_LOG_VERBOSE);
    //esp_log_level_set("PERIPH_BUTTON", ESP_LOG_VERBOSE);
    //esp_log_level_set("PERIPH_TOUCH", ESP_LOG_VERBOSE);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    } 
    ESP_ERROR_CHECK(ret);

    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI(TAG, "wakeup");
            xTaskCreate(beep_task, "Beep", 4096, NULL, configMAX_PRIORITIES - 2, NULL);
            return;
        default:
            ESP_LOGI(TAG, "hello");

            //xTaskCreate(i2cscanner_task, "I2CScanner", 2048, NULL, configMAX_PRIORITIES - 3, NULL);
            //xTaskCreate(list_sdcard_task, "ListSDCard", 2048, NULL, configMAX_PRIORITIES - 3, NULL);
            xTaskCreate(sound_task, "MP3", 4096, NULL, configMAX_PRIORITIES - 1, NULL);
            return;
    }
} 
