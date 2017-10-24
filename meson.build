project(
  'ob-compositor', 'cpp'
, default_options : [
    'cpp_std=c++1z'
  , 'werror=true'
  , 'warning_level=3'
  ]
)

cpp_compiler = meson.get_compiler('cpp')
maybe_cpp_flags = [
  '-fmax-errors=3'
, '-ferror-limit=3'
, '-fvisibility=hidden'
]
cpp_flags = []
foreach flag : maybe_cpp_flags
  if cpp_compiler.has_argument(flag)
    cpp_flags += flag
  endif
endforeach

libdrm = dependency('libdrm', required : true)

executable(
  'ob-compositor'
, 'src/compositor.cpp'
, install: true
#, include_directories : include
#, link_with : [liboblong_input]
, dependencies : [libdrm]
, cpp_args : cpp_flags
)

#include = include_directories('include')
#libinput = dependency('libinput', required : true)
#libudev = dependency('libudev', required : true)
#threads = dependency('threads', required : true)
#
#liboblong_input = library(
#  'oblong-input'
#, [
#    'src/event_listener.cpp'
#  , 'src/polled_event_listener.cpp'
#  , 'src/logger.cpp'
#  ]
#, install: true
#, include_directories : include
#, dependencies : [libinput, libudev, threads]
#, cpp_args : cpp_flags
#)
#install_subdir('include/oblong', install_dir : 'include')
#
#inputcat = executable(
#  'ob-inputcat'
#, 'src/ob-inputcat.cpp'
#, install: true
#, include_directories : include
#, link_with : [liboblong_input]
#, cpp_args : cpp_flags
#)
#
#integration_test = executable(
#  'integration_test'
#, 'test/integration.cpp'
#, include_directories : include
#, link_with : [liboblong_input]
#, cpp_args : cpp_flags
#)
#
#test('integration', integration_test)

