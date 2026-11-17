# Monocypher, for an encrypted pak: XChaCha20-Poly1305 seals each file and the index, and BLAKE2b
# makes a key's own keys and each file's nonce, from its bytes, so a file that doesn't change seals
# the same. It's one C file, built into fbengine_native and fbengine_tools_native statically.
FetchContent_Declare(
  monocypher
  GIT_REPOSITORY https://github.com/LoupVaillant/Monocypher.git
  GIT_TAG 4.0.3
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR none
  SYSTEM
)
FetchContent_MakeAvailable(monocypher)

add_library(fbengine_monocypher STATIC "${monocypher_SOURCE_DIR}/src/monocypher.c")
target_include_directories(fbengine_monocypher SYSTEM PUBLIC "${monocypher_SOURCE_DIR}/src")
set_target_properties(fbengine_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)
