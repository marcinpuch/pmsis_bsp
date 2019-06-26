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

#ifndef __BSP__BSP_H__
#define __BSP__BSP_H__

#if defined(CONFIG_GAPUINO)
#include <bsp/gapuino.h>
#endif

#if defined(CONFIG_GAPOC_A)
#include <bsp/gapoc_a.h>
#endif

#if defined(CONFIG_AI_DECK)
#include <bsp/ai_deck.h>
#endif

#if defined(CONFIG_HIMAX)
#include "bsp/camera/himax.h"
void bsp_himax_conf_init(struct himax_conf *conf);
int bsp_himax_open(struct himax_conf *conf);
#endif

#if defined(CONFIG_MT9V034)
#include "bsp/camera/mt9v034.h"
void bsp_mt9v034_conf_init(struct mt9v034_conf *conf);
int bsp_mt9v034_open(struct mt9v034_conf *conf);
#endif

#if defined(CONFIG_ILI9341)
#include "bsp/display/ili9341.h"
void bsp_ili9341_conf_init(struct ili9341_conf *conf);
int bsp_ili9341_open(struct ili9341_conf *conf);

#endif

#if defined(CONFIG_HYPERFLASH)
#include "bsp/flash/hyperflash.h"
void bsp_hyperflash_conf_init(struct hyperflash_conf *conf);
int bsp_hyperflash_open(struct hyperflash_conf *conf);
#endif

#if defined(CONFIG_NINA_W10)
#include "bsp/transport/nina_w10.h"
void bsp_nina_w10_conf_init(struct nina_w10_conf *conf);
int bsp_nina_w10_open(struct nina_w10_conf *conf);
#endif


#endif