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

"""Tests that diagnostics from an include file and the main shader are printed
in the order the compiler emits them, that their severity and the summary
counts agree, and that an object file exists exactly when the shader still
compiles.

The expectations follow the compile option contract in glslc/README.asciidoc:

  * -w suppresses all warning output; the warning is silently ignored.
  * -Werror shows warnings as errors and makes them fail compilation.
  * -w suppresses warnings before they would be converted to errors, so -w
    together with -Werror still succeeds when the only problems are warnings;
    genuine errors are reported regardless of -w.
"""

import expect
from glslc_test_framework import inside_glslc_testsuite
from placeholder import FileShader, SpecializedString
from environment import File, Directory

# An include whose only diagnostic (a warning) sits on line 9.
WARNING_INCLUDE = ''.join(
    '// include filler {}\n'.format(i) for i in range(1, 9)) + \
    'layout(location = 0) attribute float include_attr;\n'

# The include's warning is on line 9 while the main shader's warning is on
# line 5.  Diagnostics are emitted in source order (include first), not sorted
# by ascending line number, so this fixture would catch a line-number sort.
WARNING_MAIN = '''#version 400
#include "common.glsl"
// main filler 1
// main filler 2
layout(location = 1) attribute float main_attr;
void main() {}
'''

# Both sources warn on line 3.  Only the filename tells the two messages
# apart, so a reader must pair each message with its source by emission order.
SAME_LINE_INCLUDE = '''// filler 1
// filler 2
layout(location = 0) attribute float include_attr;
'''

SAME_LINE_MAIN = '''#version 400
#include "common.glsl"
layout(location = 1) attribute float main_attr;
void main() {}
'''

# The include has a warning on line 9 followed by a genuine error on line 10.
# The main shader has a warning on line 3 followed by two genuine errors on
# line 4.  Emission order is: include warning, include error, main warning,
# main error, main error.
MIXED_INCLUDE = ''.join(
    '// include filler {}\n'.format(i) for i in range(1, 9)) + '''\
layout(location = 0) attribute float include_attr;
int broken() {
}
'''

MIXED_MAIN = '''#version 400
#include "common.glsl"
layout(location = 1) attribute float main_attr;
int main() {
}
'''

DEPRECATED_ATTRIBUTE = 'attribute deprecated in version 130; ' + \
    'may be removed in future release\n'

MAIN_ATTR_WARNING = ':3: warning: ' + DEPRECATED_ATTRIBUTE
MAIN_ATTR_WARNING_ON_LINE_5 = ':5: warning: ' + DEPRECATED_ATTRIBUTE
MAIN_ENTRY_ERR = ':4: error: \'int\' :  entry point cannot return a value\n'
MAIN_NO_RETURN_ERR = ':4: error: \'\' : function does not return a value: ' + \
    'main\n'


@inside_glslc_testsuite('DiagnosticOrdering')
class WarningOrderIsEmissionOrderNotLineNumberOrder(
        expect.ValidObjectFileWithWarning):
    """Tests that a warning from the include (line 9) is shown before a warning
    from the main shader (line 5), i.e. in emission order.  Both messages
    remain warnings, are counted as two warnings, and the shader still
    produces a valid SPIR-V object file."""

    shader = FileShader(WARNING_MAIN, '.vert')
    include = SpecializedString('$directory/common.glsl')
    environment = Directory('.', [File('common.glsl', WARNING_INCLUDE)])
    glslc_args = ['-c', shader]

    expected_warning = [
        include, ':9: warning: ', DEPRECATED_ATTRIBUTE,
        shader, MAIN_ATTR_WARNING_ON_LINE_5,
        '2 warnings generated.\n']


@inside_glslc_testsuite('DiagnosticOrdering')
class SameLineNumberKeepsSourceEmissionOrder(
        expect.ValidObjectFileWithWarning):
    """Tests that when both sources diagnose the same line number, each
    message is still paired with the correct file in emission order: the
    included source is reported first, then the main shader.  Both are
    warnings and a valid object file is produced."""

    shader = FileShader(SAME_LINE_MAIN, '.vert')
    include = SpecializedString('$directory/common.glsl')
    environment = Directory('.', [File('common.glsl', SAME_LINE_INCLUDE)])
    glslc_args = ['-c', shader]

    expected_warning = [
        include, ':3: warning: ', DEPRECATED_ATTRIBUTE,
        shader, MAIN_ATTR_WARNING,
        '2 warnings generated.\n']


@inside_glslc_testsuite('DiagnosticOrdering')
class WerrorPromotesWarningsAndRemovesObjectFile(
        expect.ErrorMessage, expect.NoObjectFile, expect.NoOutputOnStdout):
    """Tests that -Werror presents the warnings from both sources with error
    severity, counts them as errors, fails compilation, and therefore leaves
    no object file behind.  The messages keep their emission order."""

    shader = FileShader(WARNING_MAIN, '.vert')
    include = SpecializedString('$directory/common.glsl')
    environment = Directory('.', [File('common.glsl', WARNING_INCLUDE)])
    glslc_args = ['-c', shader, '-Werror']

    expected_error = [
        include, ':9: error: ', DEPRECATED_ATTRIBUTE,
        shader, ':5: error: ', DEPRECATED_ATTRIBUTE,
        '2 errors generated.\n']


@inside_glslc_testsuite('DiagnosticOrdering')
class DashWSuppressesWarningsButKeepsObjectFile(
        expect.ValidObjectFile):
    """Tests that -w silently ignores the warnings from both the include and
    the main shader: nothing is printed, the summary is omitted, and the
    object file is still generated."""

    shader = FileShader(WARNING_MAIN, '.vert')
    environment = Directory('.', [File('common.glsl', WARNING_INCLUDE)])
    glslc_args = ['-c', shader, '-w']


@inside_glslc_testsuite('DiagnosticOrdering')
class DashWThenWerrorStillCompiles(expect.ValidObjectFile):
    """Tests the README contract that -w suppresses warnings before -Werror
    would convert them: together they emit nothing and still produce an
    object file."""

    shader = FileShader(WARNING_MAIN, '.vert')
    environment = Directory('.', [File('common.glsl', WARNING_INCLUDE)])
    glslc_args = ['-c', '-w', '-Werror', shader]


@inside_glslc_testsuite('DiagnosticOrdering')
class WerrorThenDashWStillCompiles(expect.ValidObjectFile):
    """Tests that the order of -w and -Werror on the command line does not
    matter; -w still suppresses the warnings before promotion."""

    shader = FileShader(WARNING_MAIN, '.vert')
    environment = Directory('.', [File('common.glsl', WARNING_INCLUDE)])
    glslc_args = ['-c', '-Werror', '-w', shader]


@inside_glslc_testsuite('DiagnosticOrdering')
class WarningsAndErrorsKeepEmissionOrderAcrossSources(
        expect.ErrorMessage, expect.NoObjectFile, expect.NoOutputOnStdout):
    """Tests that warnings and genuine errors from the include and the main
    shader are printed in emission order, each attributed to the file and line
    that produced it, and are summarized as 2 warnings and 3 errors.  Since
    compilation fails, no object file is written."""

    shader = FileShader(MIXED_MAIN, '.vert')
    include = SpecializedString('$directory/common.glsl')
    environment = Directory('.', [File('common.glsl', MIXED_INCLUDE)])
    glslc_args = ['-c', shader]

    expected_error = [
        include, ':9: warning: ', DEPRECATED_ATTRIBUTE,
        include, ':10: error: \'\' : function does not return a value: ',
        'broken\n',
        shader, MAIN_ATTR_WARNING,
        shader, MAIN_ENTRY_ERR,
        shader, MAIN_NO_RETURN_ERR,
        '2 warnings and 3 errors generated.\n']


@inside_glslc_testsuite('DiagnosticOrdering')
class DashWKeepsGenuineErrorsFromBothSources(
        expect.ErrorMessage, expect.NoObjectFile, expect.NoOutputOnStdout):
    """Tests that -w only drops the warnings: the real errors from the include
    and the main shader are still reported with error severity, counted as
    three errors, and no object file is produced."""

    shader = FileShader(MIXED_MAIN, '.vert')
    include = SpecializedString('$directory/common.glsl')
    environment = Directory('.', [File('common.glsl', MIXED_INCLUDE)])
    glslc_args = ['-c', shader, '-w']

    expected_error = [
        include, ':10: error: \'\' : function does not return a value: ',
        'broken\n',
        shader, MAIN_ENTRY_ERR,
        shader, MAIN_NO_RETURN_ERR,
        '3 errors generated.\n']


@inside_glslc_testsuite('DiagnosticOrdering')
class WerrorCountsPromotedWarningsWithRealErrors(
        expect.ErrorMessage, expect.NoObjectFile, expect.NoOutputOnStdout):
    """Tests that -Werror turns the two warnings into errors while the three
    genuine errors keep their error severity and position; the summary counts
    all five as errors and no object file is produced."""

    shader = FileShader(MIXED_MAIN, '.vert')
    include = SpecializedString('$directory/common.glsl')
    environment = Directory('.', [File('common.glsl', MIXED_INCLUDE)])
    glslc_args = ['-c', shader, '-Werror']

    expected_error = [
        include, ':9: error: ', DEPRECATED_ATTRIBUTE,
        include, ':10: error: \'\' : function does not return a value: ',
        'broken\n',
        shader, ':3: error: ', DEPRECATED_ATTRIBUTE,
        shader, MAIN_ENTRY_ERR,
        shader, MAIN_NO_RETURN_ERR,
        '5 errors generated.\n']


@inside_glslc_testsuite('DiagnosticOrdering')
class DashWWithWerrorReportsGenuineErrorsOnly(
        expect.ErrorMessage, expect.NoObjectFile, expect.NoOutputOnStdout):
    """Tests that -w and -Werror together suppress the warnings before
    promotion, leaving the genuine errors from the include and the main shader
    intact, counted as errors, with no object file."""

    shader = FileShader(MIXED_MAIN, '.vert')
    include = SpecializedString('$directory/common.glsl')
    environment = Directory('.', [File('common.glsl', MIXED_INCLUDE)])
    glslc_args = ['-c', '-w', '-Werror', shader]

    expected_error = [
        include, ':10: error: \'\' : function does not return a value: ',
        'broken\n',
        shader, MAIN_ENTRY_ERR,
        shader, MAIN_NO_RETURN_ERR,
        '3 errors generated.\n']


@inside_glslc_testsuite('DiagnosticOrdering')
class WerrorWithDashWReportsGenuineErrorsOnly(
        expect.ErrorMessage, expect.NoObjectFile, expect.NoOutputOnStdout):
    """Tests that putting -Werror before -w on the command line has the same
    result: warnings are suppressed first, so only the genuine errors are
    reported and counted."""

    shader = FileShader(MIXED_MAIN, '.vert')
    include = SpecializedString('$directory/common.glsl')
    environment = Directory('.', [File('common.glsl', MIXED_INCLUDE)])
    glslc_args = ['-c', '-Werror', '-w', shader]

    expected_error = [
        include, ':10: error: \'\' : function does not return a value: ',
        'broken\n',
        shader, MAIN_ENTRY_ERR,
        shader, MAIN_NO_RETURN_ERR,
        '3 errors generated.\n']
