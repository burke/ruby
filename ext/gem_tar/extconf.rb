require "mkmf"

have_library("z", "gzopen") || abort("zlib is required")
create_makefile("gem_tar")
