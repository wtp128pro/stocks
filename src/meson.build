fs = import('fs')

# Each module directory exports a `<module>_dep` declare_dependency. Modules are
# listed in dependency order (a module may only use deps of earlier ones) and
# are skipped while their directory does not exist yet.
foreach module : ['core', 'provider', 'indices', 'storage', 'catalog', 'chart', 'picker', 'ui']
  if fs.is_dir(module)
    subdir(module)
  endif
endforeach
