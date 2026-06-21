#ifndef MCAL_REG_HPP
#define MCAL_REG_HPP

#include <cstdint>
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include "esp_rom_gpio.h"

namespace mcal
{
    namespace gpio
    {
        inline void init_output_pin(uint8_t pin)
        {
            esp_rom_gpio_pad_select_gpio((gpio_num_t)pin);
            GPIO.enable_w1ts = (1ULL << pin);
        }

        inline void IRAM_ATTR set_pin_high(uint8_t pin)
        {
            GPIO.out_w1ts = (1ULL << pin);
        }

        inline void IRAM_ATTR set_pin_low(uint8_t pin)
        {
            GPIO.out_w1tc = (1ULL << pin);
        }
    }
}

#endif // MCAL_REG_HPP
