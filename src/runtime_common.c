/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright 2013 Joshua C. Klontz                                           *
 *                                                                           *
 * Licensed under the Apache License, Version 2.0 (the "License");           *
 * you may not use this file except in compliance with the License.          *
 * You may obtain a copy of the License at                                   *
 *                                                                           *
 *     http://www.apache.org/licenses/LICENSE-2.0                            *
 *                                                                           *
 * Unless required by applicable law or agreed to in writing, software       *
 * distributed under the License is distributed on an "AS IS" BASIS,         *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  *
 * See the License for the specific language governing permissions and       *
 * limitations under the License.                                            *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "likely/runtime.h"

void likely_assert(bool condition, const char *format, ...)
{
    va_list ap;
    if (condition)
        return;

    va_start(ap, format);
    fprintf(stderr, "Likely ");
    vfprintf(stderr, format, ap);
    fprintf(stderr, "\n");

#ifdef _WIN32
    exit(EXIT_FAILURE); // We prefer not to trigger the Windows crash dialog box
#else // !_WIN32
    abort();
#endif // _WIN32
}

size_t likely_bits(const size_t value, const size_t mask)
{
//! [likely_bits implementation.]
    return value & mask;
//! [likely_bits implementation.]
}

void likely_set_bits(size_t *value, const size_t new_value, const size_t mask)
{
//! [likely_set_bits implementation.]
    *value &= ~mask;
    *value |= new_value & mask;
//! [likely_set_bits implementation.]
}

bool likely_bit(const size_t value, const size_t mask)
{
//! [likely_bit implementation.]
    return (value & mask) != 0;
//! [likely_bit implementation.]
}

void likely_set_bit(size_t *value, const bool new_value, const size_t mask)
{
//! [likely_set_bit implementation.]
    if (new_value) *value |= mask;
    else           *value &= ~mask;
//! [likely_set_bit implementation.]
}

size_t likely_depth(likely_type type) { return likely_bits(type, likely_matrix_depth); }
void likely_set_depth(likely_type *type, size_t depth) { likely_set_bits(type, depth, likely_matrix_depth); }

likely_size likely_elements(likely_const_mat m)
{
    return m->channels * m->columns * m->rows * m->frames;
}

likely_size likely_bytes(likely_const_mat m)
{
    return (likely_depth(m->type) * likely_elements(m) + 7) / 8;
}

likely_mat likely_new(likely_type type, likely_size channels, likely_size columns, likely_size rows, likely_size frames, void const *data)
{
    likely_mat m;
    const size_t dataBytes = (((uint64_t)likely_depth(type)) * channels * columns * rows * frames + 7) / 8;
    const size_t bytes = sizeof(struct likely_matrix) + dataBytes;
    m = (likely_mat) malloc(bytes);
    m->bytes = bytes;
    m->ref_count = 1;
    m->type = type;
    m->channels = channels;
    m->columns = columns;
    m->rows = rows;
    m->frames = frames;

    if (channels > 1) m->type |= likely_matrix_multi_channel;
    if (columns  > 1) m->type |= likely_matrix_multi_column;
    if (rows     > 1) m->type |= likely_matrix_multi_row;
    if (frames   > 1) m->type |= likely_matrix_multi_frame;

    if (data)
        memcpy((void*)m->data, data, dataBytes);

    return m;
}

likely_mat likely_scalar(likely_type type, double value)
{
    return likely_scalar_n(type, &value, 1);
}

likely_mat likely_scalar_n(likely_type type, double *values, size_t n)
{
    likely_mat m = likely_new(type, n, 1, 1, 1, NULL);
    for (size_t i=0; i<n; i++)
        likely_set_element(m, values[i], i, 0, 0, 0);
    return m;
}

likely_mat likely_scalar_va(likely_type type, double value, ...)
{
    double *values = NULL;
    int i = 0;

    va_list ap;
    va_start(ap, value);
    while (!isnan(value)) {
        if (i % 2 == 0)
            values = realloc(values, 2 * (i == 0 ? 1 : i) * sizeof(double));
        values[i++] = value;
        value = va_arg(ap, double);
    }
    va_end(ap);

    return likely_scalar_n(type, values, i);
}

likely_mat likely_string(const char *str)
{
    return likely_new(likely_matrix_i8, strlen(str)+1, 1, 1, 1, str);
}

likely_mat likely_void()
{
    return likely_new(likely_matrix_void, 0, 0, 0, 0, NULL);
}

likely_mat likely_copy(likely_const_mat m)
{
    return likely_new(m->type, m->channels, m->columns, m->rows, m->frames, m->data);
}

likely_mat likely_retain(likely_const_mat m)
{
    if (!m) return NULL;
    assert(m->ref_count > 0);
    ((likely_mat) m)->ref_count++;
    return (likely_mat) m;
}

void likely_release(likely_const_mat m)
{
    if (!m) return;
    assert(m->ref_count > 0);
    if (--((likely_mat) m)->ref_count) return;
    free((void*) m);
}

likely_type likely_c_type(likely_type type)
{
    likely_type c_type = type & likely_matrix_element;
    c_type &= ~likely_matrix_saturated;
    if (c_type & likely_matrix_floating)
        c_type &= ~likely_matrix_signed;
    return c_type;
}

double likely_element(likely_const_mat m, likely_size c, likely_size x, likely_size y, likely_size t)
{
    likely_size columnStep, rowStep, frameStep, index;
    if (!m)
        return NAN;

    columnStep = m->channels;
    rowStep = m->columns * columnStep;
    frameStep = m->rows * rowStep;
    index = t*frameStep + y*rowStep + x*columnStep + c;

    switch (likely_c_type(m->type)) {
      case likely_matrix_u8:  return (double) (( uint8_t const*) m->data)[index];
      case likely_matrix_u16: return (double) ((uint16_t const*)m->data)[index];
      case likely_matrix_u32: return (double) ((uint32_t const*)m->data)[index];
      case likely_matrix_u64: return (double) ((uint64_t const*)m->data)[index];
      case likely_matrix_i8:  return (double) ((  int8_t const*)m->data)[index];
      case likely_matrix_i16: return (double) (( int16_t const*)m->data)[index];
      case likely_matrix_i32: return (double) (( int32_t const*)m->data)[index];
      case likely_matrix_i64: return (double) (( int64_t const*)m->data)[index];
      case likely_matrix_f32: return (double) ((   float const*)m->data)[index];
      case likely_matrix_f64: return (double) ((  double const*)m->data)[index];
      case likely_matrix_u1:  return (double) ((((uint8_t const*)m->data)[index/8] & (1 << index%8)) != 0);
      default: assert(!"likely_element unsupported type");
    }
    return NAN;
}

void likely_set_element(likely_mat m, double value, likely_size c, likely_size x, likely_size y, likely_size t)
{
    likely_size columnStep, rowStep, frameStep, index;
    if (!m)
        return;

    columnStep = m->channels;
    rowStep = m->columns * columnStep;
    frameStep = m->rows * rowStep;
    index = t*frameStep + y*rowStep + x*columnStep + c;

    switch (likely_c_type(m->type)) {
      case likely_matrix_u8:  (( uint8_t*)m->data)[index] = ( uint8_t)value; break;
      case likely_matrix_u16: ((uint16_t*)m->data)[index] = (uint16_t)value; break;
      case likely_matrix_u32: ((uint32_t*)m->data)[index] = (uint32_t)value; break;
      case likely_matrix_u64: ((uint64_t*)m->data)[index] = (uint64_t)value; break;
      case likely_matrix_i8:  ((  int8_t*)m->data)[index] = (  int8_t)value; break;
      case likely_matrix_i16: (( int16_t*)m->data)[index] = ( int16_t)value; break;
      case likely_matrix_i32: (( int32_t*)m->data)[index] = ( int32_t)value; break;
      case likely_matrix_i64: (( int64_t*)m->data)[index] = ( int64_t)value; break;
      case likely_matrix_f32: ((   float*)m->data)[index] = (   float)value; break;
      case likely_matrix_f64: ((  double*)m->data)[index] = (  double)value; break;
      case likely_matrix_u1:  if (value == 0) (((uint8_t*)m->data)[index/8] &= ~(1 << index%8));
                              else            (((uint8_t*)m->data)[index/8] |=  (1 << index%8)); break;
      default: assert(!"likely_set_element unsupported type");
    }
}

bool likely_is_string(likely_const_mat m)
{
    return m && (m->type == likely_matrix_string) && !m->data[likely_elements(m)-1];
}
