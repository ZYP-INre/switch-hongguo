include_directories(include)
include_directories(lib)
if (__SWITCH__ OR VITASDK)
    include_directories(include/custom)
endif ()

set(
  LIBMARISA_SOURCES
  lib/marisa/trie.cc
  lib/marisa/agent.cc
  lib/marisa/grimoire/io/reader.cc
  lib/marisa/grimoire/io/writer.cc
  lib/marisa/grimoire/io/mapper.cc
  lib/marisa/grimoire/trie/louds-trie.cc
  lib/marisa/grimoire/trie/tail.cc
  lib/marisa/grimoire/vector/bit-vector.cc
  lib/marisa/keyset.cc
)

add_library(marisa STATIC ${LIBMARISA_SOURCES})
if (VITASDK)
    set_target_properties(marisa PROPERTIES POSITION_INDEPENDENT_CODE OFF)
else ()
    set_target_properties(marisa PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif ()
source_group(marisa FILES ${LIBMARISA_SOURCES})
