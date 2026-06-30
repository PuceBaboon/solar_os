#include "solar_os_spi.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_board.h"
#include "solar_os_board_caps.h"

#if SOLAR_OS_BOARD_HAS_SPI
#include "spi_bus.h"
#endif

#ifndef SOLAR_OS_BOARD_SPI_CS_SLOTS
#define SOLAR_OS_BOARD_SPI_CS_SLOTS {{.pin = -1, .name = NULL}}
#endif
#ifndef SOLAR_OS_BOARD_SPI_NAME
#define SOLAR_OS_BOARD_SPI_NAME "SPI"
#endif

#if SOLAR_OS_BOARD_HAS_SPI
static const solar_os_spi_cs_t spi_cs_slots[] = SOLAR_OS_BOARD_SPI_CS_SLOTS;
#endif

static bool spi_slot_active(const solar_os_spi_cs_t *slot)
{
    return slot != NULL && slot->pin >= 0 && slot->name != NULL && slot->name[0] != '\0';
}

esp_err_t solar_os_spi_init(void)
{
#if SOLAR_OS_BOARD_HAS_SPI
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_spi_get_status(solar_os_spi_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
#if SOLAR_OS_BOARD_HAS_SPI
    status->available = true;
    status->host = (int)solar_os_spi_bus_host();
    status->name = SOLAR_OS_BOARD_SPI_NAME;
    status->sclk_pin = solar_os_spi_bus_sclk_pin();
    status->miso_pin = solar_os_spi_bus_miso_pin();
    status->mosi_pin = solar_os_spi_bus_mosi_pin();
    status->max_transfer_size = solar_os_spi_bus_max_transfer_size();
    status->default_speed_hz = SOLAR_OS_SPI_DEFAULT_SPEED_HZ;

    for (size_t i = 0;
         i < sizeof(spi_cs_slots) / sizeof(spi_cs_slots[0]) &&
             status->cs_count < SOLAR_OS_SPI_MAX_CS;
         i++) {
        if (!spi_slot_active(&spi_cs_slots[i])) {
            continue;
        }
        status->cs[status->cs_count++] = spi_cs_slots[i];
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_spi_resolve_cs(const char *name, int *pin)
{
    if (name == NULL || name[0] == '\0' || pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if SOLAR_OS_BOARD_HAS_SPI
    for (size_t i = 0; i < sizeof(spi_cs_slots) / sizeof(spi_cs_slots[0]); i++) {
        if (spi_slot_active(&spi_cs_slots[i]) && strcmp(name, spi_cs_slots[i].name) == 0) {
            *pin = spi_cs_slots[i].pin;
            return ESP_OK;
        }
    }

    char *end = NULL;
    errno = 0;
    const long parsed = strtol(name, &end, 0);
    if (errno != 0 || end == name || *end != '\0' || parsed < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(spi_cs_slots) / sizeof(spi_cs_slots[0]); i++) {
        if (spi_slot_active(&spi_cs_slots[i]) && parsed == spi_cs_slots[i].pin) {
            *pin = (int)parsed;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_spi_transfer(int cs_pin,
                                uint8_t mode,
                                uint32_t speed_hz,
                                const uint8_t *tx_data,
                                uint8_t *rx_data,
                                size_t len)
{
#if SOLAR_OS_BOARD_HAS_SPI
    if (speed_hz == 0) {
        speed_hz = SOLAR_OS_SPI_DEFAULT_SPEED_HZ;
    }
    if (speed_hz > SOLAR_OS_SPI_MAX_SPEED_HZ) {
        return ESP_ERR_INVALID_ARG;
    }
    return solar_os_spi_bus_transfer(cs_pin, mode, speed_hz, tx_data, rx_data, len);
#else
    (void)cs_pin;
    (void)mode;
    (void)speed_hz;
    (void)tx_data;
    (void)rx_data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
