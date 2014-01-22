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

#ifndef LIKELY_FRONTEND_H
#define LIKELY_FRONTEND_H

#include <likely/likely_export.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct likely_ast
{
    union {
        struct {
            struct likely_ast *atoms;
            size_t num_atoms;
        };
        struct {
            const char *atom;
            size_t atom_len;
        };
    };
    size_t begin, end; // indicies into the source string
    bool is_list;
} likely_ast;

// If 'str' starts with '(' the string is assumed to contain s-expressions,
// otherwise 'str' is assumed to be Github Flavored Markdown (GFM) with s-expression(s) in the code blocks
LIKELY_EXPORT likely_ast *likely_tokens_from_string(const char *str, size_t *num_tokens); // Return value managed internally and guaranteed until the next call to this function
LIKELY_EXPORT const char *likely_tokens_to_string(likely_ast *tokens, size_t num_tokens); // Return value managed internally and guaranteed until the next call to this function
LIKELY_EXPORT likely_ast likely_ast_from_tokens(likely_ast *tokens, size_t num_tokens); // Top level is a list of expressions
LIKELY_EXPORT likely_ast *likely_ast_to_tokens(const likely_ast ast, size_t *num_tokens); // Return value managed internally and guaranteed until the next call to this function
LIKELY_EXPORT likely_ast likely_ast_from_string(const char *str);
LIKELY_EXPORT const char *likely_ast_to_string(const likely_ast ast); // Return value managed internally and guaranteed until the next call to this function
LIKELY_EXPORT void likely_free_ast(likely_ast ast);

#ifdef __cplusplus
}
#endif

#endif // LIKELY_FRONTEND_H