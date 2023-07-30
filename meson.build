project(
	'aylp_alsa',
	'c',
	default_options: ['c_std=gnu17', 'warning_level=3', 'optimization=3']
)

incdir = include_directories(['libaylp'])

alsa_dep = dependency('alsa')
gsl_dep = dependency('gsl')
json_dep = dependency('json-c')
deps = [alsa_dep, gsl_dep, json_dep]

shared_library('aylp_alsa', 'aylp_alsa.c',
	name_prefix: '',
	install: true,
	dependencies: deps,
	include_directories: incdir,
	override_options: 'b_lundef=false'
)

