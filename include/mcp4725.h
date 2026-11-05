#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Kode keluaran maksimum DAC 12-bit. */
#define MCP4725_DAC_CODE_MAX 4095
/** Alamat I2C 7-bit bawaan MCP4725. */
#define MCP4725_I2C_ADDRESS_DEFAULT 0x60
/** Batas bawah VDD operasi MCP4725 menurut spesifikasi perangkat. */
#define MCP4725_SUPPLY_VOLTAGE_MIN_V 2.7f
/** Batas atas VDD operasi MCP4725 menurut spesifikasi perangkat. */
#define MCP4725_SUPPLY_VOLTAGE_MAX_V 5.5f

/** Handle buram DAC; buat dan lepaskan melalui fungsi lifecycle MCP4725. */
typedef struct mcp4725_device *mcp4725_handle_t;

/** Mode keluaran DAC, termasuk pilihan resistor internal saat power-down. */
typedef enum {
    MCP4725_POWER_DOWN_NORMAL = 0,       /**< DAC aktif dengan keluaran buffered. */
    MCP4725_POWER_DOWN_1K_TO_GND = 1,    /**< Keluaran ditarik ke ground melalui 1 kOhm. */
    MCP4725_POWER_DOWN_100K_TO_GND = 2,  /**< Keluaran ditarik ke ground melalui 100 kOhm. */
    MCP4725_POWER_DOWN_500K_TO_GND = 3,  /**< Keluaran ditarik ke ground melalui 500 kOhm. */
} mcp4725_power_down_t;

/** Metode menulis DAC register tanpa mengubah EEPROM. */
typedef enum {
    MCP4725_WRITE_FAST = 0,       /**< Fast Write dua byte untuk pembaruan rutin. */
    MCP4725_WRITE_DAC_REGISTER,   /**< Perintah DAC register tiga byte. */
} mcp4725_write_mode_t;

/**
 * @brief Parameter komunikasi I2C dan referensi tegangan DAC.
 */
typedef struct {
    uint8_t i2c_address;          /**< Alamat 7-bit MCP4725, umumnya 0x60 hingga 0x67. */
    uint32_t i2c_frequency_hz;    /**< Kecepatan SCL untuk device MCP4725. */
    uint32_t i2c_timeout_ms;      /**< Batas waktu setiap transaksi I2C. */
    float supply_voltage_v;      /**< VDD aktual (2,7 hingga 5,5 V) untuk konversi kode. */
} mcp4725_config_t;

/**
 * @brief Status dan isi DAC register serta EEPROM yang dibaca dari chip.
 */
typedef struct {
    bool ready;                             /**< True jika EEPROM tidak sedang ditulis. */
    bool power_on_reset;                    /**< Flag power-on reset dari perangkat. */
    mcp4725_power_down_t dac_power_down;    /**< Mode keluaran DAC aktif. */
    uint16_t dac_code;                      /**< Kode 12-bit pada DAC register. */
    mcp4725_power_down_t eeprom_power_down; /**< Mode keluaran tersimpan di EEPROM. */
    uint16_t eeprom_code;                   /**< Kode 12-bit tersimpan di EEPROM. */
} mcp4725_readback_t;

/**
 * @brief Menghasilkan konfigurasi awal tanpa melakukan transaksi I2C.
 *
 * @return Konfigurasi dengan alamat 0x60, clock 100 kHz, timeout 1000 ms,
 *         dan VDD referensi 3.3 V.
 */
mcp4725_config_t mcp4725_get_default_config(void);

/**
 * @brief Mendaftarkan MCP4725 pada bus yang dibuat aplikasi.
 *
 * Fungsi membaca status awal perangkat, tetapi tidak mengubah DAC atau EEPROM.
 * Bus tetap dimiliki aplikasi dan harus tetap hidup selama handle DAC digunakan.
 *
 * @param[in] bus Handle bus I2C master bersama.
 * @param[in] config Alamat, kecepatan, timeout, dan VDD aktual.
 * @param[out] out_dac Menerima handle DAC jika proses berhasil.
 * @return `ESP_OK` jika berhasil; kode error ESP-IDF jika parameter, alokasi,
 *         atau transaksi gagal.
 */
esp_err_t mcp4725_create(i2c_master_bus_handle_t bus,
                         const mcp4725_config_t *config,
                         mcp4725_handle_t *out_dac);

/**
 * @brief Melepaskan MCP4725 dari bus dan membebaskan handle.
 *
 * @param[in] dac Handle yang dibuat oleh `mcp4725_create()`.
 * @return `ESP_OK` jika berhasil; kode error ESP-IDF jika pelepasan gagal.
 */
esp_err_t mcp4725_delete(mcp4725_handle_t dac);

/**
 * @brief Menulis kode DAC 12-bit dan mode keluaran tanpa mengubah EEPROM.
 *
 * @param[in] dac Handle DAC.
 * @param[in] code Kode keluaran 0 sampai `MCP4725_DAC_CODE_MAX`.
 * @param[in] power_down Mode aktif atau mode resistor power-down.
 * @param[in] write_mode Fast Write atau DAC Register Write.
 * @return `ESP_OK` jika transaksi berhasil; error jika nilai atau transaksi gagal.
 */
esp_err_t mcp4725_set_raw(mcp4725_handle_t dac,
                          uint16_t code,
                          mcp4725_power_down_t power_down,
                          mcp4725_write_mode_t write_mode);

/**
 * @brief Mengubah tegangan permintaan menjadi kode DAC lalu menulisnya.
 *
 * Tegangan harus berada pada rentang representasi `VDD × 4095 / 4096`.
 * Pembulatan kode dilakukan ke nilai integer terdekat; nilai masukan di luar
 * rentang tidak dipotong otomatis.
 *
 * @param[in] dac Handle DAC.
 * @param[in] voltage_v Tegangan keluaran yang diminta dalam volt.
 * @param[in] power_down Mode aktif atau mode resistor power-down.
 * @param[in] write_mode Metode penulisan DAC register.
 * @return `ESP_OK` jika transaksi berhasil; `ESP_ERR_INVALID_ARG` jika tegangan
 *         atau enum tidak valid.
 */
esp_err_t mcp4725_set_voltage(mcp4725_handle_t dac,
                              float voltage_v,
                              mcp4725_power_down_t power_down,
                              mcp4725_write_mode_t write_mode);

/**
 * @brief Membaca status, kode DAC aktif, dan konfigurasi EEPROM.
 *
 * @param[in] dac Handle DAC.
 * @param[out] readback Menerima data hasil baca jika transaksi berhasil.
 * @return `ESP_OK` jika berhasil; kode error I2C jika pembacaan gagal.
 */
esp_err_t mcp4725_get_readback(mcp4725_handle_t dac,
                               mcp4725_readback_t *readback);

/**
 * @brief Membaca kode aktif lalu menghitung tegangan ideal berdasarkan VDD.
 *
 * Hasil bukan pengukuran analog pada pin VOUT. Bila `readback` bukan NULL,
 * status perangkat turut disalin ke struktur tersebut.
 *
 * @param[in] dac Handle DAC.
 * @param[out] voltage_v Tegangan ideal hasil konversi kode DAC dalam volt.
 * @param[out] readback Struktur status opsional.
 * @return `ESP_OK` jika berhasil; kode error I2C jika pembacaan gagal.
 */
esp_err_t mcp4725_get_voltage(mcp4725_handle_t dac,
                              float *voltage_v,
                              mcp4725_readback_t *readback);

/**
 * @brief Mengubah mode power-down menggunakan kode DAC terakhir yang diketahui.
 *
 * Jika DAC mungkin diubah oleh pemilik lain, baca state terlebih dahulu dengan
 * `mcp4725_get_readback()`.
 *
 * @param[in] dac Handle DAC.
 * @param[in] power_down Mode keluaran yang diinginkan.
 * @param[in] write_mode Metode penulisan DAC register.
 * @return `ESP_OK` jika transaksi berhasil; error jika parameter atau transaksi gagal.
 */
esp_err_t mcp4725_set_power_down(mcp4725_handle_t dac,
                                 mcp4725_power_down_t power_down,
                                 mcp4725_write_mode_t write_mode);

/**
 * @brief Menulis kode dan mode power-down ke DAC register serta EEPROM.
 *
 * Operasi ini lebih lambat daripada penulisan DAC biasa dan memakai endurance
 * EEPROM. Fungsi menunggu hingga perangkat siap sebelum dan sesudah penulisan.
 *
 * @param[in] dac Handle DAC.
 * @param[in] code Kode keluaran 0 sampai 4095.
 * @param[in] power_down Mode yang disimpan bersama kode.
 * @return `ESP_OK` jika EEPROM selesai ditulis; timeout atau error I2C jika gagal.
 */
esp_err_t mcp4725_save_to_eeprom(mcp4725_handle_t dac,
                                 uint16_t code,
                                 mcp4725_power_down_t power_down);

/**
 * @brief Mengubah tegangan ke kode, lalu menyimpannya ke DAC register dan EEPROM.
 *
 * Gunakan hanya ketika nilai persisten memang berubah; jangan panggil dalam
 * loop pembaruan rutin.
 *
 * @param[in] dac Handle DAC.
 * @param[in] voltage_v Tegangan yang akan disimpan dalam volt.
 * @param[in] power_down Mode yang disimpan bersama kode.
 * @return `ESP_OK` jika EEPROM selesai ditulis; error jika argumen atau I2C gagal.
 */
esp_err_t mcp4725_save_voltage_to_eeprom(mcp4725_handle_t dac,
                                         float voltage_v,
                                         mcp4725_power_down_t power_down);

/**
 * @brief Menunggu hingga perangkat melaporkan EEPROM siap.
 *
 * Fungsi memblokir task pemanggil selama polling. Timeout ini membatasi waktu
 * tunggu keseluruhan secara pendekatan; setiap transaksi tetap menggunakan
 * timeout I2C pada konfigurasi handle.
 *
 * @param[in] dac Handle DAC.
 * @param[in] timeout_ms Batas waktu tunggu dalam milidetik; harus lebih dari nol.
 * @return `ESP_OK` saat siap, `ESP_ERR_TIMEOUT` jika waktu habis, atau error I2C.
 */
esp_err_t mcp4725_wait_ready(mcp4725_handle_t dac, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
