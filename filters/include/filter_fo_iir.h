/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief First-order IIR filter handle
 */
typedef struct filter_fo_iir_t *filter_fo_iir_handle_t;

/**
 * @brief First-order IIR filter configuration
 */
typedef struct {
    struct {
        float b0; /*!< the numerator filter coefficient value for z^0 */
        float b1; /*!< the numerator filter coefficient value for z^(-1) */
        float a1; /*!< the denominator filter coefficient value for z^(-1) */
    } coeffs;
    struct {
        float x1; /*!< the input value at time sample n=-1 */
        float y1; /*!< the output value at time sample n=-1 */
    } initial_state;
} filter_fo_iir_config_t;

/**
 * @brief
 *
 * @param config
 * @param ret_filter
 * @return esp_err_t
 */
esp_err_t filter_new_fo_iir(const filter_fo_iir_config_t *config, filter_fo_iir_handle_t *ret_filter);

/**
 * @brief
 *
 * @param filter
 * @return esp_err_t
 */
esp_err_t filter_del_fo_iir(filter_fo_iir_handle_t filter);

/**
 * @brief Runs a first-order filter of the form:
 *
 *        y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1]
 *
 * @param filter Filter handle
 * @param input The input value to filter
 * @param ret_result The output value from the filter
 * @return
 */
esp_err_t filter_fo_iir_run(filter_fo_iir_handle_t filter, const float input, float *ret_result);

/**
 * @brief Runs a first-order filter of the form:
 *
 *        y[n] = b0*x[n] - a1*y[n-1]
 *
 * @param filter Filter handle
 * @param input The input value to filter
 * @param ret_result The output value from the filter
 * @return
 */
esp_err_t filter_fo_iir_run_form_0(filter_fo_iir_handle_t filter, const float input, float *ret_result);

#ifdef __cplusplus
}
#endif
