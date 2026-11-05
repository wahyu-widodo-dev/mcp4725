# Komponen MCP4725

Driver MCP4725 untuk ESP-IDF 5.3.0 atau lebih baru. MCP4725 adalah DAC satu
kanal 12-bit dengan keluaran tegangan dan EEPROM internal. Komponen
menggunakan I2C master baru ESP-IDF, sehingga dapat berbagi bus yang sama
dengan INA226 atau perangkat I2C lain. Alamat dan kecepatan SCL disetel per
perangkat saat `mcp4725_create()`.
Untuk konfigurasi controller/pin bus, berbagi bus dengan beberapa perangkat,
dan penggunaan bus terpisah, lihat
[dokumentasi komponen I2C bus](../i2c_bus/README.md).

## Fitur

- Tulis DAC dengan nilai raw 0–4095 atau nilai tegangan.
- Pilih Fast Write (2 byte) atau Write DAC Register (3 byte).
- Atur mode output normal atau power-down dengan resistor internal 1 kOhm,
  100 kOhm, atau 500 kOhm ke ground.
- Baca status Ready/Busy, POR, nilai DAC, mode power-down saat ini, serta nilai
  dan mode yang tersimpan di EEPROM.
- Simpan nilai DAC dan mode power-down ke EEPROM, lalu polling sampai penulisan
  selesai atau timeout.
- Ubah alamat I2C (0x60–0x67), kecepatan I2C, timeout transaksi, dan nilai VDD
  yang dipakai untuk konversi volt/raw.

## Koneksi perangkat keras

| MCP4725 | ESP32-S3 (contoh proyek) |
|---|---|
| VDD | 3V3 |
| VSS/GND | GND bersama |
| SDA | GPIO15 / SDA bus I2C aplikasi |
| SCL | GPIO16 / SCL bus I2C aplikasi |
| VOUT | Masukan analog beban (0 sampai mendekati VDD) |
| A0 | Menentukan bit alamat paling rendah sesuai level pada modul |

Alamat default adalah alamat I2C 7-bit `0x60`. Pin A0 dapat membuat alamat
menjadi `0x61`; varian alamat MCP4725 yang mendukung pin A2/A1 juga berada pada
rentang `0x60`–`0x67`. Gunakan alamat sesuai datasheet modul, bukan alamat yang
sudah digeser dengan bit R/W.

SDA dan SCL perlu pull-up ke tegangan logika yang aman. Jangan menghubungkan
pin VOUT ke beban yang melebihi kemampuan output DAC atau rentang tegangan
masukan perangkat berikutnya. MCP4725 hanya menghasilkan tegangan; ia bukan
sumber arus atau pengganti penguat daya.

## Siklus hidup

Buat bus I2C satu kali di aplikasi, lalu buat handle MCP4725 di atas bus itu.
Hapus handle DAC sebelum menghapus bus:

```c
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "i2c_bus.h"
#include "mcp4725.h"

i2c_master_bus_handle_t bus = NULL;
mcp4725_handle_t dac = NULL;

ESP_ERROR_CHECK(i2c_bus_create(GPIO_NUM_15, GPIO_NUM_16, &bus));

mcp4725_config_t config = mcp4725_get_default_config();
config.i2c_address = 0x60;
config.i2c_frequency_hz = 100000;
config.i2c_timeout_ms = 1000;
config.supply_voltage_v = 3.3f;
ESP_ERROR_CHECK(mcp4725_create(bus, &config, &dac));

/* Pakai DAC di sini. */

ESP_ERROR_CHECK(mcp4725_delete(dac));
dac = NULL;
ESP_ERROR_CHECK(i2c_bus_delete(bus));
bus = NULL;
```

`mcp4725_create()` memasang perangkat ke bus dan membaca state awal DAC/EEPROM.
Ia tidak mengubah output DAC. Konfigurasi `supply_voltage_v` harus cocok
dengan tegangan VDD aktual karena digunakan untuk mengubah volt ke kode DAC;
nilai ini tidak mengatur tegangan supply secara elektrik.

Default dari `mcp4725_get_default_config()`:

| Field | Default |
|---|---:|
| `i2c_address` | `0x60` |
| `i2c_frequency_hz` | 100000 Hz |
| `i2c_timeout_ms` | 1000 ms |
| `supply_voltage_v` | 3.3 V |

MCP4725 mendukung mode I2C lebih cepat daripada 400 kHz, tetapi komponen ini
membatasi clock per-device hingga 400 kHz agar sesuai dengan master ESP32-S3
yang digunakan proyek. `supply_voltage_v` harus mencerminkan VDD aktual dan
berada pada rentang operasi perangkat 2.7–5.5 V.

## Mengatur output

### Menulis kode DAC langsung

Kode adalah bilangan 12-bit unsigned. Kode 0 meminta output 0 V, kode 2048
kira-kira setengah VDD, dan kode 4095 meminta nilai paling tinggi:

```c
ESP_ERROR_CHECK(mcp4725_set_raw(dac,
                                2048,
                                MCP4725_POWER_DOWN_NORMAL,
                                MCP4725_WRITE_FAST));
```

`MCP4725_WRITE_FAST` mengirim dua byte dan memperbarui DAC register. Ini cocok
untuk update output rutin. `MCP4725_WRITE_DAC_REGISTER` memakai perintah DAC
register tiga byte; EEPROM tidak berubah pada kedua mode ini.

### Mengatur tegangan

```c
ESP_ERROR_CHECK(mcp4725_set_voltage(dac,
                                    1.65f,
                                    MCP4725_POWER_DOWN_NORMAL,
                                    MCP4725_WRITE_FAST));
```

Driver menghitung kode dengan rumus:

```text
VOUT ideal = VDD × DAC_code / 4096
DAC_code   = round(VOUT yang diminta × 4096 / VDD)
```

Nilai tegangan di luar rentang representasi ditolak dengan
`ESP_ERR_INVALID_ARG`; driver tidak diam-diam membatasi (clamp) input. Kode
maksimum adalah 4095, sehingga output ideal maksimum
`VDD × 4095 / 4096` (pada VDD 3.3 V sekitar 3.2992 V), bukan tepat VDD.
Tegangan aktual juga dipengaruhi toleransi VDD, offset/gain DAC, beban, dan
spesifikasi output MCP4725.

Untuk mendapatkan output dalam mode power-down:

| Enum | Efek pada VOUT |
|---|---|
| `MCP4725_POWER_DOWN_NORMAL` | DAC aktif dan output buffered |
| `MCP4725_POWER_DOWN_1K_TO_GND` | Output diputus dari buffer dan ditarik ke GND melalui 1 kOhm |
| `MCP4725_POWER_DOWN_100K_TO_GND` | Output ditarik ke GND melalui 100 kOhm |
| `MCP4725_POWER_DOWN_500K_TO_GND` | Output ditarik ke GND melalui 500 kOhm |

Mode power-down bukan sekadar mematikan output; pin VOUT mendapat jalur resistor
ke ground. Pertimbangkan impedansi rangkaian yang terhubung. Saat keluar dari
power-down, output memerlukan waktu settling sesuai datasheet.

### Membaca state dan tegangan

```c
mcp4725_readback_t state;
ESP_ERROR_CHECK(mcp4725_get_readback(dac, &state));

float requested_output_v;
ESP_ERROR_CHECK(mcp4725_get_voltage(dac, &requested_output_v, NULL));

ESP_LOGI("mcp4725",
         "DAC=%u, EEPROM=%u, ready=%d, power-down=%d",
         state.dac_code,
         state.eeprom_code,
         state.ready,
         state.dac_power_down);
```

`mcp4725_get_readback()` membaca lima byte status/DAC/EEPROM dari perangkat.
Data DAC dan EEPROM dikemas dalam pasangan byte: byte pertama membawa bit
D11–D4, sedangkan empat bit paling signifikan byte berikutnya membawa D3–D0.
Field `ready` bernilai false saat EEPROM sedang ditulis. `power_on_reset`
menunjukkan status POR yang dilaporkan chip. `dac_code` dan `dac_power_down`
adalah setting DAC aktif; `eeprom_code` dan `eeprom_power_down` adalah setting
nonvolatile.

`mcp4725_get_voltage()` mengubah kode DAC aktif ke tegangan ideal berdasarkan
VDD pada konfigurasi handle. Nilai ini bukan pengukuran analog pada pin VOUT.
Jika mode power-down aktif, angka tersebut tetap tegangan ideal dari kode DAC
yang tersimpan, bukan tegangan aktual pin setelah resistor pull-down.

### Mengubah power-down tanpa mengganti nilai

```c
ESP_ERROR_CHECK(mcp4725_set_power_down(dac,
                                      MCP4725_POWER_DOWN_100K_TO_GND,
                                      MCP4725_WRITE_FAST));
```

Fungsi ini memakai kode DAC aktif terakhir yang dicatat handle. Bila perangkat
diubah oleh master lain atau nilai terakhir tidak diketahui, panggil
`mcp4725_get_readback()` dahulu untuk menyegarkan state driver.

## EEPROM: menyimpan nilai saat power-on

DAC register adalah nilai aktif sementara. Ketika MCP4725 dinyalakan, perangkat
memuat kode dan mode power-down dari EEPROM. Untuk menyimpan output baru agar
digunakan setelah power cycle:

```c
ESP_ERROR_CHECK(mcp4725_save_voltage_to_eeprom(dac,
                                              1.65f,
                                              MCP4725_POWER_DOWN_NORMAL));
```

Atau gunakan kode raw:

```c
ESP_ERROR_CHECK(mcp4725_save_to_eeprom(dac,
                                      2048,
                                      MCP4725_POWER_DOWN_NORMAL));
```

Fungsi tersebut menulis DAC register sekaligus EEPROM, lalu menunggu bit
Ready/Busy kembali ready. Operasi EEPROM jauh lebih lambat daripada update DAC
biasa (dapat memerlukan puluhan milidetik). Gunakan hanya ketika konfigurasi
persisten memang berubah, bukan untuk loop kontrol atau update berulang.
EEPROM memiliki batas endurance; lihat datasheet MCP4725 yang digunakan.
Jangan jalankan perintah write lain ke device selama EEPROM sedang busy; chip
mengabaikan perintah tersebut. API EEPROM menunggu Ready sebelum memulai write
baru dan kembali menunggu sampai write selesai. Jika task lain memakai handle
yang sama, serialisasikan pemanggilan API di aplikasi.

`mcp4725_wait_ready(dac, timeout_ms)` dapat dipakai untuk menunggu penulisan
EEPROM yang dimulai oleh pemanggilan lain. Jika timeout habis, fungsi
mengembalikan `ESP_ERR_TIMEOUT`. Penulisan EEPROM kemungkinan sudah dimulai dan
akan berlanjut di dalam chip; baca ulang status sebelum mencoba perintah write
berikutnya.

Timeout `mcp4725_wait_ready()` mengukur waktu tunggu keseluruhan secara
pendekatan. Tiap transaksi baca status tetap dapat menunggu hingga timeout I2C
yang diatur pada `mcp4725_config_t`.

## Error dan batas API

Semua fungsi mengembalikan `esp_err_t`. Periksa `ESP_OK` sebelum menggunakan
hasil. Error umum:

| Error | Makna |
|---|---|
| `ESP_ERR_INVALID_ARG` | Handle/pointer salah, alamat atau enum di luar rentang, kode/tegangan tidak valid |
| `ESP_ERR_NO_MEM` | Alokasi handle gagal |
| `ESP_ERR_TIMEOUT` | Transaksi I2C atau waktu tunggu Ready melewati batas |
| Error I2C lain | Device tidak ACK, bus bermasalah, atau transaksi gagal |

API tidak menyediakan General Call Reset/Wake. General Call dikirim ke alamat
I2C `0x00` dan berpotensi memengaruhi perangkat lain yang mendukung general
call pada bus bersama; pemanggil harus menangani operasi broadcast tersebut
secara eksplisit bila memang dibutuhkan.

Komponen ini tidak melakukan lock tingkat aplikasi untuk beberapa task yang
memakai satu handle. Serialisasikan operasi bila satu MCP4725 diakses dari
beberapa task. Operasi I2C blocking harus dipanggil dari task, bukan ISR.

## Kompatibilitas dan distribusi

Komponen ini menggunakan I2C master baru ESP-IDF dan ditujukan untuk ESP-IDF
5.3.0 atau lebih baru. Komponen ESP-IDF yang digunakan dideklarasikan di
`CMakeLists.txt`; versi, dependensi ESP-IDF minimum, dan lisensi MIT tercatat
di `idf_component.yml`. Folder komponen dapat digunakan dari direktori
`components/` proyek maupun diterbitkan sebagai komponen terpisah. Driver ini
khusus ESP-IDF dan bukan abstraksi lintas Arduino atau platform lain.
