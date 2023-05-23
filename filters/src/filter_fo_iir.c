/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include "esp_check.h"
#include "filter_fo_iir.h"

static const char *TAG = "filter.iir.fo";

typedef struct filter_fo_iir_t {
    float a1; //!< the denominator filter coefficient value for z^(-1)
    float b0; //!< the numerator filter coefficient value for z^0
    float b1; //!< the numerator filter coefficient value for z^(-1)
    float x1; //!< the input value at time sample n=-1
    float y1; //!< the output value at time sample n=-1
} filter_fo_iir_t;

esp_err_t filter_new_fo_iir(const filter_fo_iir_config_t *config, filter_fo_iir_handle_t *ret_filter)
{
    esp_err_t ret = ESP_OK;
    filter_fo_iir_t *iir_fo = NULL;
    ESP_RETURN_ON_FALSE(config && ret_filter, ESP_ERR_INVALID_ARG, TAG, "config and ret_filter must not be NULL");
    iir_fo = calloc(1, sizeof(filter_fo_iir_t));
    ESP_GOTO_ON_FALSE(iir_fo, ESP_ERR_NO_MEM, err, TAG, "no mem for iir_fo");

    iir_fo->a1 = config->coeffs.a1;
    iir_fo->b0 = config->coeffs.b0;
    iir_fo->b1 = config->coeffs.b1;
    iir_fo->x1 = config->initial_state.x1;
    iir_fo->y1 = config->initial_state.y1;
    *ret_filter = iir_fo;
    return ESP_OK;
err:
    // error handling
    if (iir_fo) {
        free(iir_fo);
    }
    return ret;
}

esp_err_t filter_del_fo_iir(filter_fo_iir_handle_t filter)
{
    ESP_RETURN_ON_FALSE(filter, ESP_ERR_INVALID_ARG, TAG, "filter must not be NULL");
    free(filter);
    return ESP_OK;
}

esp_err_t filter_fo_iir_run(filter_fo_iir_handle_t filter, const float input, float *ret_result)
{
    ESP_RETURN_ON_FALSE(filter && ret_result, ESP_ERR_INVALID_ARG, TAG, "filter and ret_result must not be NULL");

    float a1 = filter->a1;
    float b0 = filter->b0;
    float b1 = filter->b1;
    float x1 = filter->x1;
    float y1 = filter->y1;
    float y0 = 1.0;
    float x0 = input;

    //
    // Compute the output
    //
    y0 = (b0 * x0) + (b1 * x1) - (a1 * y1);

    //
    // Store values for next time
    //
    filter->x1 = input;
    filter->y1 = y0;

    *ret_result = y0;

    return ESP_OK;
}

esp_err_t filter_fo_iir_run_form_0(filter_fo_iir_handle_t filter, const float input, float *ret_result)
{
    ESP_RETURN_ON_FALSE(filter && ret_result, ESP_ERR_INVALID_ARG, TAG, "filter and ret_result must not be NULL");

    float a1 = filter->a1;
    float b0 = filter->b0;
    float y1 = filter->y1;

    //
    // Compute the output
    //
    float y0 = (b0 * input) - (a1 * y1);

    //
    // Store values for next time
    //
    filter->y1 = y0;

    *ret_result = y0;
    return ESP_OK;
}
