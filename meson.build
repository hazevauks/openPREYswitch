project(
  'OpenPrey',
  ['c', 'cpp'],
  version: '0.0.1',
  meson_version: '>=1.2.0',
  default_options: ['c_std=c11', 'cpp_std=c++20', 'warning_level=0', 'b_vscrt=static_from_buildtype'],
)

host_system = host_machine.system()
host_cpu_family = host_machine.cpu_family()
if not ['windows', 'linux', 'darwin', 'horizon'].contains(host_system)
  error('Unsupported host system for OpenPrey Meson build: ' + host_system)
endif

# Nintendo Switch (devkitPro libnx). Cross-compile only; see tools/switch/meson/switch-cross.ini.
# The platform bring-up is in progress: the game code is linked statically into the
# engine because Switch homebrew cannot load a separate game module at runtime.
is_switch = host_system == 'horizon'

binary_arch = host_cpu_family
if host_cpu_family == 'x86_64'
  binary_arch = 'x64'
elif host_cpu_family == 'x86'
  binary_arch = 'x86'
elif host_cpu_family == 'aarch64'
  binary_arch = 'arm64'
endif
game_binary_name = 'game_' + binary_arch
engine_client_binary_name = meson.project_name() + '-client_' + binary_arch
engine_ded_binary_name = meson.project_name() + '-ded_' + binary_arch

if host_cpu_family != 'x86_64'
  warning(
    'OpenPrey currently targets x64 as the active compatibility baseline. '
    + 'Non-x64 builds are considered experimental during platform bring-up.'
  )
endif

platform_backend_requested = get_option('platform_backend')
platform_backend = platform_backend_requested
if host_system == 'windows' and platform_backend_requested == 'native'
  error('platform_backend=native is only valid on non-Windows hosts.')
endif

if host_system != 'windows'
  if platform_backend_requested == 'legacy_win32'
    error('platform_backend=legacy_win32 is only valid on Windows hosts.')
  endif

  if platform_backend_requested == 'sdl3'
    message(
      'platform_backend=sdl3 on non-Windows currently runs in staging mode and maps to native platform sources. '
      + 'Use this to keep command lines/CI aligned while SDL3 migration is in progress.'
    )
  endif
  platform_backend = 'native'
endif

use_sdl3_backend = host_system == 'windows' and platform_backend_requested == 'sdl3'
if host_system == 'windows' and not use_sdl3_backend
  warning(
    'legacy_win32 backend is transitional. '
    + 'Use -Dplatform_backend=sdl3 for portability-focused development.'
  )
endif

python_mod = import('python')
fs_mod = import('fs')
py = python_mod.find_installation('python3', required: true)
if host_system == 'darwin'
  add_languages('objc', 'objcpp', required: true)
endif

build_engine = get_option('build_engine')
build_games = get_option('build_games')
legacy_build_game_sp = get_option('build_game_sp')
legacy_build_game_mp = get_option('build_game_mp')
install_root_dir = meson.project_source_root() / '.install'
install_gamedir_dir = install_root_dir / 'basepr'
game_module_defs_file = meson.project_source_root() / 'src' / 'game' / 'Game.def'

if legacy_build_game_sp or legacy_build_game_mp
  warning(
    'build_game_sp/build_game_mp are legacy no-op options. '
    + 'OpenPrey now builds a unified game module (game_<arch>) when build_games=true.'
  )
endif

if not build_engine and not build_games
  error('At least one build target must be enabled. Use -Dbuild_engine=true and/or -Dbuild_games=true.')
endif

engine_source_discovery = run_command(
  py,
  meson.project_source_root() / 'tools' / 'build' / 'meson_sources.py',
  '--host-system',
  host_system,
  '--platform-backend',
  platform_backend,
  '--include-game',
  'false',
  check: true,
)

engine_source_paths = engine_source_discovery.stdout().strip().split('\n')
if engine_source_paths.length() == 0
  error('tools/build/meson_sources.py returned no engine source files.')
endif

engine_sources = files(engine_source_paths)

game_sources = []
game_idlib_sources = []
if build_games
  game_source_discovery = run_command(
    py,
    meson.project_source_root() / 'tools' / 'build' / 'list_sources.py',
    meson.project_source_root(),
    'src/game',
    'src/game/Callbacks.cpp',
    'src/game/gamesys/Callbacks.cpp',
    check: true,
  )
  game_source_paths = game_source_discovery.stdout().strip().split('\n')
  if game_source_paths.length() == 0
    error('tools/build/list_sources.py returned no game source files.')
  endif

  prey_source_discovery = run_command(
    py,
    meson.project_source_root() / 'tools' / 'build' / 'list_sources.py',
    meson.project_source_root(),
    'src/Prey',
    'src/Prey/ai_Navigator.cpp',
    'src/Prey/game_events.cpp',
    'src/Prey/particles_particles.cpp',
    'src/Prey/prey_weapon.cpp',
    check: true,
  )
  prey_source_paths = prey_source_discovery.stdout().strip().split('\n')
  if prey_source_paths.length() == 0
    error('tools/build/list_sources.py returned no Prey source files.')
  endif

  game_sources = files(game_source_paths + prey_source_paths)

  game_idlib_discovery = run_command(
    py,
    meson.project_source_root() / 'tools' / 'build' / 'list_sources.py',
    meson.project_source_root(),
    'src/idlib',
    'src/idlib/math/Simd_AltiVec.cpp',
    check: true,
  )
  game_idlib_paths = game_idlib_discovery.stdout().strip().split('\n')
  if game_idlib_paths.length() == 0
    error('tools/build/list_sources.py returned no game idlib source files.')
  endif
  game_idlib_sources = files(game_idlib_paths)
endif

root_include_dir = include_directories('.')
resource_compiler = find_program('rc', 'windres', required: false)
if host_system == 'windows' and resource_compiler.found()
  windows_mod = import('windows')
  engine_sources += windows_mod.compile_resources(
    'src/sys/win32/rc/OpenPreyVersion.rc',
    include_directories: [root_include_dir],
  )
endif

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

is_msvc = cc.get_id() == 'msvc' and cpp.get_id() == 'msvc'
msvc_2026_min = '19.46.0'
if is_msvc
  if cpp.version().version_compare('<' + msvc_2026_min)
    msvc_2026_msg = 'MSVC 19.46+ (Visual Studio 2026 toolset baseline) is recommended. Detected: ' + cpp.version() + '.'
    if get_option('enforce_msvc_2026')
      error(msvc_2026_msg)
    else
      warning(msvc_2026_msg + ' Configure with -Denforce_msvc_2026=true to enforce this baseline.')
    endif
  endif
elif get_option('enforce_msvc_2026')
  warning('enforce_msvc_2026 is only meaningful when using the MSVC toolchain.')
endif

fallback_opt = ['default_library=static']

glew_dep = disabler()
stb_vorbis_dep = disabler()
openal_dep = disabler()
sdl3_dep = disabler()
deps = []
if build_engine
  if host_system == 'windows'
    glew_dep = dependency(
      'glew',
      required: true,
      fallback: ['glew', 'glew_dep'],
      default_options: fallback_opt,
    )
  elif is_switch
    # No GLEW on Switch: GL entry points are resolved through eglGetProcAddress.
    glew_dep = dependency('', required: false)
  else
    glew_dep = dependency('glew', required: true)
  endif

  stb_vorbis_dep = dependency(
    'stb_vorbis',
    required: true,
    fallback: ['stb_vorbis', 'stb_vorbis_dep'],
    default_options: fallback_opt,
  )

  if host_system == 'darwin'
    openal_dep = dependency('appleframeworks', modules: ['OpenAL'], required: true)
  elif is_switch
    # devkitPro switch-openal-soft package (static libopenal.a).
    openal_dep = cc.find_library('openal', required: true)
  else
    openal_dep = dependency(
      'openal',
      required: true,
      fallback: ['openal-soft-prebuilt', 'openal_dep'],
      default_options: fallback_opt,
    )
  endif

  if use_sdl3_backend
    sdl3_dep = dependency(
      'sdl3',
      version: '>=3.4.0',
      required: true,
      fallback: ['sdl3', 'sdl3_dep'],
      default_options: fallback_opt,
    )
  endif

  deps = [glew_dep, openal_dep, stb_vorbis_dep]

  if host_system == 'windows'
    deps += [
      cc.find_library('opengl32', required: true),
      cc.find_library('Dbghelp', required: true),
      cc.find_library('Iphlpapi', required: true),
      cc.find_library('Winmm', required: true),
      cc.find_library('ws2_32', required: true),
    ]

    if use_sdl3_backend
      deps += [sdl3_dep]
    else
      deps += [
        cc.find_library('dinput8', required: true),
        cc.find_library('dxguid', required: true),
      ]
    endif
  elif host_system == 'linux'
    deps += [
      dependency('threads', required: true),
      cc.find_library('dl', required: true),
      dependency('x11', required: true),
      dependency('xext', required: true),
      dependency('xxf86vm', required: true),
      cc.find_library('GL', required: true),
    ]
  elif host_system == 'darwin'
    deps += [
      dependency('threads', required: true),
      dependency(
        'appleframeworks',
        modules: ['Cocoa', 'OpenGL', 'ApplicationServices', 'Carbon'],
        required: true,
      ),
    ]
  elif is_switch
    # devkitPro portlibs: SDL2 for window/input, Mesa (EGL + glapi + nouveau) for OpenGL.
    # Mesa is C++ and needs libstdc++/libm even when linked from C.
    switch_mesa_sdk = get_option('switch_mesa_sdk')
    if switch_mesa_sdk == ''
      switch_gl_deps = [
        cc.find_library('EGL', required: true),
        cc.find_library('glapi', required: true),
        cc.find_library('drm_nouveau', required: true),
      ]
    else
      # Alternative Mesa build: its headers and archives take precedence over portlibs.
      # - danfromtico/mesa-switch (Mesa 26): own Horizon GPU backend, needs expat, no libdrm_nouveau;
      # - devkitPro switch-mesa 20.1 rebuilt from source (tools/switch/mesa20): needs libdrm_nouveau.
      # Both are optional here; an archive nothing references adds nothing to the link.
      switch_mesa_lib_dir = switch_mesa_sdk / 'lib'
      switch_gl_deps = [
        declare_dependency(compile_args: ['-I' + (switch_mesa_sdk / 'include')]),
        cc.find_library('EGL', dirs: [switch_mesa_lib_dir], required: true),
        cc.find_library('glapi', dirs: [switch_mesa_lib_dir], required: true),
        cc.find_library('expat', dirs: [switch_mesa_lib_dir], required: false),
        cc.find_library('drm_nouveau', required: false),
        cc.find_library('z', required: true),
      ]
      message('Switch: using the Mesa SDK at ' + switch_mesa_sdk)
    endif
    deps += [dependency('threads', required: true), cc.find_library('SDL2', required: true)]
    deps += switch_gl_deps
    deps += [
      cc.find_library('stdc++', required: true),
      cc.find_library('m', required: true),
      cc.find_library('nx', required: true),
    ]
  endif
endif

shared_cpp_args = ['-DUSE_OPENAL', '-DGLEW_NO_GLU', '-DHUMANHEAD=1']
shared_c_args = ['-DGLEW_NO_GLU', '-DHUMANHEAD=1']
engine_link_args = []

if host_system == 'windows'
  shared_cpp_args += ['-D_WINDOWS', '-DWIN32']
  shared_c_args += ['-D_WINDOWS', '-DWIN32']
  engine_link_args += ['/STACK:16777216,16777216']
elif is_switch
  # No libGL to hard-link against: qgl* pointers are loaded at runtime through EGL.
  shared_cpp_args += ['-DOPENPREY_SWITCH=1']
  shared_c_args += ['-DOPENPREY_SWITCH=1']
else
  shared_cpp_args += ['-DID_GL_HARDLINK']
  shared_c_args += ['-DID_GL_HARDLINK']
endif

if host_system == 'darwin'
  shared_cpp_args += ['-DMACOS_X=1']
  shared_c_args += ['-DMACOS_X=1']
endif

if use_sdl3_backend
  shared_cpp_args += ['-DUSE_SDL3=1']
  shared_c_args += ['-DUSE_SDL3=1']
endif

if cpp.get_argument_syntax() == 'msvc'
  shared_cpp_args += ['/J', '/FS', '/source-charset:windows-1252', '/execution-charset:windows-1252', '/Zc:strictStrings-', '/W0']
  shared_c_args += ['/FS', '/source-charset:windows-1252', '/execution-charset:windows-1252', '/W0']
else
  shared_cpp_args += ['-fpermissive']
endif

pch_headers = []
if get_option('use_pch')
  pch_headers = ['src/idlib/precompiled.h']
endif
game_idlib_pch_headers = ['src/idlib/precompiled.h']
game_module_pch_headers = ['../src/idlib/precompiled.h']

src_include_dir = include_directories('src')
engine_include_dirs = [root_include_dir, src_include_dir]
game_include_dirs = [root_include_dir, src_include_dir]

engine_cpp_args = shared_cpp_args + ['-D__DOOM_DLL__']
engine_c_args = shared_c_args

if build_engine
  if host_system == 'windows'
    executable(
      engine_client_binary_name,
      engine_sources,
      include_directories: engine_include_dirs,
      dependencies: deps,
      cpp_args: engine_cpp_args,
      c_args: engine_c_args,
      cpp_pch: pch_headers,
      win_subsystem: 'windows',
      link_args: engine_link_args,
      install: true,
      install_dir: install_root_dir,
    )
  elif host_system == 'darwin'
    executable(
      engine_client_binary_name,
      engine_sources,
      include_directories: engine_include_dirs,
      dependencies: deps,
      cpp_args: engine_cpp_args,
      c_args: engine_c_args,
      objcpp_args: engine_cpp_args,
      objc_args: engine_c_args,
      cpp_pch: pch_headers,
      link_args: engine_link_args,
      install: true,
      install_dir: install_root_dir,
    )
  elif is_switch
    # Built after subdir('basepy'), which produces the game object it links.
  else
    executable(
      engine_client_binary_name,
      engine_sources,
      include_directories: engine_include_dirs,
      dependencies: deps,
      cpp_args: engine_cpp_args,
      c_args: engine_c_args,
      cpp_pch: pch_headers,
      link_args: engine_link_args,
      install: true,
      install_dir: install_root_dir,
    )
  endif

  dedicated_cpp_args = engine_cpp_args + ['-DID_DEDICATED']
  dedicated_c_args = engine_c_args + ['-DID_DEDICATED']

  if host_system == 'windows'
    executable(
      engine_ded_binary_name,
      engine_sources,
      include_directories: engine_include_dirs,
      dependencies: deps,
      cpp_args: dedicated_cpp_args,
      c_args: dedicated_c_args,
      cpp_pch: pch_headers,
      win_subsystem: 'windows',
      link_args: engine_link_args,
      install: true,
      install_dir: install_root_dir,
    )
  elif host_system == 'darwin'
    executable(
      engine_ded_binary_name,
      engine_sources,
      include_directories: engine_include_dirs,
      dependencies: deps,
      cpp_args: dedicated_cpp_args,
      c_args: dedicated_c_args,
      objcpp_args: dedicated_cpp_args,
      objc_args: dedicated_c_args,
      cpp_pch: pch_headers,
      link_args: engine_link_args,
      install: true,
      install_dir: install_root_dir,
    )
  elif is_switch
    # No dedicated server on Switch.
  else
    executable(
      engine_ded_binary_name,
      engine_sources,
      include_directories: engine_include_dirs,
      dependencies: deps,
      cpp_args: dedicated_cpp_args,
      c_args: dedicated_c_args,
      cpp_pch: pch_headers,
      link_args: engine_link_args,
      install: true,
      install_dir: install_root_dir,
    )
  endif

  if host_system == 'windows' and host_cpu_family == 'x86_64'
    openal_runtime_candidates = [
      meson.project_source_root() / 'src' / 'external' / 'openal-soft' / 'bin' / 'win64' / 'OpenAL32.dll',
      meson.project_source_root() / 'subprojects' / 'openal-soft-prebuilt' / 'bin' / 'win64' / 'OpenAL32.dll',
    ]
    openal_runtime_staged = false
    foreach openal_runtime_dll : openal_runtime_candidates
      if fs_mod.is_file(openal_runtime_dll)
        install_data(
          openal_runtime_dll,
          install_dir: install_root_dir,
        )
        openal_runtime_staged = true
        break
      endif
    endforeach
    if not openal_runtime_staged
      warning(
        'OpenAL Soft runtime DLL was not found in expected locations; '
        + 'runtime audio will fall back to the system OpenAL32.dll.'
      )
    endif
  endif
endif

game_common_cpp_args = shared_cpp_args + ['-DGAME_DLL']
game_common_c_args = shared_c_args
game_module_link_args = []
if host_system == 'windows'
  game_module_link_args = ['/FIXED:NO', '/LARGEADDRESSAWARE']
  if host_cpu_family == 'x86_64'
    game_module_link_args += ['/BASE:0x180000000']
  else
    game_module_link_args += ['/BASE:0x20000000']
  endif
  release_like = ['release', 'debugoptimized', 'minsize'].contains(get_option('buildtype'))
  if release_like
    game_module_link_args += ['/STACK:4194304', '/OPT:REF', '/OPT:ICF', '/MAP']
  else
    game_module_link_args += ['/STACK:16000000,16000000']
  endif
endif

game_idlib_library = disabler()
if build_games
  game_idlib_library = static_library(
    'openprey_game_idlib',
    game_idlib_sources,
    include_directories: game_include_dirs,
    cpp_args: game_common_cpp_args,
    c_args: game_common_c_args,
    cpp_pch: game_idlib_pch_headers,
    install: false,
  )
endif

if build_engine or build_games
  subdir('basepy')
endif

if is_switch and build_engine
  if not build_games
    error('The Switch engine links the game statically: configure with -Dbuild_games=true.')
  endif

  # devkitPro Mesa has no libGL: generate the GL 1.1 entry points, resolved
  # through eglGetProcAddress at GLimp_Init.
  switch_gl11_loader = custom_target(
    'switch_gl11_loader',
    input: ['tools/switch/gen_gl11_loader.py', 'src/external/glew/glew.h'],
    output: 'switch_gl11_loader.cpp',
    command: [py, '@INPUT0@', '@INPUT1@', '@OUTPUT@'],
  )

  switch_engine_elf = executable(
    engine_client_binary_name,
    engine_sources + [switch_gl11_loader, switch_game_object],
    include_directories: engine_include_dirs + [include_directories('src/sys/switch')],
    dependencies: deps,
    cpp_args: engine_cpp_args,
    c_args: engine_c_args,
    cpp_pch: pch_headers,
    # --wrap=pthread_create: every thread, library ones included, starts through
    # src/sys/switch/switch_threads.cpp, which places it on a CPU core.
    link_args: engine_link_args + ['-Wl,-Map,' + engine_client_binary_name + '.map', '-Wl,--wrap=pthread_create'],
    name_suffix: 'elf',
    install: false,
  )

  # Homebrew package: metadata (.nacp) + executable -> .nro
  dkp_tools_dir = '/opt/devkitpro/tools/bin'
  nacptool = find_program('nacptool', dirs: [dkp_tools_dir], required: true)
  elf2nro = find_program('elf2nro', dirs: [dkp_tools_dir], required: true)

  # Builds against an alternative Mesa SDK get their own name and title, so they
  # can sit on the SD card side by side (switch_variant names them; default mesa-sdk).
  switch_variant = get_option('switch_variant')
  if switch_variant == '' and get_option('switch_mesa_sdk') != ''
    switch_variant = 'mesa-sdk'
  endif
  switch_nro_name = switch_variant == '' ? 'OpenPrey' : 'OpenPrey-' + switch_variant
  switch_app_title = switch_variant == '' ? 'OpenPrey' : 'OpenPrey (' + switch_variant + ')'

  switch_nacp = custom_target(
    'switch_nacp',
    output: switch_nro_name + '.nacp',
    command: [nacptool, '--create', switch_app_title, 'OpenPrey Switch port', meson.project_version(), '@OUTPUT@'],
  )

  custom_target(
    'switch_nro',
    input: switch_engine_elf,
    output: switch_nro_name + '.nro',
    command: [
      elf2nro, '@INPUT@', '@OUTPUT@',
      '--nacp=' + switch_nacp.full_path(),
      '--icon=' + '/opt/devkitpro/libnx/default_icon.jpg',
    ],
    depends: switch_nacp,
    build_by_default: true,
  )
endif

install_data(
  'assets/icons/prey.ico',
  install_dir: install_root_dir,
  rename: 'OpenPrey.ico',
)

install_data(
  'assets/icons/prey.icns',
  install_dir: install_root_dir,
  rename: 'OpenPrey.icns',
)

if host_system == 'linux'
  linux_desktop_conf = configuration_data()
  linux_desktop_conf.set('OPENPREY_CLIENT_BINARY', engine_client_binary_name)
  linux_desktop_conf.set('OPENPREY_PROJECT_VERSION', meson.project_version())
  linux_desktop_conf.set('OPENPREY_PROJECT_VERSION_FULL', meson.project_version())
  linux_desktop_entry = configure_file(
    input: 'assets/linux/openprey.desktop.in',
    output: 'openprey.desktop',
    configuration: linux_desktop_conf,
  )
  install_data(
    linux_desktop_entry,
    install_dir: install_root_dir / 'share' / 'applications',
  )

  if fs_mod.is_file('assets/icons/prey_256.svg')
    install_data(
      'assets/icons/prey_256.svg',
      install_dir: install_root_dir / 'share' / 'icons' / 'hicolor' / 'scalable' / 'apps',
      rename: 'openprey.svg',
    )
  endif

  icon_sizes = ['16', '24', '32', '48', '64', '128', '256']
  foreach icon_size : icon_sizes
    icon_png = 'assets/icons/prey_' + icon_size + '.png'
    if fs_mod.is_file(icon_png)
      install_data(
        icon_png,
        install_dir: install_root_dir / 'share' / 'icons' / 'hicolor' / (icon_size + 'x' + icon_size) / 'apps',
        rename: 'openprey.png',
      )
    endif
  endforeach
endif

summary(
  {
    'Host system': host_system,
    'Host cpu family': host_cpu_family,
    'MSVC toolchain': is_msvc,
    'MSVC version': cpp.version(),
    'MSVC 2026 baseline enforced': get_option('enforce_msvc_2026'),
    'C++ standard': get_option('cpp_std'),
    'Platform backend requested': platform_backend_requested,
    'Platform backend': platform_backend,
    'Build engine': build_engine,
    'engine client binary': engine_client_binary_name,
    'engine dedicated binary': engine_ded_binary_name,
    'Build games': build_games,
    'Unified game binary': build_games ? game_binary_name : '',
    'Resource compiler': resource_compiler.found(),
    'SDL3': use_sdl3_backend and sdl3_dep.found(),
    'GLEW': glew_dep.found(),
    'OpenAL': openal_dep.found(),
    'stb_vorbis': stb_vorbis_dep.found(),
    'Precompiled headers': get_option('use_pch'),
    'Dedicated server target': build_engine,
    'Install root': install_root_dir,
  },
  section: 'Build Features',
  bool_yn: true,
)
