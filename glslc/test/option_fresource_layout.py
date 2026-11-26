# Copyright 2026 The Shaderc Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import expect
from glslc_test_framework import inside_glslc_testsuite
from placeholder import FileShader

# A vertex shader and a fragment shader that share some resources but declare
# them in different orders.  The vertex shader also declares a resource before
# the shared ones, and the fragment shader declares one after, so an independent
# sequential allocation would give the shared resources different slots.
VERTEX_SHADER = """#version 450
layout(location = 0) out vec2 uv;
uniform texture2D albedo;
uniform sampler albedo_sampler;
uniform Transform { mat4 mvp; } transform;
void main() {
  uv = vec2(0.0);
  gl_Position = transform.mvp * vec4(0.0, 0.0, 0.0, 1.0);
}
"""

FRAGMENT_SHADER = """#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
uniform sampler albedo_sampler;
uniform texture2D extra_noise;
uniform texture2D albedo;
void main() {
  color = texture(sampler2D(albedo, albedo_sampler), uv) +
          texture(sampler2D(extra_noise, albedo_sampler), uv);
}
"""

LAYOUT_ARGS = ['-fresource-layout',
               'albedo', 'texture', '0', '3',
               'albedo_sampler', 'sampler', '0', '4',
               'transform', 'ubo', '0', '0']


@inside_glslc_testsuite('OptionFResourceLayout')
class ResourceLayoutRequiresAutoBind(expect.ErrorMessageSubstr):
    """Tests that -fresource-layout only affects automatic binding, so a shader
    with unbound resources still fails without -fauto-bind-uniforms."""

    shader = FileShader(FRAGMENT_SHADER, '.frag')
    glslc_args = ['-S', shader] + LAYOUT_ARGS
    expected_error_substr = "sampler/texture/image requires layout(binding=X)"


@inside_glslc_testsuite('OptionFResourceLayout')
class ResourceLayoutGivesSharedResourcesLayoutSlots(
        expect.ValidAssemblyFileWithSubstr):
    """Tests that shared resources use the layout's set and binding."""

    shader = FileShader(FRAGMENT_SHADER, '.frag')
    glslc_args = ['-S', shader, '-fauto-bind-uniforms'] + LAYOUT_ARGS
    expected_assembly_substrings = [
        "OpDecorate %albedo Binding 3",
        "OpDecorate %albedo DescriptorSet 0",
        "OpDecorate %albedo_sampler Binding 4",
        "OpDecorate %albedo_sampler DescriptorSet 0",
    ]


@inside_glslc_testsuite('OptionFResourceLayout')
class ResourceLayoutKeepsStageOnlyResourceOffReservedSlots(
        expect.ValidAssemblyFileWithSubstr):
    """Tests that a resource only one stage sees never takes a slot the layout
    reserves for another stage."""

    shader = FileShader(FRAGMENT_SHADER, '.frag')
    glslc_args = ['-S', shader, '-fauto-bind-uniforms'] + LAYOUT_ARGS
    expected_assembly_substr = "OpDecorate %extra_noise Binding 1"


@inside_glslc_testsuite('OptionFResourceLayout')
class ResourceLayoutSameSlotsInVertexAndFragment(
        expect.ValidAssemblyFileWithSubstr):
    """Tests that the same layout applied to a vertex shader gives the shared
    resources the same slots as the fragment shader, despite the different
    declaration orders and different resource counts."""

    shader = FileShader(VERTEX_SHADER, '.vert')
    glslc_args = ['-S', shader, '-fauto-bind-uniforms'] + LAYOUT_ARGS
    expected_assembly_substrings = [
        "OpDecorate %albedo Binding 3",
        "OpDecorate %albedo_sampler Binding 4",
        "OpDecorate %transform Binding 0",
    ]


@inside_glslc_testsuite('OptionFResourceLayout')
class ResourceLayoutConflictWithExplicitBindingIsError(
        expect.ErrorMessageSubstr):
    """Tests that a shader's explicit binding that disagrees with the layout is
    reported, naming the resource and the conflicting slots."""

    shader = FileShader("""#version 450
layout(location = 0) out vec4 color;
layout(binding = 9) uniform sampler2D albedo;
void main() { color = texture(albedo, vec2(0.0)); }
""", '.frag')
    glslc_args = ['-S', shader, '-fauto-bind-uniforms',
                  '-fresource-layout', 'albedo', 'texture', '0', '3']
    expected_error_substr = (
        "resource 'albedo' explicitly uses set 0, binding 9, which conflicts "
        "with the shared resource layout entry for 'albedo' (texture) at "
        "set 0, binding 3")


@inside_glslc_testsuite('OptionFResourceLayout')
class ResourceLayoutRejectsUnknownKind(expect.ErrorMessageSubstr):
    """Tests that an unknown resource kind is rejected."""

    shader = FileShader(FRAGMENT_SHADER, '.frag')
    glslc_args = ['-S', shader, '-fauto-bind-uniforms',
                  '-fresource-layout', 'albedo', 'bogus', '0', '3']
    expected_error_substr = "unknown resource kind 'bogus'"


@inside_glslc_testsuite('OptionFResourceLayout')
class ResourceLayoutRequiresAGroup(expect.ErrorMessageSubstr):
    """Tests that -fresource-layout without any group is an error."""

    shader = FileShader(FRAGMENT_SHADER, '.frag')
    glslc_args = ['-S', shader, '-fauto-bind-uniforms', '-fresource-layout']
    expected_error_substr = "requires at least one <name> <kind> <set> <binding>"
