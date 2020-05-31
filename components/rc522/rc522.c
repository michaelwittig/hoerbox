#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i2c_bus.h"
#include "board.h"

#include "rc522.h"

static i2c_bus_handle_t i2c_handle;

static int i2c_addr = (0x28 << 1) | I2C_MASTER_WRITE;

esp_err_t rc522_write_n(uint8_t reg, uint8_t n, uint8_t *data) {
    return i2c_bus_write_bytes(i2c_handle, i2c_addr, &reg, sizeof(reg), data, n);
}

esp_err_t rc522_write(uint8_t reg, uint8_t val) {
    return rc522_write_n(reg, 1, &val);
}

uint8_t rc522_read(uint8_t reg) {
    uint8_t data = 0xFF;
    ESP_ERROR_CHECK(i2c_bus_read_bytes(i2c_handle, i2c_addr, &reg, sizeof(reg), &data, 1));
    return data;
}

esp_err_t rc522_set_bitmask(uint8_t reg, uint8_t mask) {
    return rc522_write(reg, rc522_read(reg) | mask);
}

esp_err_t rc522_clear_bitmask(uint8_t reg, uint8_t mask) {
    return rc522_write(reg, rc522_read(reg) & ~mask);
}

esp_err_t rc522_antenna_on() {
    esp_err_t ret = ESP_OK;

    if(~ (rc522_read(0x14) & 0x03)) {
        ret = rc522_set_bitmask(0x14, 0x03);
        if(ret != ESP_OK) {
            return ret;
        }
    }

    return rc522_write(0x26, 0x60); // 43dB gain
}

esp_err_t rc522_init() {
    esp_err_t ret = ESP_OK;

    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ret = get_i2c_pins(I2C_NUM_0, &i2c_cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    i2c_handle = i2c_bus_create(I2C_NUM_0, &i2c_cfg);

    // ---------- RW test ------------
    ret = rc522_write(0x24, 0x25);
    if (ret != ESP_OK) {
        return ret;
    }
    assert(rc522_read(0x24) == 0x25);
    ret = rc522_write(0x24, 0x26);
    if (ret != ESP_OK) {
        return ret;
    }
    assert(rc522_read(0x24) == 0x26);
    
    // ------- Config --------
    ret = rc522_write(0x01, 0x0F);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = rc522_write(0x2A, 0x8D);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = rc522_write(0x2B, 0x3E);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = rc522_write(0x2D, 0x1E);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = rc522_write(0x2C, 0x00);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = rc522_write(0x15, 0x40);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = rc522_write(0x11, 0x3D);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = rc522_antenna_on();
    if (ret != ESP_OK) {
        return ret;
    }

    printf("RC522 Firmware 0x%x\n", rc522_fw_version());

    return ret;
}

esp_err_t rc522_clear() {
    return i2c_bus_delete(i2c_handle);
}

uint8_t* rc522_card_write(uint8_t cmd, uint8_t *data, uint8_t n, uint8_t* res_n) {
    uint8_t *result = NULL;
    uint8_t irq = 0x00;
    uint8_t irq_wait = 0x00;
    uint8_t last_bits = 0;
    uint8_t nn = 0;
    
    if(cmd == 0x0E) {
        irq = 0x12;
        irq_wait = 0x10;
    }
    else if(cmd == 0x0C) {
        irq = 0x77;
        irq_wait = 0x30;
    }

    rc522_write(0x02, irq | 0x80);
    rc522_clear_bitmask(0x04, 0x80);
    rc522_set_bitmask(0x0A, 0x80);
    rc522_write(0x01, 0x00);

    rc522_write_n(0x09, n, data);

    rc522_write(0x01, cmd);

    if(cmd == 0x0C) {
        rc522_set_bitmask(0x0D, 0x80);
    }

    uint16_t i = 1000;

    for(;;) {
        nn = rc522_read(0x04);
        i--;

        if(! (i != 0 && (((nn & 0x01) == 0) && ((nn & irq_wait) == 0)))) {
            break;
        }
    }

    rc522_clear_bitmask(0x0D, 0x80);

    if(i != 0) {
        if((rc522_read(0x06) & 0x1B) == 0x00) {
            if(cmd == 0x0C) {
                nn = rc522_read(0x0A);
                last_bits = rc522_read(0x0C) & 0x07;

                if (last_bits != 0) {
                    *res_n = (nn - 1) + last_bits;
                } else {
                    *res_n = nn;
                }

                result = (uint8_t*) malloc(*res_n);

                for(i = 0; i < *res_n; i++) {
                    result[i] = rc522_read(0x09);
                }
            }
        }
    }

    return result;
}

/* Returns pointer to dynamically allocated array of two element */
uint8_t* rc522_calculate_crc(uint8_t *data, uint8_t n) {
    rc522_clear_bitmask(0x05, 0x04);
    rc522_set_bitmask(0x0A, 0x80);

    rc522_write_n(0x09, n, data);

    rc522_write(0x01, 0x03);

    uint8_t i = 255;
    uint8_t nn = 0;

    for(;;) {
        nn = rc522_read(0x05);
        i--;

        if(! (i != 0 && ! (nn & 0x04))) {
            break;
        }
    }

    uint8_t* res = (uint8_t*) malloc(2); 
    
    res[0] = rc522_read(0x22);
    res[1] = rc522_read(0x21);

    return res;
}

uint8_t* rc522_request(uint8_t* res_n) {
    uint8_t* result = NULL;
    rc522_write(0x0D, 0x07);

    uint8_t req_mode = 0x26;
    result = rc522_card_write(0x0C, &req_mode, 1, res_n);

    if(*res_n * 8 != 0x10) {
        free(result);
        return NULL;
    }

    return result;
}

uint8_t* rc522_anticoll() {
    uint8_t* result = NULL;
    uint8_t res_n;
    uint8_t serial_number[] = { 0x93, 0x20 };

    rc522_write(0x0D, 0x00);

    result = rc522_card_write(0x0C, serial_number, 2, &res_n);

    if(result != NULL && res_n != 5) {
        free(result);
        return NULL;
    }

    return result;
}

uint8_t* rc522_get_tag() {
    uint8_t* result = NULL;
    uint8_t* res_data = NULL;
    uint8_t res_data_n;

    res_data = rc522_request(&res_data_n);

    if(res_data != NULL) {
        free(res_data);

        result = rc522_anticoll(&res_data_n);

        if(result != NULL) {
            uint8_t buf[] = { 0x50, 0x00, 0x00, 0x00 };
            uint8_t* crc = rc522_calculate_crc(buf, 2);

            buf[2] = crc[0];
            buf[3] = crc[1];

            free(crc);

            res_data = rc522_card_write(0x0C, buf, 4, &res_data_n);
            free(res_data);

            rc522_clear_bitmask(0x08, 0x08);

            return result;
        }
    }

    return NULL;
}
