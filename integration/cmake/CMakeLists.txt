# Copyright 2016 Jan Dalheimer <jan@dalheimer.de>
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

add_custom_target(dummy SOURCES ralph.cmake RalphHelpers.cmake RalphFunctions.cmake)

add_library(ralph_cmake_integration STATIC CMakeIntegration.h CMakeIntegration.cpp)
target_include_directories(ralph_cmake_integration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ralph_cmake_integration PUBLIC ralph_clientlib)

install(FILES ralph.cmake RalphHelpers.cmake RalphFunctions.cmake DESTINATION lib/cmake COMPONENT CMakeIntegration)
