# The loaders' corpus

Hostile files the reviews and the fuzzers found, each one that mustn't decode, or must fail in
time: tests/assets/models_tests.cpp and images_tests.cpp name what each must fail with, and
fuzz_tests.cpp breaks each of them further. A new find goes here, with a test that names it.

- `zstd_hang.ktx2`: a KTX2 whose data format descriptor has a block of size 0, which libktx
  (v4.4.2) walks forever in ktxFormatSize_initFromDfd. The engine's check_dfd refuses it first.

Found under the sanitizers but not in the engine's code, so not patched:

- libktx v4.4.2's ETC1S encoder allocates its supercompression data with `new[]`
  (basis_encode.cpp, ktxTexture2_CompressBasisEx) and frees it with `free` (texture2.c,
  ktxTexture2_destruct). Only the tests' own ETC1S encodes reach it; the cooker encodes UASTC.
- stb_image_write, at stb's commit 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5, shifts into an int's
  sign in its JPEG writer (stbiw__jpg_writeBits), which UBSan reports. It's the tests' own writer of
  seed images, never a loader.
