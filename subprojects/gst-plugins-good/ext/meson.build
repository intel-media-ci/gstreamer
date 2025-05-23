# We need libsoup for the soup plugin and for adaptivedemux2, and we can link
# to either libsoup-2.4 or libsoup-3.0. There's a few cases here:
#
# 1. Windows, where we do not support dlopen()
# 2. UNIX, and we build only a shared library
# 3. UNIX, and we build only a static library
# 4. UNIX, and we build both (static and shared)
#
# In cases 1,2,3: we look up the dependency
# In case 1: we create one library() target that always links to libsoup
# In cases 3,4: we create one static_library() target that links to libsoup
# In cases 2,4: we create one shared_library() target that dlopen()s libsoup

libsoup2_dep = dependency('', required: false)
libsoup3_dep = dependency('', required: false)
soup_ver_opt = get_option('soup-version')

default_library = get_option('default_library')
soup_linked_target = host_system == 'windows' or default_library != 'shared'
soup_lookup_dep = get_option('soup-lookup-dep') or soup_linked_target
soup_dlopen_target_deps = []
soup_dlopen_target_kwargs = {}

if get_option('soup').allowed() or get_option('adaptivedemux2').allowed()
  if soup_ver_opt in ['auto', '3']
    libsoup3_dep = dependency('libsoup-3.0', allow_fallback: true, include_type: 'system',
                              required: soup_ver_opt == '3' and soup_lookup_dep,
                              default_options: {'tests': false})
  endif
  if soup_ver_opt in ['auto', '2']
    libsoup2_dep = dependency('libsoup-2.4', version: '>=2.48', allow_fallback: true,
                              include_type: 'system', default_options: ['sysprof=disabled'],
                              required: soup_ver_opt == '2' and soup_lookup_dep)
  endif

  if soup_linked_target
    if libsoup3_dep.found()
      soup_linked_target_deps = [libsoup3_dep]
      soup_linked_target_args = ['-DLINK_SOUP=3']
      message('soup and adaptivedemux2 plugins: linking to libsoup-3.0')
    elif libsoup2_dep.found()
      soup_linked_target_deps = [libsoup2_dep]
      soup_linked_target_args = ['-DLINK_SOUP=2']
      message('soup and adaptivedemux2 plugins: linking to libsoup-2.4')
    endif
  endif

  # Hack to set the LC_RPATH for making dlopen() work, since meson will only
  # add that for deps that are in the list of dependencies. We also need to add
  # install_rpath to cover the installation case. Every other dep is loaded via
  # absolute LC_LOAD_DYLIB entries.
  if host_system == 'darwin'
    foreach dep : [libsoup3_dep, libsoup2_dep]
      if not dep.found()
        continue
      endif
      soup_dlopen_target_deps += [dep]
      if dep.type_name() == 'internal'
        soup_dlopen_target_kwargs += {'install_rpath': '@loader_path/..'}
      else
        soup_dlopen_target_kwargs += {'install_rpath': dep.get_variable('libdir')}
      endif
      break
    endforeach
  endif
endif

subdir('aalib')
subdir('adaptivedemux2')
subdir('amrnb')
subdir('amrwbdec')
subdir('cairo')
subdir('flac')
subdir('gdk_pixbuf')
subdir('gtk')
subdir('jack')
subdir('jpeg')
subdir('lame')
subdir('libcaca')
subdir('dv')
subdir('libpng')
subdir('mpg123')
subdir('raw1394')
subdir('qt')
subdir('qt6')
subdir('pulse')
subdir('shout2')
subdir('soup')
subdir('speex')
subdir('taglib')
subdir('twolame')
subdir('vpx')
subdir('wavpack')
