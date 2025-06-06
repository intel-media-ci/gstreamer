dwrite_sources = [
  'gstdwrite-effect.cpp',
  'gstdwrite-enums.cpp',
  'gstdwrite-renderer.cpp',
  'gstdwrite-utils.cpp',
  'gstdwritebaseoverlay.cpp',
  'gstdwritebitmapmemory.cpp',
  'gstdwritebitmappool.cpp',
  'gstdwriteclockoverlay.cpp',
  'gstdwriteoverlayobject.cpp',
  'gstdwriterender_bitmap.cpp',
  'gstdwriterender_d3d11.cpp',
  'gstdwriterender.cpp',
  'gstdwritesubtitlemux.cpp',
  'gstdwritesubtitleoverlay.cpp',
  'gstdwritetextoverlay.cpp',
  'gstdwritetimeoverlay.cpp',
  'plugin.cpp',
]

dwrite_headers = [
  'gstdwritetimeoverlay.h',
  'gstdwriterender_d3d11.h',
  'gstdwritebaseoverlay.h',
  'gstdwritebitmapmemory.h',
  'gstdwriterender_d3d12.h',
  'gstdwritebitmappool.h',
  'gstdwriteoverlayobject.h',
  'gstdwriterender_bitmap.h',
  'gstdwriterender.h',
  'gstdwrite-utils.h',
  'gstdwrite-effect.h',
  'gstdwritetextoverlay.h',
  'gstdwrite-renderer.h',
  'gstdwrite-enums.h',
  'gstdwritesubtitlemux.h',
  'gstdwriteclockoverlay.h',
  'gstdwritesubtitleoverlay.h',
]

d3d12_sources = [
  'gstdwriterender_d3d12.cpp'
]

doc_sources = []
foreach s: dwrite_sources + d3d12_sources + dwrite_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'dwrite': pathsep.join(doc_sources)
}

extra_args = ['-DGST_USE_UNSTABLE_API']
extra_deps = []

dwrite_option = get_option('dwrite')
if host_system != 'windows' or dwrite_option.disabled()
  subdir_done()
endif

if not gstd3d11_dep.found()
  if dwrite_option.enabled()
    error('The dwrite was enabled explicitly, but required dependencies were not found.')
  endif
  subdir_done()
endif

d2d_dep = cc.find_library('d2d1', required: dwrite_option)
dwrite_lib = cc.find_library('dwrite', required : dwrite_option)
windowscodecs_lib = cc.find_library('windowscodecs', required : dwrite_option)
have_d2d_h = cc.has_header('d2d1_1.h')
have_dwrite_h = cc.has_header('dwrite.h')
have_wincodec_h = cc.has_header('wincodec.h')

if not have_d2d_h or not have_dwrite_h or not have_wincodec_h
  if dwrite_option.enabled()
    error('The dwrite was enabled explicitly, but required dependencies were not found.')
  endif
  subdir_done ()
endif

if cc.has_header('d2d1_3.h') and cc.has_header('dwrite_3.h')
  # DWRITE_GLYPH_IMAGE_FORMATS enum requires NTDDI_WIN10_RS1
  extra_args += ['-U_WIN32_WINNT',
                 '-UWINVER',
                 '-DWINVER=0x0A00',
                 '-D_WIN32_WINNT=0x0A00',
                 '-DNTDDI_VERSION=0x0A000002',
                 '-DHAVE_DWRITE_COLOR_FONT']
endif

# MinGW 32bits compiler seems to be complaining about redundant-decls
# when ComPtr is in use. Let's just disable the warning
if cc.get_id() != 'msvc'
  extra_mingw_args = cc.get_supported_arguments([
    '-Wno-redundant-decls',
  ])

  extra_args += extra_mingw_args
endif

subdir('libcaption')

if gstd3d12_dep.found() and cc.has_header('d3d11on12.h') and cc.has_header('d3d12video.h')
  extra_args += ['-DHAVE_GST_D3D12']
  extra_deps += [gstd3d12_dep]
  dwrite_sources += d3d12_sources
endif

gstdwrite = library('gstdwrite',
  dwrite_sources,
  c_args : gst_plugins_bad_args + extra_args,
  cpp_args: gst_plugins_bad_args + extra_args,
  include_directories : [configinc],
  dependencies : [gstbase_dep, gstvideo_dep, gstd3d11_dep, d2d_dep, dwrite_lib,
                  dwrite_libcaption_dep] + extra_deps,
  install : true,
  install_dir : plugins_install_dir,
)
plugins += [gstdwrite]
