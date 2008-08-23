
//
//    Copyright (C) 2007-2008 Sebastian Kuzminsky
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
//

#include <linux/slab.h>

#include "rtapi.h"
#include "rtapi_app.h"
#include "rtapi_string.h"
#include "rtapi_math.h"

#include "hal.h"

#include "hal/drivers/mesa-hostmot2/hostmot2.h"




int hm2_encoder_parse_md(hostmot2_t *hm2, int md_index) {
    hm2_module_descriptor_t *md = &hm2->md[md_index];
    int r;


    // 
    // some standard sanity checks
    //

    if (!hm2_md_is_consistent(hm2, md_index, 2, 5, 4, 0x0003)) {
        ERR("inconsistent Module Descriptor!\n");
        return -EINVAL;
    }

    if (hm2->encoder.num_instances != 0) {
        ERR(
            "found duplicate Module Descriptor for %s (inconsistent firmware), not loading driver\n",
            hm2_get_general_function_name(md->gtag)
        );
        return -EINVAL;
    }

    if (hm2->config.num_encoders > md->instances) {
        ERR(
            "config.num_encoders=%d, but only %d are available, not loading driver\n",
            hm2->config.num_encoders,
            md->instances
        );
        return -EINVAL;
    }

    if (hm2->config.num_encoders == 0) {
        INFO("num_encoders=0, skipping encoder MD\n");
        return 0;
    }


    // 
    // looks good, start initializing
    // 


    if (hm2->config.num_encoders == -1) {
        hm2->encoder.num_instances = md->instances;
    } else {
        hm2->encoder.num_instances = hm2->config.num_encoders;
    }


    hm2->encoder.instance = (hm2_encoder_instance_t *)hal_malloc(hm2->encoder.num_instances * sizeof(hm2_encoder_instance_t));
    if (hm2->encoder.instance == NULL) {
        ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail;
    }

    hm2->encoder.stride = md->register_stride;
    hm2->encoder.clock_frequency = md->clock_freq;
    hm2->encoder.version = md->version;

    hm2->encoder.counter_addr = md->base_address + (0 * md->register_stride);
    hm2->encoder.latch_control_addr = md->base_address + (1 * md->register_stride);
    hm2->encoder.timestamp_div_addr = md->base_address + (2 * md->register_stride);
    hm2->encoder.timestamp_count_addr = md->base_address + (3 * md->register_stride);
    hm2->encoder.filter_rate_addr = md->base_address + (4 * md->register_stride);

    r = hm2_register_tram_read_region(hm2, hm2->encoder.counter_addr, (hm2->encoder.num_instances * sizeof(u32)), &hm2->encoder.counter_reg);
    if (r < 0) {
        ERR("error registering tram read region for Encoder Counter register (%d)\n", r);
        goto fail;
    }


    r = hm2_register_tram_read_region(hm2, hm2->encoder.latch_control_addr, (hm2->encoder.num_instances * sizeof(u32)), &hm2->encoder.control_reg);
    if (r < 0) {
        ERR("error registering tram read region for Encoder Control register (%d)\n", r);
        goto fail;
    }

    hm2->encoder.control_reg = (u32 *)kmalloc(hm2->encoder.num_instances * sizeof(u32), GFP_KERNEL);
    if (hm2->encoder.control_reg == NULL) {
        ERR("out of memory!\n");
        r = -ENOMEM;
        goto fail;
    }

    // export the encoders to HAL
    // FIXME: r hides the r in enclosing function, and it returns the wrong thing
    {
        int i;
        int r;
        char name[HAL_NAME_LEN + 2];

        for (i = 0; i < hm2->encoder.num_instances; i ++) {
            // pins
            rtapi_snprintf(name, HAL_NAME_LEN, "%s.encoder.%02d.count", hm2->llio->name, i);
            r = hal_pin_s32_new(name, HAL_OUT, &(hm2->encoder.instance[i].hal.pin.count), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                ERR("error adding pin '%s', aborting\n", name);
                goto fail;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.encoder.%02d.position", hm2->llio->name, i);
            r = hal_pin_float_new(name, HAL_OUT, &(hm2->encoder.instance[i].hal.pin.position), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                ERR("error adding pin '%s', aborting\n", name);
                goto fail;
            }

            // rtapi_snprintf(name, HAL_NAME_LEN, "%s.encoder.%02d.velocity", hm2->llio->name, i);
            // r = hal_pin_float_new(name, HAL_OUT, &(hm2->encoder.instance[i].hal.pin.velocity), hm2->llio->comp_id);
            // if (r != HAL_SUCCESS) {
            //     ERR("error adding pin '%s', aborting\n", name);
            //     goto fail;
            // }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.encoder.%02d.reset", hm2->llio->name, i);
            r = hal_pin_bit_new(name, HAL_IN, &(hm2->encoder.instance[i].hal.pin.reset), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                ERR("error adding pin '%s', aborting\n", name);
                goto fail;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.encoder.%02d.index-enable", hm2->llio->name, i);
            r = hal_pin_bit_new(name, HAL_IO, &(hm2->encoder.instance[i].hal.pin.index_enable), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                ERR("error adding pin '%s', aborting\n", name);
                goto fail;
            }


            // parameters
            rtapi_snprintf(name, HAL_NAME_LEN, "%s.encoder.%02d.scale", hm2->llio->name, i);
            r = hal_param_float_new(name, HAL_RW, &(hm2->encoder.instance[i].hal.param.scale), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                ERR("error adding param '%s', aborting\n", name);
                goto fail;
            }

            rtapi_snprintf(name, HAL_NAME_LEN, "%s.encoder.%02d.index-invert", hm2->llio->name, i);
            r = hal_param_bit_new(name, HAL_RW, &(hm2->encoder.instance[i].hal.param.index_invert), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                ERR("error adding param '%s', aborting\n", name);
                goto fail;
            }


            rtapi_snprintf(name, HAL_NAME_LEN, "%s.encoder.%02d.index-mask", hm2->llio->name, i);
            r = hal_param_bit_new(name, HAL_RW, &(hm2->encoder.instance[i].hal.param.index_mask), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                ERR("error adding param '%s', aborting\n", name);
                goto fail;
            }


            rtapi_snprintf(name, HAL_NAME_LEN, "%s.encoder.%02d.index-mask-invert", hm2->llio->name, i);
            r = hal_param_bit_new(name, HAL_RW, &(hm2->encoder.instance[i].hal.param.index_mask_invert), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                ERR("error adding param '%s', aborting\n", name);
                goto fail;
            }


            rtapi_snprintf(name, HAL_NAME_LEN, "%s.encoder.%02d.counter-mode", hm2->llio->name, i);
            r = hal_param_bit_new(name, HAL_RW, &(hm2->encoder.instance[i].hal.param.counter_mode), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                ERR("error adding param '%s', aborting\n", name);
                goto fail;
            }


            rtapi_snprintf(name, HAL_NAME_LEN, "%s.encoder.%02d.filter", hm2->llio->name, i);
            r = hal_param_bit_new(name, HAL_RW, &(hm2->encoder.instance[i].hal.param.filter), hm2->llio->comp_id);
            if (r != HAL_SUCCESS) {
                ERR("error adding param '%s', aborting\n", name);
                goto fail;
            }


            // init hal objects
            *(hm2->encoder.instance[i].hal.pin.count) = 0;  // FIXME: useless
            *(hm2->encoder.instance[i].hal.pin.position) = 0.0;  // FIXME: useless
            hm2->encoder.instance[i].hal.param.scale = 1.0;
            hm2->encoder.instance[i].hal.param.filter = 1;

        }
    }


    //
    // initialize the encoder control register buffers
    //

    {
        int i;
        for (i = 0; i < hm2->encoder.num_instances; i ++) {
            hm2->encoder.instance[i].prev_control = 
                hm2->encoder.control_reg[i] =
                    HM2_ENCODER_FILTER | HM2_ENCODER_INDEX_JUSTONCE;
                    // quad filter = 15 clocks
        }
    }


    return hm2->encoder.num_instances;


fail:
    hm2->encoder.num_instances = 0;
    return r;
}


void hm2_encoder_force_write(hostmot2_t *hm2) {
    if (hm2->encoder.num_instances == 0) return;
    hm2->llio->write(hm2->llio, hm2->encoder.latch_control_addr, hm2->encoder.control_reg, (hm2->encoder.num_instances * sizeof(u32)));
}


void hm2_encoder_tram_init(hostmot2_t *hm2) {
    int i;

    // all the encoders start where they are
    for (i = 0; i < hm2->encoder.num_instances; i ++) {
        hm2->encoder.instance[i].prev_counter = hm2->encoder.counter_reg[i];
    }
}

static void do_flag(u32 *reg, int condition, u32 bits) {
    if(condition) {
        *reg |= bits;
    } else {
        *reg &= ~bits;
    }
}

void hm2_encoder_process_tram_read(hostmot2_t *hm2) {
    int i;

    // read counters & timestamps
    for (i = 0; i < hm2->encoder.num_instances; i ++) {
        hm2_encoder_instance_t *instance;
        u16 count, timestamp;
        u16 prev_count, prev_timestamp;
        s32 count_diff;
        u32 control, prev_control;

        instance = &hm2->encoder.instance[i];

        count = hm2->encoder.counter_reg[i] & 0x0000ffff;
        timestamp = (hm2->encoder.counter_reg[i] >> 16) & 0x0000ffff;
        control = hm2->encoder.control_reg[i] & HM2_ENCODER_MASK;
        prev_control = instance->prev_control & 0xffff;

        // FIXME: maybe hm2_encoder_t should have u16 *prev_count, *prev_timestamp instead of u32 *prev_counter
        prev_count = hm2->encoder.instance[i].prev_counter & 0x0000ffff;
        prev_timestamp = (hm2->encoder.instance[i].prev_counter >> 16) & 0x0000ffff;

        if((prev_control & HM2_ENCODER_CLEAR_INDEX)
                && !(control & HM2_ENCODER_CLEAR_INDEX)) {
            (*instance->hal.pin.index_enable) = 0;
            *(hm2->encoder.instance[i].hal.pin.count) = prev_count = 0;
            prev_control &= ~HM2_ENCODER_CLEAR_INDEX;
        }

        count_diff = (s32)count - (s32)prev_count;

        if (count_diff > 32768) count_diff -= 65536;
        if (count_diff < -32768) count_diff += 65536;
        *(hm2->encoder.instance[i].hal.pin.count) += count_diff;

        do_flag(&control, *instance->hal.pin.index_enable,
                        HM2_ENCODER_CLEAR_INDEX | HM2_ENCODER_INDEX_JUSTONCE);
        do_flag(&control, instance->hal.param.index_invert,
                                            HM2_ENCODER_INDEX_POLARITY);
        do_flag(&control, instance->hal.param.index_mask,
                                            HM2_ENCODER_INDEX_MASK);
        do_flag(&control, instance->hal.param.index_mask_invert,
                                            HM2_ENCODER_INDEX_MASK_POLARITY);
        do_flag(&control, instance->hal.param.counter_mode,
                                            HM2_ENCODER_COUNTER_MODE);
        do_flag(&control, instance->hal.param.filter,
                                            HM2_ENCODER_FILTER);
        
        if(control != prev_control ) {
            hm2->llio->write(hm2->llio,
                hm2->encoder.latch_control_addr + i * hm2->encoder.stride,
                &control, sizeof(u32));
            instance->prev_control = control;
        }

        if (hm2->encoder.instance[i].hal.param.scale == 0.0) {
            WARN("encoder %d has invalid scale 0.0, setting to 1.0\n", i);
            hm2->encoder.instance[i].hal.param.scale = 1.0;
        }
        *(hm2->encoder.instance[i].hal.pin.position) = *(hm2->encoder.instance[i].hal.pin.count) / hm2->encoder.instance[i].hal.param.scale;

        // now we're here
        hm2->encoder.instance[i].prev_counter = hm2->encoder.counter_reg[i];
    }
}


void hm2_encoder_cleanup(hostmot2_t *hm2) {
}


void hm2_encoder_print_module(int msg_level, hostmot2_t *hm2) {
    int i;
    PRINT(msg_level, "Encoders: %d\n", hm2->encoder.num_instances);
    if (hm2->encoder.num_instances <= 0) return;
    PRINT(msg_level, "    clock_frequency: %d Hz (%s MHz)\n", hm2->encoder.clock_frequency, hm2_hz_to_mhz(hm2->encoder.clock_frequency));
    PRINT(msg_level, "    version: %d\n", hm2->encoder.version);
    PRINT(msg_level, "    counter_addr: 0x%04X\n", hm2->encoder.counter_addr);
    PRINT(msg_level, "    latch_control_addr: 0x%04X\n", hm2->encoder.latch_control_addr);
    PRINT(msg_level, "    timestamp_div_addr: 0x%04X\n", hm2->encoder.timestamp_div_addr);
    PRINT(msg_level, "    timestamp_count_addr: 0x%04X\n", hm2->encoder.timestamp_count_addr);
    PRINT(msg_level, "    filter_rate_addr: 0x%04X\n", hm2->encoder.filter_rate_addr);
    for (i = 0; i < hm2->encoder.num_instances; i ++) {
        PRINT(msg_level, "    instance %d:\n", i);
        PRINT(msg_level, "        hw:\n");
        PRINT(msg_level, "            counter = %04x.%04x\n", (hm2->encoder.counter_reg[i] >> 16), (hm2->encoder.counter_reg[i] & 0xffff));
    }
}

