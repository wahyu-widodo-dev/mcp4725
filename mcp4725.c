#include "mcp4725.h"

#include <math.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MCP4725_EEPROM_WRITE_COMMAND 0x60
#define MCP4725_DAC_REGISTER_WRITE_COMMAND 0x40
#define MCP4725_EEPROM_WRITE_POLL_INTERVAL_MS 1
#define MCP4725_READBACK_SIZE 5

struct mcp4725_device {
    i2c_master_dev_handle_t i2c_device;
    uint32_t timeout_ms;
    float supply_voltage_v;
    uint16_t dac_code;
    mcp4725_power_down_t power_down;
};

static bool valid_power_down(mcp4725_power_down_t power_down)
{
    return (unsigned)power_down <= MCP4725_POWER_DOWN_500K_TO_GND;
}

static esp_err_t read_readback(mcp4725_handle_t dac, mcp4725_readback_t *readback)
{
    uint8_t data[MCP4725_READBACK_SIZE];
    esp_err_t err = i2c_master_receive(dac->i2c_device,
                                       data,
                                       sizeof(data),
                                       dac->timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    /* Register DAC mengemas D11-D4 pada data[1] dan D3-D0 di nibble atas data[2]. */
    *readback = (mcp4725_readback_t) {
        .ready = (data[0] & 0x80) != 0,
        .power_on_reset = (data[0] & 0x40) != 0,
        .dac_power_down = (mcp4725_power_down_t)((data[0] >> 1) & 0x03),
        .dac_code = ((uint16_t)data[1] << 4) | (data[2] >> 4),
        .eeprom_power_down = (mcp4725_power_down_t)((data[3] >> 5) & 0x03),
        .eeprom_code = ((uint16_t)(data[3] & 0x0F) << 8) | data[4],
    };
    return ESP_OK;
}

static esp_err_t write_fast(mcp4725_handle_t dac,
                            uint16_t code,
                            mcp4725_power_down_t power_down)
{
    /* Fast Write menempatkan PD1:PD0 dan D11:D8 pada byte pertama. */
    const uint8_t data[] = {
        (uint8_t)(((uint16_t)power_down << 4) | ((code >> 8) & 0x0F)),
        (uint8_t)code,
    };
    return i2c_master_transmit(dac->i2c_device,
                               data,
                               sizeof(data),
                               dac->timeout_ms);
}

static esp_err_t write_register(mcp4725_handle_t dac,
                                uint8_t command,
                                uint16_t code,
                                mcp4725_power_down_t power_down)
{
    /* Perintah register menambahkan command byte sebelum data PD dan DAC. */
    const uint8_t data[] = {
        command,
        (uint8_t)(((uint16_t)power_down << 4) | ((code >> 8) & 0x0F)),
        (uint8_t)code,
    };
    return i2c_master_transmit(dac->i2c_device,
                               data,
                               sizeof(data),
                               dac->timeout_ms);
}

static esp_err_t voltage_to_code(mcp4725_handle_t dac, float voltage_v, uint16_t *code)
{
    ESP_RETURN_ON_FALSE(isfinite(voltage_v), ESP_ERR_INVALID_ARG, "mcp4725", "voltage must be finite");
    const float maximum_output_v =
        dac->supply_voltage_v * (float)MCP4725_DAC_CODE_MAX / 4096.0f;
    ESP_RETURN_ON_FALSE(voltage_v >= 0.0f && voltage_v <= maximum_output_v,
                        ESP_ERR_INVALID_ARG, "mcp4725", "voltage is outside the representable output range");
    *code = (uint16_t)lroundf(voltage_v * 4096.0f / dac->supply_voltage_v);
    if (*code > MCP4725_DAC_CODE_MAX) {
        *code = MCP4725_DAC_CODE_MAX;
    }
    return ESP_OK;
}

mcp4725_config_t mcp4725_get_default_config(void)
{
    return (mcp4725_config_t) {
        .i2c_address = MCP4725_I2C_ADDRESS_DEFAULT,
        .i2c_frequency_hz = 100000,
        .i2c_timeout_ms = 1000,
        .supply_voltage_v = 3.3f,
    };
}

esp_err_t mcp4725_create(i2c_master_bus_handle_t bus,
                         const mcp4725_config_t *config,
                         mcp4725_handle_t *out_dac)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "bus is NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "config is NULL");
    ESP_RETURN_ON_FALSE(out_dac != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "out_dac is NULL");
    *out_dac = NULL;
    ESP_RETURN_ON_FALSE(config->i2c_address >= 0x60 && config->i2c_address <= 0x67,
                        ESP_ERR_INVALID_ARG, "mcp4725", "address must be in the MCP4725 7-bit range 0x60-0x67");
    ESP_RETURN_ON_FALSE(config->i2c_frequency_hz > 0 && config->i2c_frequency_hz <= 400000,
                        ESP_ERR_INVALID_ARG, "mcp4725", "I2C frequency must be between 1 Hz and 400 kHz");
    ESP_RETURN_ON_FALSE(config->i2c_timeout_ms > 0,
                        ESP_ERR_INVALID_ARG, "mcp4725", "I2C timeout must be greater than zero");
    ESP_RETURN_ON_FALSE(isfinite(config->supply_voltage_v) &&
                            config->supply_voltage_v >= MCP4725_SUPPLY_VOLTAGE_MIN_V &&
                            config->supply_voltage_v <= MCP4725_SUPPLY_VOLTAGE_MAX_V,
                        ESP_ERR_INVALID_ARG,
                        "mcp4725",
                        "supply voltage must be between 2.7 V and 5.5 V");

    mcp4725_handle_t dac = calloc(1, sizeof(*dac));
    ESP_RETURN_ON_FALSE(dac != NULL, ESP_ERR_NO_MEM, "mcp4725", "failed to allocate DAC handle");
    dac->timeout_ms = config->i2c_timeout_ms;
    dac->supply_voltage_v = config->supply_voltage_v;

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->i2c_address,
        .scl_speed_hz = config->i2c_frequency_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &device_config, &dac->i2c_device);
    if (err != ESP_OK) {
        free(dac);
        return err;
    }

    mcp4725_readback_t readback;
    err = read_readback(dac, &readback);
    if (err != ESP_OK) {
        esp_err_t remove_err = i2c_master_bus_rm_device(dac->i2c_device);
        free(dac);
        return remove_err == ESP_OK ? err : remove_err;
    }
    dac->dac_code = readback.dac_code;
    dac->power_down = readback.dac_power_down;
    *out_dac = dac;
    return ESP_OK;
}

esp_err_t mcp4725_delete(mcp4725_handle_t dac)
{
    ESP_RETURN_ON_FALSE(dac != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "dac is NULL");
    esp_err_t err = i2c_master_bus_rm_device(dac->i2c_device);
    if (err == ESP_OK) {
        free(dac);
    }
    return err;
}

esp_err_t mcp4725_set_raw(mcp4725_handle_t dac,
                          uint16_t code,
                          mcp4725_power_down_t power_down,
                          mcp4725_write_mode_t write_mode)
{
    ESP_RETURN_ON_FALSE(dac != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "dac is NULL");
    ESP_RETURN_ON_FALSE(code <= MCP4725_DAC_CODE_MAX,
                        ESP_ERR_INVALID_ARG, "mcp4725", "DAC code must be 0-4095");
    ESP_RETURN_ON_FALSE(valid_power_down(power_down),
                        ESP_ERR_INVALID_ARG, "mcp4725", "power-down mode is invalid");
    ESP_RETURN_ON_FALSE(write_mode == MCP4725_WRITE_FAST ||
                            write_mode == MCP4725_WRITE_DAC_REGISTER,
                        ESP_ERR_INVALID_ARG, "mcp4725", "write mode is invalid");

    esp_err_t err = write_mode == MCP4725_WRITE_FAST
                        ? write_fast(dac, code, power_down)
                        : write_register(dac,
                                         MCP4725_DAC_REGISTER_WRITE_COMMAND,
                                         code,
                                         power_down);
    if (err == ESP_OK) {
        dac->dac_code = code;
        dac->power_down = power_down;
    }
    return err;
}

esp_err_t mcp4725_set_voltage(mcp4725_handle_t dac,
                              float voltage_v,
                              mcp4725_power_down_t power_down,
                              mcp4725_write_mode_t write_mode)
{
    ESP_RETURN_ON_FALSE(dac != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "dac is NULL");
    uint16_t code;
    esp_err_t err = voltage_to_code(dac, voltage_v, &code);
    if (err != ESP_OK) {
        return err;
    }
    return mcp4725_set_raw(dac, code, power_down, write_mode);
}

esp_err_t mcp4725_get_readback(mcp4725_handle_t dac,
                               mcp4725_readback_t *readback)
{
    ESP_RETURN_ON_FALSE(dac != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "dac is NULL");
    ESP_RETURN_ON_FALSE(readback != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "readback is NULL");

    esp_err_t err = read_readback(dac, readback);
    if (err == ESP_OK) {
        dac->dac_code = readback->dac_code;
        dac->power_down = readback->dac_power_down;
    }
    return err;
}

esp_err_t mcp4725_get_voltage(mcp4725_handle_t dac,
                              float *voltage_v,
                              mcp4725_readback_t *readback)
{
    ESP_RETURN_ON_FALSE(dac != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "dac is NULL");
    ESP_RETURN_ON_FALSE(voltage_v != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "voltage_v is NULL");

    mcp4725_readback_t state;
    esp_err_t err = mcp4725_get_readback(dac, &state);
    if (err == ESP_OK) {
        *voltage_v = dac->supply_voltage_v * state.dac_code / 4096.0f;
        if (readback != NULL) {
            *readback = state;
        }
    }
    return err;
}

esp_err_t mcp4725_set_power_down(mcp4725_handle_t dac,
                                 mcp4725_power_down_t power_down,
                                 mcp4725_write_mode_t write_mode)
{
    ESP_RETURN_ON_FALSE(dac != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "dac is NULL");
    return mcp4725_set_raw(dac, dac->dac_code, power_down, write_mode);
}

esp_err_t mcp4725_wait_ready(mcp4725_handle_t dac, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(dac != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "dac is NULL");
    ESP_RETURN_ON_FALSE(timeout_ms > 0, ESP_ERR_INVALID_ARG, "mcp4725", "timeout must be greater than zero");

    const int64_t start_time_us = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)timeout_ms * 1000;
    TickType_t poll_ticks = pdMS_TO_TICKS(MCP4725_EEPROM_WRITE_POLL_INTERVAL_MS);
    if (poll_ticks == 0) {
        poll_ticks = 1;
    }

    while (true) {
        mcp4725_readback_t readback;
        esp_err_t err = read_readback(dac, &readback);
        if (err != ESP_OK) {
            return err;
        }
        if (readback.ready) {
            return ESP_OK;
        }
        if (esp_timer_get_time() - start_time_us >= timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(poll_ticks);
    }
}

esp_err_t mcp4725_save_to_eeprom(mcp4725_handle_t dac,
                                 uint16_t code,
                                 mcp4725_power_down_t power_down)
{
    ESP_RETURN_ON_FALSE(dac != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "dac is NULL");
    ESP_RETURN_ON_FALSE(code <= MCP4725_DAC_CODE_MAX,
                        ESP_ERR_INVALID_ARG, "mcp4725", "DAC code must be 0-4095");
    ESP_RETURN_ON_FALSE(valid_power_down(power_down),
                        ESP_ERR_INVALID_ARG, "mcp4725", "power-down mode is invalid");

    esp_err_t err = mcp4725_wait_ready(dac, dac->timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    err = write_register(dac, MCP4725_EEPROM_WRITE_COMMAND, code, power_down);
    if (err != ESP_OK) {
        return err;
    }
    dac->dac_code = code;
    dac->power_down = power_down;
    return mcp4725_wait_ready(dac, dac->timeout_ms);
}

esp_err_t mcp4725_save_voltage_to_eeprom(mcp4725_handle_t dac,
                                         float voltage_v,
                                         mcp4725_power_down_t power_down)
{
    ESP_RETURN_ON_FALSE(dac != NULL, ESP_ERR_INVALID_ARG, "mcp4725", "dac is NULL");
    uint16_t code;
    esp_err_t err = voltage_to_code(dac, voltage_v, &code);
    if (err != ESP_OK) {
        return err;
    }
    return mcp4725_save_to_eeprom(dac, code, power_down);
}
