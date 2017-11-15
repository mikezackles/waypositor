project(
  'waypositor', ['c', 'cpp']
, default_options : [
    'cpp_std=c++1z'
  , 'werror=true'
  , 'warning_level=3'
  ]
)

c_compiler = meson.get_compiler('c')

cpp_compiler = meson.get_compiler('cpp')
maybe_cpp_flags = [
  '-fmax-errors=3'
, '-ferror-limit=3'
, '-fdiagnostics-show-template-tree'
#, '-fvisibility=hidden'
]
cpp_flags = []
foreach flag : maybe_cpp_flags
  if cpp_compiler.has_argument(flag)
    cpp_flags += flag
  endif
endforeach

libdrm = dependency('libdrm', required : true)
libgbm = dependency('gbm', required : true)
libegl = dependency('egl', required : true)
libgl = dependency('gl', required : true)
libwayland_client = dependency('wayland-client', required : true)
boost = dependency('boost', modules : ['system'], required : true)
threads = dependency('threads', required : false)

executable(
  'waypositor'
, 'src/compositor.cpp'
, install : true
, include_directories : include_directories('include')
#, link_with : [liboblong_input]
, dependencies : [libdrm, libgbm, libegl, libgl, boost, threads]
, cpp_args : cpp_flags
)

incdir = include_directories('protocol')

executable(
  'ob-client-dmabuf'
, 'clients/client-dmabuf.cpp'
, 'protocol/linux-dmabuf-unstable-v1-protocol.c'
, include_directories : incdir
, install : true
, dependencies : [libdrm, libgbm, libwayland_client, threads]
, cpp_args : cpp_flags
)

# This is temporary so that we can deal with just the protocol until we need
# graphics
executable(
  'protocol-server'
, 'src/protocol-server.cpp'
, install : true
, include_directories : include_directories('include')
, dependencies : [boost, threads]
, cpp_args : cpp_flags
, link_args : ['-lstdc++fs']
)
