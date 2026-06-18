execute_deps = [libmist_dep]

if have_ndi
  execute_deps += [ndi_lib_dep, ndi_sdk_dep]
endif

if have_libxml2
  execute_deps += [onvif_lib_dep, libxml2]
endif

executables += {
  'name': 'MistController', 
  'sources' : [
    files( 'controller.cpp',
           'controller_external_writers.cpp',
           'controller_updater.cpp',
           'controller_streams.cpp',
           'controller_storage.cpp',
           'controller_connectors.cpp',
           'controller_statistics.cpp',
           'controller_capabilities.cpp',
           'controller_uplink.cpp',
           'controller_api.cpp',
           'controller_push.cpp',
           'controller_variables.cpp',
           'controller_discovery_utils.cpp',
           'controller_discovery.cpp'),
    header_tgts,
    variant_vfs_targets],
  'defines': [],
  'deps' : execute_deps
}
