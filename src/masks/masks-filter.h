#pragma once

#include <obs-module.h>
#include "masks.h"
#include "obs-utils.h"

static const char *mask_filter_name(void *unused);
static void *mask_filter_create(obs_data_t *settings, obs_source_t *source);
static void mask_filter_destroy(void *data);
static uint32_t mask_filter_width(void *data);
static uint32_t mask_filter_height(void *data);
static void mask_filter_update(void *data, obs_data_t *settings);
static void mask_filter_update_v2(void *data, obs_data_t *settings);
static void mask_filter_video_render(void *data, gs_effect_t *effect);
static bool mask_filter_multi_pass(mask_filter_data_t *filter);
static obs_properties_t *mask_filter_properties(void *data);
static void mask_filter_video_tick(void *data, float seconds);
static void mask_filter_defaults(obs_data_t *settings);
static void mask_filter_defaults_v2(obs_data_t *settings);
extern void get_input_source(base_filter_data_t *filter);
static void draw_output(mask_filter_data_t *filter);
static void mask_filter_render_filter(mask_filter_data_t *filter);
static void render_mask(mask_filter_data_t *filter);

static bool setting_mask_effect_modified(void *data, obs_properties_t *props, obs_property_t *p, obs_data_t *settings);

static bool setting_mask_adjustment_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings);

static bool setting_mask_type_modified(void *data, obs_properties_t *props, obs_property_t *p, obs_data_t *settings);
static void load_output_effect(mask_filter_data_t *filter);
