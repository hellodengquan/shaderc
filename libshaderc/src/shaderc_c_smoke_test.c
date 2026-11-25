// Copyright 2016 The Shaderc Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "shaderc/shaderc.h"
#include <assert.h>
#include <string.h>

// Because we want to test this as a plain old C file, we cannot use
// gtest, so just run a simple smoke test.

int main() {
  const char* test_program =
      "#version 310 es\n"
      "layout(location = 0) in highp vec4 vtxColor;\n"
      "layout(location = 0) out highp vec4 outColor;\n"
      "void main() {\n"
      "  outColor = vtxColor;"
      "}\n";
  shaderc_compiler_t compiler;
  shaderc_compilation_result_t result;
  shaderc_compile_options_t options;

  compiler = shaderc_compiler_initialize();
  options = shaderc_compile_options_initialize();
  shaderc_compile_options_add_macro_definition(options, "FOO", 3, "1", 1);
  result = shaderc_compile_into_spv(
      compiler, test_program, strlen(test_program),
      shaderc_glsl_fragment_shader, "a.glsl", "main", options);

  assert(result);

  if (shaderc_result_get_compilation_status(result) !=
      shaderc_compilation_status_success) {
    // Early exit on failure.
    return -1;
  }
  shaderc_result_release(result);

  // Compile a group of entry points in one request.  This is mostly here to
  // check that the C interface stays usable from plain C.
  {
    const shaderc_entry_point entry_points[] = {
        {"main", shaderc_fragment_shader},
        {"other", shaderc_vertex_shader},
    };
    shaderc_compilation_result_set_t result_set =
        shaderc_compile_into_spv_multiple_entry_points(
            compiler, test_program, strlen(test_program), "a.glsl",
            entry_points, 2, options);
    assert(result_set);
    assert(shaderc_result_set_get_num_results(result_set) == 2);
    assert(strcmp(shaderc_result_set_get_entry_point_name(result_set, 1),
                  "other") == 0);
    assert(shaderc_result_set_get_entry_point_stage(result_set, 1) ==
           shaderc_vertex_shader);
    // The fragment entry point above compiled; the vertex one did not.
    assert(shaderc_result_get_compilation_status(
               shaderc_result_set_get_result(result_set, 0)) ==
           shaderc_compilation_status_success);
    shaderc_result_set_release(result_set);
  }

  shaderc_compile_options_release(options);
  shaderc_compiler_release(compiler);

  return 0;
}

