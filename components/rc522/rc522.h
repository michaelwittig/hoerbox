#pragma once

esp_err_t rc522_write_n(uint8_t reg, uint8_t n, uint8_t *data);
esp_err_t rc522_write(uint8_t reg, uint8_t val);

uint8_t rc522_read(uint8_t reg);
#define rc522_fw_version() rc522_read(0x37)
esp_err_t rc522_init();
esp_err_t rc522_clear();

esp_err_t rc522_set_bitmask(uint8_t reg, uint8_t mask);
esp_err_t rc522_clear_bitmask(uint8_t reg, uint8_t mask);
esp_err_t rc522_antenna_on();
uint8_t* rc522_calculate_crc(uint8_t *data, uint8_t n);
uint8_t* rc522_card_write(uint8_t cmd, uint8_t *data, uint8_t n, uint8_t* res_n);
uint8_t* rc522_request(uint8_t* res_n);
uint8_t* rc522_anticoll();
uint8_t* rc522_get_tag();
