nvcodec_sources = [
  'gstcudabasetransform.c',
  'gstcudacompositor.cpp',
  'gstcudaconverter.cpp',
  'gstcudaconvertscale.c',
  'gstcudaipc.cpp',
  'gstcudaipcclient.cpp',
  'gstcudaipcserver.cpp',
  'gstcudaipcsink.cpp',
  'gstcudaipcsrc.cpp',
  'gstcudamemorycopy.c',
  'gstnvcodecutils.cpp',
  'plugin.c'
]

# Only docstring-containing headers need listing here
nvcodec_headers = [
  'gstcudaconverter.h'
]

nvcodec_dgpu_sources = [
  'gstcuvidloader.c',
  'gstnvav1dec.cpp',
  'gstnvav1encoder.cpp',
  'gstnvdec.c',
  'gstnvdecobject.cpp',
  'gstnvdecoder.cpp',
  'gstnvenc.c',
  'gstnvencobject.cpp',
  'gstnvencoder.cpp',
  'gstnvh264dec.cpp',
  'gstnvh264encoder.cpp',
  'gstnvh265dec.cpp',
  'gstnvh265encoder.cpp',
  'gstnvjpegenc.cpp',
  'gstnvvp8dec.cpp',
  'gstnvvp9dec.cpp'
]

nvcodec_win32_sources = [
  'gstcudaipcclient_win32.cpp',
  'gstcudaipcserver_win32.cpp',
]

nvcodec_unix_sources = [
  'gstcudaipcclient_unix.cpp',
  'gstcudaipcserver_unix.cpp',
]

nvcodec_d3d12_sources = [
  'gstcudainterop_d3d12.cpp',
]

doc_sources = []
foreach s: nvcodec_sources + nvcodec_dgpu_sources + nvcodec_win32_sources + nvcodec_unix_sources + nvcodec_d3d12_sources + nvcodec_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'nvcodec': pathsep.join(doc_sources)
}

if get_option('nvcodec').disabled()
  subdir_done()
endif

if not gstcuda_dep.found()
  if get_option('nvcodec').enabled()
    error('The nvcodec was enabled explicitly, but required gstcuda dependency is not found')
  endif
  subdir_done()
endif

plugin_incdirs = [configinc, cuda_stubinc]
extra_args = ['-DGST_USE_UNSTABLE_API']
extra_deps = []

nvcodec_kernel_precompiled = []
nvcodec_precompile_opt = get_option('nvcodec-cuda-precompile')
if not nvcodec_precompile_opt.disabled() and not meson.is_cross_build()
  nvcc = find_program ('nvcc', required : nvcodec_precompile_opt)
  if nvcc.found()
    subdir('kernel')
    extra_args += ['-DNVCODEC_CUDA_PRECOMPILED']
  endif
endif

if gstgl_dep.found()
  extra_args += ['-DHAVE_CUDA_GST_GL']
endif

if host_system == 'linux'
  if have_nvbufsurface_h
    extra_args += ['-DHAVE_CUDA_NVMM']
    plugin_incdirs += gstcuda_nvmm_inc
  endif

  gio_unix_dep = dependency('gio-unix-2.0', required : get_option('nvcodec'))
  extra_deps += [gio_dep, gio_unix_dep]

  nvcodec_sources += nvcodec_unix_sources
else
  nvcodec_sources += nvcodec_win32_sources
endif

# if the system is not a tegra based system we add the other sources for encoding and decoding
if not nvbuf_dep.found()
  extra_args += ['-DHAVE_NVCODEC_DGPU']
  nvcodec_sources += nvcodec_dgpu_sources
endif

if gstd3d12_dep.found() and cc.has_header('d3d12video.h')
  extra_args += ['-DHAVE_GST_D3D12']
  extra_deps += [gstd3d12_dep]
  nvcodec_sources += nvcodec_d3d12_sources
endif

if cc.get_id() != 'msvc'
  if host_system == 'windows'
    # MinGW 32bits compiler seems to be complaining about redundant-decls
    # when ComPtr is in use. Let's just disable the warning
    extra_args += cc.get_supported_arguments([
      '-Wno-redundant-decls',
    ])
  endif

  # Allow deprecated decls since it's part of SDK header
  extra_args += cc.get_supported_arguments([
    '-Wno-deprecated-declarations',
  ])
endif

gstnvcodec = library('gstnvcodec',
  nvcodec_sources + nvcodec_kernel_precompiled,
  c_args : gst_plugins_bad_args + extra_args,
  cpp_args : gst_plugins_bad_args + extra_args,
  include_directories : plugin_incdirs,
  dependencies : [gstbase_dep, gstvideo_dep, gstpbutils_dep, gstgl_dep,
                  gstglproto_dep, gmodule_dep, gstcodecs_dep,
                  gstd3d11_dep, gstcuda_dep] + extra_deps,
  override_options : ['cpp_std=c++14'],
  install : true,
  install_dir : plugins_install_dir,
)
plugins += [gstnvcodec]
