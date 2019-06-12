/*
 * Copyright (C) 2019 GreenWaves Technologies
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "pmsis.h"

#include "bsp/gapuino.h"
#include "bsp/camera/himax.h"
#include "bsp/display/ili9341.h"
#include "bsp/flash/hyperflash.h"


void bsp_hyperflash_conf_init(struct hyperflash_conf *conf)
{
  conf->hyper_itf = CONFIG_HYPERFLASH_HYPER_ITF;
  conf->hyper_cs = CONFIG_HYPERFLASH_HYPER_CS;
}

int bsp_hyperflash_open(struct hyperflash_conf *conf)
{
  return 0;
}



void bsp_himax_conf_init(struct himax_conf *conf)
{
  conf->i2c_itf = CONFIG_HIMAX_I2C_ITF;
  conf->cpi_itf = CONFIG_HIMAX_CPI_ITF;
}

int bsp_himax_open(struct himax_conf *conf)
{
  return 0;
}



void bsp_ili9341_conf_init(struct ili9341_conf *conf)
{
  conf->gpio = CONFIG_ILI9341_GPIO;
  conf->spi_itf = CONFIG_ILI9341_SPI_ITF;
  conf->spi_cs = CONFIG_ILI9341_SPI_CS;

}

int bsp_ili9341_open(struct ili9341_conf *conf)
{
  if (!conf->skip_pads_config)
  {
 #ifndef __ZEPHYR__
  	rt_pad_set_function(CONFIG_ILI9341_GPIO_PAD, CONFIG_ILI9341_GPIO_PAD_FUNC);
 #endif
  }

  return 0;
}
