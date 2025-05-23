gtk_sources = [
  'gstgtkbasesink.c',
  'gstgtksink.c',
  'gstgtkutils.c',
  'gstplugin.c',
  'gtkgstbasewidget.c',
  'gtkgstwidget.c',
]

gtk_headers = [
  'gtkgstglwidget.h',
  'gstgtksink.h',
  'gtkgstbasewidget.h',
  'gtkgstwidget.h',
  'gstgtkglsink.h',
  'gstgtkutils.h',
  'gstgtkbasesink.h',
]

glwindowing_sources = [
  'gstgtkglsink.c',
  'gtkgstglwidget.c',
]

doc_sources = []
foreach s: gtk_sources + glwindowing_sources + gtk_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'gtk': pathsep.join(doc_sources)
}

gtk_defines = []
optional_deps = []

gtk_dep = dependency('gtk+-3.0', required : get_option('gtk3'))
if gtk_dep.found()
  # FIXME: automagic
  if have_gstgl and gtk_dep.version().version_compare('>=3.15.0')
    have_gtk3_gl_windowing = false

    if gst_gl_have_window_x11 and gst_gl_have_platform_glx
      # FIXME: automagic
      gtk_x11_dep = dependency('gtk+-x11-3.0', required : false)
      if gtk_x11_dep.found()
        optional_deps += [gtk_x11_dep, gstglx11_dep]
        have_gtk3_gl_windowing = true
      endif
    endif

    if gst_gl_have_window_wayland and gst_gl_have_platform_egl
      # FIXME: automagic
      gtk_wayland_dep = dependency('gtk+-wayland-3.0', required : false)
      if gtk_wayland_dep.found()
        optional_deps += [gtk_wayland_dep, gstglegl_dep, gstglwayland_dep]
        have_gtk3_gl_windowing = true
      endif
    endif

    if gst_gl_have_window_win32 and gst_gl_have_platform_wgl
      # FIXME: automagic
      gtk_win32_dep = dependency('gtk+-win32-3.0', required : false)
      if gtk_win32_dep.found() and gtk_win32_dep.version().version_compare('>=3.24.43')
        optional_deps += [gtk_win32_dep]
        have_gtk3_gl_windowing = true
      endif
    endif

    if have_gtk3_gl_windowing
      gtk_sources += glwindowing_sources
      optional_deps += [gstgl_dep, gstglproto_dep]
      gtk_defines += ['-DGST_USE_UNSTABLE_API', '-DHAVE_GTK3_GL']
    endif
  endif

  gstgtk = library('gstgtk',
    gtk_sources,
    c_args : gst_plugins_good_args + gtk_defines,
    link_args : noseh_link_args,
    include_directories : [configinc],
    dependencies : [gtk_dep, gstvideo_dep, gstbase_dep, libm] + optional_deps,
    install : true,
    install_dir : plugins_install_dir,
  )
  plugins += [gstgtk]
endif
