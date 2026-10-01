if build_games
  if host_system == 'windows'
    shared_module(
      game_binary_name,
      game_sources,
      include_directories: game_include_dirs,
      link_with: game_idlib_library,
      cpp_args: game_common_cpp_args,
      c_args: game_common_c_args,
      cpp_pch: game_module_pch_headers,
      link_args: game_module_link_args,
      vs_module_defs: game_module_defs_file,
      name_prefix: '',
      install: true,
      install_dir: install_gamedir_dir,
    )
  elif is_switch
    # Switch homebrew cannot dlopen a game module: build it as a static library
    # that the engine executable links directly.
    game_static_library = static_library(
      game_binary_name,
      game_sources,
      include_directories: game_include_dirs,
      link_with: game_idlib_library,
      cpp_args: game_common_cpp_args,
      c_args: game_common_c_args,
      cpp_pch: game_module_pch_headers,
      install: false,
    )

    # One relocatable object exporting only GetGameAPI, so the game keeps the
    # private globals and idlib copy it has as a DLL on desktop platforms.
    switch_game_object = custom_target(
      'switch_game_object',
      input: [game_static_library, game_idlib_library],
      output: game_binary_name + '.o',
      command: [
        py,
        meson.project_source_root() / 'tools' / 'switch' / 'make_game_object.py',
        get_option('b_lto') ? ['--lto'] : [],
        '@OUTPUT@',
        find_program('objcopy', required: true),
        # the LTO prelink generates the game's code, with the same profile flags
        meson.get_compiler('cpp').cmd_array() + switch_pgo_args,
        '--',
        '@INPUT@',
      ],
    )
  else
    shared_module(
      game_binary_name,
      game_sources,
      include_directories: game_include_dirs,
      link_with: game_idlib_library,
      cpp_args: game_common_cpp_args,
      c_args: game_common_c_args,
      cpp_pch: game_module_pch_headers,
      link_args: game_module_link_args,
      name_prefix: '',
      install: true,
      install_dir: install_gamedir_dir,
    )
  endif
endif

install_subdir(
  'materials',
  install_dir: install_gamedir_dir,
)

install_subdir(
  'glprogs',
  install_dir: install_gamedir_dir,
)

install_subdir(
  'guis',
  install_dir: install_gamedir_dir,
  exclude_directories: ['assets'],
)

install_data(
  [
    'guis/assets/menu/background_left.tga',
    'guis/assets/menu/background_right.tga',
    'guis/assets/menu/background_top.tga',
    'guis/assets/menu/background_bottom.tga',
  ],
  install_dir: install_gamedir_dir / 'guis' / 'assets' / 'menu',
)

install_data(
  [
    'guis/assets/loading/loading.tga',
    'guis/assets/loading/roadhouse.tga',
    'guis/assets/loading/loading_left.tga',
    'guis/assets/loading/loading_right.tga',
    'guis/assets/loading/loading_top.tga',
    'guis/assets/loading/loading_bottom.tga',
    'guis/assets/loading/roadhouse_left.tga',
    'guis/assets/loading/roadhouse_right.tga',
    'guis/assets/loading/roadhouse_top.tga',
    'guis/assets/loading/roadhouse_bottom.tga',
  ],
  install_dir: install_gamedir_dir / 'guis' / 'assets' / 'loading',
)

install_subdir(
  'maps',
  install_dir: install_gamedir_dir,
)

install_subdir(
  'script',
  install_dir: install_gamedir_dir,
)

install_subdir(
  'strings',
  install_dir: install_gamedir_dir,
)
