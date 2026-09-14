// Symbol prefix mapping for the zlib 1.3.2 test reference objects.
//
// This header is force-included (-include) only when compiling the reference
// C sources in this directory and the ZlibReference adapter that calls them.
// It renames every externally visible zlib identifier to fiber_test_zlib_* so
// the reference code can never satisfy a link dependency on the production
// zlib C API. It must not be included by any production target.
//
// Keep this list in sync with the sources; verify with:
//   nm --defined-only <objects> | grep -v fiber_test_zlib
// (only local (t/T-less lowercase) symbols may remain unprefixed).

#ifndef FIBER_TEST_ZLIB_PREFIX_H
#define FIBER_TEST_ZLIB_PREFIX_H

// deflate.c
#define deflate_copyright fiber_test_zlib_deflate_copyright
#define deflateInit_ fiber_test_zlib_deflateInit_
#define deflateInit2_ fiber_test_zlib_deflateInit2_
#define deflateReset fiber_test_zlib_deflateReset
#define deflateResetKeep fiber_test_zlib_deflateResetKeep
#define deflateSetDictionary fiber_test_zlib_deflateSetDictionary
#define deflateGetDictionary fiber_test_zlib_deflateGetDictionary
#define deflateSetHeader fiber_test_zlib_deflateSetHeader
#define deflatePending fiber_test_zlib_deflatePending
#define deflateUsed fiber_test_zlib_deflateUsed
#define deflatePrime fiber_test_zlib_deflatePrime
#define deflateParams fiber_test_zlib_deflateParams
#define deflateTune fiber_test_zlib_deflateTune
#define deflateBound_z fiber_test_zlib_deflateBound_z
#define deflateBound fiber_test_zlib_deflateBound
#define deflate fiber_test_zlib_deflate
#define deflateEnd fiber_test_zlib_deflateEnd
#define deflateCopy fiber_test_zlib_deflateCopy

// trees.c
#define _tr_init fiber_test_zlib__tr_init
#define _tr_tally fiber_test_zlib__tr_tally
#define _tr_flush_block fiber_test_zlib__tr_flush_block
#define _tr_flush_bits fiber_test_zlib__tr_flush_bits
#define _tr_align fiber_test_zlib__tr_align
#define _tr_stored_block fiber_test_zlib__tr_stored_block
#define _length_code fiber_test_zlib__length_code
#define _dist_code fiber_test_zlib__dist_code

// inflate.c
#define inflate_copyright fiber_test_zlib_inflate_copyright
#define inflateInit_ fiber_test_zlib_inflateInit_
#define inflateInit2_ fiber_test_zlib_inflateInit2_
#define inflateReset fiber_test_zlib_inflateReset
#define inflateReset2 fiber_test_zlib_inflateReset2
#define inflateResetKeep fiber_test_zlib_inflateResetKeep
#define inflatePrime fiber_test_zlib_inflatePrime
#define inflateGetHeader fiber_test_zlib_inflateGetHeader
#define inflateGetDictionary fiber_test_zlib_inflateGetDictionary
#define inflateSetDictionary fiber_test_zlib_inflateSetDictionary
#define inflateSync fiber_test_zlib_inflateSync
#define inflateSyncPoint fiber_test_zlib_inflateSyncPoint
#define inflateCopy fiber_test_zlib_inflateCopy
#define inflateUndermine fiber_test_zlib_inflateUndermine
#define inflateMark fiber_test_zlib_inflateMark
#define inflateValidate fiber_test_zlib_inflateValidate
#define inflateCodesUsed fiber_test_zlib_inflateCodesUsed
#define inflate fiber_test_zlib_inflate
#define inflateEnd fiber_test_zlib_inflateEnd

// inftrees.c / inffast.c
#define inflate_table fiber_test_zlib_inflate_table
#define inflate_fast fiber_test_zlib_inflate_fast

// crc32.c
#define get_crc_table fiber_test_zlib_get_crc_table
#define crc32_z fiber_test_zlib_crc32_z
#define crc32 fiber_test_zlib_crc32
#define crc32_combine_gen64 fiber_test_zlib_crc32_combine_gen64
#define crc32_combine_gen fiber_test_zlib_crc32_combine_gen
#define crc32_combine_op fiber_test_zlib_crc32_combine_op
#define crc32_combine64 fiber_test_zlib_crc32_combine64
#define crc32_combine fiber_test_zlib_crc32_combine

// adler32.c
#define adler32_z fiber_test_zlib_adler32_z
#define adler32 fiber_test_zlib_adler32
#define adler32_combine64 fiber_test_zlib_adler32_combine64
#define adler32_combine fiber_test_zlib_adler32_combine

// zutil.c
#define zlibVersion fiber_test_zlib_zlibVersion
#define zlibCompileFlags fiber_test_zlib_zlibCompileFlags
#define zError fiber_test_zlib_zError
#define z_error fiber_test_zlib_z_error
#define z_verbose fiber_test_zlib_z_verbose
#define zcalloc fiber_test_zlib_zcalloc
#define zcfree fiber_test_zlib_zcfree
#define zmemcpy fiber_test_zlib_zmemcpy
#define zmemcmp fiber_test_zlib_zmemcmp
#define zmemzero fiber_test_zlib_zmemzero

#endif // FIBER_TEST_ZLIB_PREFIX_H
