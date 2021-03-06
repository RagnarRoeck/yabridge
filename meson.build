project(
  'yabridge',
  'cpp',
  version : '1.7.1',
  default_options : ['warning_level=3', 'cpp_std=c++2a', 'build.cpp_std=c++2a']
)

# Meson does not let us set a default cross compiler, which makes sense, but it
# also means that it's easy to forget. This will cause the setup process to
# abort if no cross compiler has been set up.
winelib_check = '''#ifndef __WINE__
#error 1
#endif'''
if not meson.get_compiler('cpp').compiles(winelib_check)
  error('You need to set up a cross compiler, check the README for compilation instructions.')
endif

# Depending on the `use-bitbridge` flag we'll enable building secondary 32-bit
# host applications that can act as a bit bridge for using 32-bit Windows plugins
# in 64-bit Linux VST hosts. The plugin will determine which host application to
# use based on the `.dll` file it's trying to load.
# This setup is necessary until Meson provides a way to have multiple
# cross-builds for a single build directory:
# https://github.com/mesonbuild/meson/issues/5125
individual_host_name_64bit = 'yabridge-host'
individual_host_name_32bit = 'yabridge-host-32'
group_host_name_64bit = 'yabridge-group'
group_host_name_32bit = 'yabridge-group-32'

# Disable the use of concepts in Boost.Asio until Boost 1.73 gets released
# https://github.com/boostorg/asio/issues/312
compiler_options = ['-DBOOST_ASIO_DISABLE_CONCEPTS']

with_bitbridge = get_option('with-bitbridge')
with_static_boost = get_option('with-static-boost')
with_winedbg = get_option('with-winedbg')

# TODO: Remove the `use-*` versions of these options in yabridge 2.0.0
if not with_bitbridge and get_option('use-bitbridge')
  with_bitbridge = true
  warning('''The 'use-bitbridge' option has been deprecated and will be ''' +
          '''removed in yabridge 2.0.0, use the 'with-bitbridge' option ''' +
          '''instead.''')
endif
if not with_winedbg and get_option('use-winedbg')
  with_winedbg = true
  warning('''The 'use-winedbg' option has been deprecated and will be ''' +
          '''removed in yabridge 2.0.0, use the 'with-winedbg' option ''' +
          '''instead.''')
endif

if with_bitbridge
  compiler_options += '-DWITH_BITBRIDGE'
endif

# This provides an easy way to start the Wine VST host using winedbg since it
# can be quite a pain to set up
if with_winedbg
  compiler_options += '-DWITH_WINEDBG'
endif

# Generate header files for configuration variables such as the current git tag
# and the name of the host binary
subdir('src/common/config')

# Statically link against Boost.Filesystem, otherwise it would become impossible
# to distribute a prebuilt version of yabridge
boost_dep = dependency('boost', version : '>=1.66', static : with_static_boost)
boost_filesystem_dep = dependency(
  'boost',
  version : '>=1.66',
  modules : ['filesystem'],
  static : with_static_boost
)
bitsery_dep = subproject('bitsery').get_variable('bitsery_dep')
threads_dep = dependency('threads')
tomlplusplus_dep = subproject('tomlplusplus', version : '2.1.0').get_variable('tomlplusplus_dep')
# The built in threads dependency does not know how to handle winegcc
wine_threads_dep = declare_dependency(link_args : '-lpthread')
xcb_dep = dependency('xcb')

include_dir = include_directories('src/include')

# The application consists of a VST plugin (yabridge) that calls a winelib
# program (yabridge-host) that can host Windows VST plugins. More information
# about the way these two components work together can be found in the readme
# file.

shared_library(
  'yabridge',
  [
    'src/common/configuration.cpp',
    'src/common/logging.cpp',
    'src/common/serialization.cpp',
    'src/common/utils.cpp',
    'src/plugin/host-process.cpp',
    'src/plugin/plugin.cpp',
    'src/plugin/plugin-bridge.cpp',
    'src/plugin/utils.cpp',
    version_header,
  ],
  native : true,
  include_directories : include_dir,
  dependencies : [
    boost_dep,
    boost_filesystem_dep,
    bitsery_dep,
    threads_dep,
    tomlplusplus_dep
  ],
  cpp_args : compiler_options,
  link_args : ['-ldl']
)

host_sources = [
  'src/common/configuration.cpp',
  'src/common/logging.cpp',
  'src/common/serialization.cpp',
  'src/common/utils.cpp',
  'src/wine-host/bridges/vst2.cpp',
  'src/wine-host/editor.cpp',
  'src/wine-host/editor.cpp',
  'src/wine-host/utils.cpp',
  version_header,
]

individual_host_sources = host_sources + ['src/wine-host/individual-host.cpp']
group_host_sources = host_sources + [
  'src/wine-host/bridges/group.cpp',
  'src/wine-host/group-host.cpp',
]

executable(
  individual_host_name_64bit,
  individual_host_sources,
  native : false,
  include_directories : include_dir,
  dependencies : [
    boost_dep,
    boost_filesystem_dep,
    bitsery_dep,
    tomlplusplus_dep,
    wine_threads_dep,
    xcb_dep
  ],
  cpp_args : compiler_options + ['-m64'],
  link_args : ['-m64']
)

executable(
  group_host_name_64bit,
  group_host_sources,
  native : false,
  include_directories : include_dir,
  dependencies : [
    boost_dep,
    boost_filesystem_dep,
    bitsery_dep,
    tomlplusplus_dep,
    wine_threads_dep,
    xcb_dep
  ],
  cpp_args : compiler_options + ['-m64'],
  link_args : ['-m64']
)

if with_bitbridge
  message('Bitbridge enabled, configuring a 32-bit host application')

  # I honestly have no idea what the correct way is to have `dependency()` or
  # `compiler.find_dependency()` search for 32-bit versions of libraries when
  # cross-compiling. Meson also doesn't seem to respect the default linker
  # search path set by the system in `find_library()`. If anyone does know how
  # to properly do this, please let me know!
  winegcc = meson.get_compiler('cpp', native : false)
  boost_filesystem_dep = winegcc.find_library(
    'boost_filesystem',
    static : with_static_boost,
    dirs : [
      # Used by Arch based distros
      '/usr/local/lib32',
      '/usr/lib32',
      # Used by Debian based distros
      '/usr/local/lib/i386-linux-gnu',
      '/usr/lib/i386-linux-gnu',
      # Used by Red Hat based distros, could cause issues though since Meson
      # cannot differentiate between the 32-bit version and the regular 64-bit
      # version that would normally be in /lib
      '/usr/local/lib',
      '/usr/lib',
    ]
  )
  xcb_dep = winegcc.find_library('xcb')

  executable(
    individual_host_name_32bit,
    individual_host_sources,
    native : false,
    include_directories : include_dir,
    dependencies : [
      boost_dep,
      boost_filesystem_dep,
      bitsery_dep,
      tomlplusplus_dep,
      wine_threads_dep,
      xcb_dep
    ],
    # FIXME: 32-bit winegcc defines `__stdcall` differently than the 64-bit
    #        version, and one of the changes is the inclusion of
    #        `__atribute__((__force_align_arg_pointer__))`. For whetever reason
    #        this causes GCC to complain when using function pointers with the
    #        `__stdcall` calling convention in template arguments, although it
    #        otherwise works just fine. We don't ignore any warnings in the
    #        regular host application so this should not cause any issues!
    cpp_args : compiler_options + ['-m32', '-Wno-ignored-attributes'],
    link_args : ['-m32']
  )

  executable(
    group_host_name_32bit,
    group_host_sources,
    native : false,
    include_directories : include_dir,
    dependencies : [
      boost_dep,
      boost_filesystem_dep,
      bitsery_dep,
      tomlplusplus_dep,
      wine_threads_dep,
      xcb_dep
    ],
    cpp_args : compiler_options + ['-m32', '-Wno-ignored-attributes'],
    link_args : ['-m32']
  )
endif
