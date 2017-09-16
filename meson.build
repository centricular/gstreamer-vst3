project('gstreamer-vst3', 'cpp',
  version : '0.1.0',
  meson_version : '>= 0.36.0',
  default_options : [ 'warning_level=1',
                      'buildtype=debugoptimized',
                      'cpp_std=c++11'])

plugins_install_dir = '@0@/gstreamer-1.0'.format(get_option('libdir'))

cxx = meson.get_compiler('cpp')

# FIXME: Requires GStreamer >= 1.12.3
#if cxx.has_argument('-fvisibility=hidden')
#  add_project_arguments('-fvisibility=hidden', language: 'cpp')
#endif

if cxx.get_id() == 'msvc'
  # Ignore several spurious warnings for things gstreamer does very commonly
  # If a warning is completely useless and spammy, use '/wdXXXX' to suppress it
  # If a warning is harmless but hard to fix, use '/woXXXX' so it's shown once
  # NOTE: Only add warnings here if you are sure they're spurious
  test_cppflags = [
      '/wd4018', # implicit signed/unsigned conversion
      '/wd4146', # unary minus on unsigned (beware INT_MIN)
      '/wd4244', # lossy type conversion (e.g. double -> int)
      '/wd4305', # truncating type conversion (e.g. double -> float)
  ]
  add_project_arguments(msvc_args, language : 'cpp')
  # Disable SAFESEH with MSVC for plugins and libs that use external deps that
  # are built with MinGW
  noseh_link_args = ['/SAFESEH:NO']
else
  test_cppflags = ['-Wno-non-virtual-dtor']
  noseh_link_args = []
endif

common_flags = []
foreach cxxflag: test_cppflags
  if cxx.has_argument(cxxflag)
    common_flags += [ cxxflag ]
  endif
endforeach

gst_dep = dependency('gstreamer-1.0', version : '>= 1.8', required : true)
gstbase_dep = dependency('gstreamer-base-1.0', version : '>= 1.8', required : true)
gstaudio_dep = dependency('gstreamer-audio-1.0', version : '>= 1.8', required : true)

vst_sdkdir = get_option('vst-sdkdir')
message('Looking for VST3 SDK in directory ' + vst_sdkdir)
if not cxx.has_header('vst/hosting/module.h', args : '-I@0@/public.sdk/source'.format(vst_sdkdir))
  error('Cannot find VST3 SDK')
endif
if not cxx.has_header('pluginterfaces/vst/ivstaudioprocessor.h', args : '-I@0@'.format(vst_sdkdir))
  error('Cannot find VST3 SDK plug interfaces')
endif

vst_includedir = '@0@/public.sdk/source'.format(vst_sdkdir)
vst_sourcedir = '@0@/public.sdk/source/'.format(vst_sdkdir)
vst_pluginterfaces_includedir = '@0@'.format(vst_sdkdir)

vst_sources = [
  vst_sourcedir + 'vst/vstinitiids.cpp',
  vst_sourcedir + 'vst/hosting/hostclasses.cpp',
  vst_sourcedir + 'vst/hosting/module.cpp',
  vst_sourcedir + 'vst/hosting/parameterchanges.cpp',
  vst_sourcedir + 'vst/hosting/stringconvert.cpp',
  vst_sourcedir + 'common/memorystream.cpp',
]

if host_machine.system() == 'windows'
  vst_platform_sources = [
    vst_sourcedir + 'vst/hosting/module_win32.cpp',
  ]
  platform_deps = []
elif host_machine.system() == 'linux'
  vst_platform_sources = [
    vst_sourcedir + 'vst/hosting/module_linux.cpp',
  ]
  threads_dep = dependency('threads', required : true)
  dl_dep = cxx.find_library('dl', required : true)

  # Need to do something different for clang and MSVC here
  libcpp_fs_dep = cxx.find_library('libstdc++fs', required : true)

  platform_deps = [dl_dep, threads_dep, libcpp_fs_dep]
elif host_machine.system() == 'osx'
  vst_platform_sources = [
    vst_sourcedir + 'vst/hosting/module_mac.mm',
  ]
  platform_deps = []
else
  error('Only Windows, Linux and macOS are supported currently')
endif

if get_option('buildtype') == 'debug'
  vst_cpp_args = ['-DDEVELOPMENT']
elif get_option('buildtype') == 'release'
  vst_cpp_args = ['-DRELEASE']
else
  vst_cpp_args = ['-DDEVELOPMENT']
endif

libbase_dep = cxx.find_library('libbase', required : true,
  dirs : [get_option('vst-libdir')])
libsdk_dep = cxx.find_library('libsdk', required : true,
  dirs : [get_option('vst-libdir')])

gstvst3 = library('gstvst3',
  ['plugin.cpp', 'gstvstaudioprocessor.cpp'] + vst_sources + vst_platform_sources,
  cpp_args : [
            '-I@0@'.format(vst_includedir),
            '-I@0@'.format(vst_pluginterfaces_includedir),
            '-DPACKAGE="gstreamer-vst3"',
            '-DGST_PACKAGE_NAME="gstreamer-vst3"',
            '-DGST_PACKAGE_ORIGIN="https://github.com/centricular/gstreamer-vst3"',
            '-DVERSION="@0@"'.format(meson.project_version())] + common_flags + vst_cpp_args,
  link_args : noseh_link_args,
  dependencies : [gstaudio_dep, gstbase_dep, gst_dep, libbase_dep, libsdk_dep] + platform_deps,
  install : true,
  install_dir : plugins_install_dir,
)

