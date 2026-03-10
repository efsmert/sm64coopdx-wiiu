#include "dynos.cpp.h"
#include <zlib.h>

extern "C" {
#include "pc/mods/mod_fs.h"
}

static const u8 DYNOS_BIN_COMPRESS_MAGIC_BYTES[8] = { 'D', 'Y', 'N', 'O', 'S', 'B', 'I', 'N' };

static inline bool DynOS_Bin_MagicMatches(const void *p) {
    return p != NULL && memcmp(p, DYNOS_BIN_COMPRESS_MAGIC_BYTES, sizeof(DYNOS_BIN_COMPRESS_MAGIC_BYTES)) == 0;
}

static inline u64 DynOS_ReadU64LE(const u8 *p) {
    if (p == NULL) { return 0; }
    return ((u64)p[0]) |
           ((u64)p[1] << 8) |
           ((u64)p[2] << 16) |
           ((u64)p[3] << 24) |
           ((u64)p[4] << 32) |
           ((u64)p[5] << 40) |
           ((u64)p[6] << 48) |
           ((u64)p[7] << 56);
}

static inline void DynOS_WriteU64LE(u8 *p, u64 v) {
    if (p == NULL) { return; }
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
    p[4] = (u8)((v >> 32) & 0xFF);
    p[5] = (u8)((v >> 40) & 0xFF);
    p[6] = (u8)((v >> 48) & 0xFF);
    p[7] = (u8)((v >> 56) & 0xFF);
}
static FILE  *sFile = NULL;
static u8 *sBufferUncompressed = NULL;
static u8 *sBufferCompressed = NULL;
static u64 sLengthUncompressed = 0;
static u64 sLengthCompressed = 0;

static inline void DynOS_Bin_Compress_Init() {
    sFile = NULL;
    sBufferUncompressed = NULL;
    sBufferCompressed = NULL;
    sLengthUncompressed = 0;
    sLengthCompressed = 0;
}

static inline void DynOS_Bin_Compress_Close() {
    if (sFile) f_close(sFile);
    sFile = NULL;
}

static inline void DynOS_Bin_Compress_Free() {
    if (sBufferCompressed) free(sBufferCompressed);
    if (sBufferUncompressed) free(sBufferUncompressed);
    DynOS_Bin_Compress_Close();
}

static inline bool DynOS_Bin_Compress_Check(bool condition, const char *function, const char *filename, const char *message) {
    if (!condition) {
        PrintError("ERROR: %s: File \"%s\": %s", function, filename, message);
        DynOS_Bin_Compress_Free();
        return false;
    }
    return true;
}

bool DynOS_Bin_IsCompressed(const SysPath &aFilename) {
    DynOS_Bin_Compress_Init();

    // Open input file
    if (!DynOS_Bin_Compress_Check(
        (sFile = fopen(aFilename.c_str(), "rb")) != NULL,
        __FUNCTION__, aFilename.c_str(), "Cannot open file"
    )) return false;

    // Read magic bytes
    u8 magic[8] = { 0 };
    if (!DynOS_Bin_Compress_Check(
        fread(magic, sizeof(magic), 1, sFile) == 1,
        __FUNCTION__, aFilename.c_str(), "Cannot read magic"
    )) return false;

    if (!DynOS_Bin_MagicMatches(magic)) {
        DynOS_Bin_Compress_Free();
        return false;
    }

    // It is a compressed file
    DynOS_Bin_Compress_Free();
    return true;
}

bool DynOS_Bin_Compress(const SysPath &aFilename) {
    DynOS_Bin_Compress_Init();
    PrintNoNewLine("Compressing file \"%s\"...", aFilename.c_str());

    // Open input file
    if (!DynOS_Bin_Compress_Check(
        (sFile = fopen(aFilename.c_str(), "rb")) != NULL,
        __FUNCTION__, aFilename.c_str(), "Cannot open file"
    )) return false;

    // Retrieve file length
    if (!DynOS_Bin_Compress_Check(
        fseek(sFile, 0, SEEK_END) == 0,
        __FUNCTION__, aFilename.c_str(), "Cannot retrieve file length"
    )) return false;

    // Check file length
    if (!DynOS_Bin_Compress_Check(
        (sLengthUncompressed = (u64) ftell(sFile)) != 0,
        __FUNCTION__, aFilename.c_str(), "Empty file"
    )) return false;

    // Allocate memory for uncompressed buffer
    if (!DynOS_Bin_Compress_Check(
        (sBufferUncompressed = (u8 *) calloc(sLengthUncompressed, sizeof(u8))) != NULL,
        __FUNCTION__, aFilename.c_str(), "Cannot allocate memory for compression"
    )) return false; else rewind(sFile);

    // Read input data
    if (!DynOS_Bin_Compress_Check(
        fread(sBufferUncompressed, sizeof(u8), sLengthUncompressed, sFile) == sLengthUncompressed,
        __FUNCTION__, aFilename.c_str(), "Cannot read uncompressed data"
    )) return false; else DynOS_Bin_Compress_Close();

    // Compute maximum output file size
    if (!DynOS_Bin_Compress_Check(
        (sLengthCompressed = compressBound(sLengthUncompressed)) != 0,
        __FUNCTION__, aFilename.c_str(), "Cannot compute compressed size"
    )) return false;

    // Allocate memory for compressed buffer
    if (!DynOS_Bin_Compress_Check(
        (sBufferCompressed = (u8 *) calloc(sLengthCompressed, sizeof(u8))) != NULL,
        __FUNCTION__, aFilename.c_str(), "Cannot allocate memory for compression"
    )) return false;

    // Compress data
    uLongf _LengthCompressed = (uLongf)sLengthCompressed;
    if (!DynOS_Bin_Compress_Check(
        compress2(sBufferCompressed, &_LengthCompressed, sBufferUncompressed, sLengthUncompressed, Z_BEST_COMPRESSION) == Z_OK,
        __FUNCTION__, aFilename.c_str(), "Cannot compress data"
    )) return false;
    sLengthCompressed = _LengthCompressed;

    // Check output length
    // If the compression generates a bigger file, skip the process, but don't return a failure
    if (!DynOS_Bin_Compress_Check(
        sLengthCompressed < sLengthUncompressed,
        __FUNCTION__, aFilename.c_str(), "Compressed data is bigger than uncompressed; Skipping compression"
    )) return true;

    // Open output file
    if (!DynOS_Bin_Compress_Check(
        (sFile = fopen(aFilename.c_str(), "wb")) != NULL,
        __FUNCTION__, aFilename.c_str(), "Cannot open file"
    )) return false;

    // Write magic + uncompressed file size (little-endian)
    u8 header[16] = { 0 };
    memcpy(header, DYNOS_BIN_COMPRESS_MAGIC_BYTES, 8);
    DynOS_WriteU64LE(header + 8, sLengthUncompressed);
    if (!DynOS_Bin_Compress_Check(
        fwrite(header, sizeof(header), 1, sFile) == 1,
        __FUNCTION__, aFilename.c_str(), "Cannot write header"
    )) return false;

    // Write compressed data
    if (!DynOS_Bin_Compress_Check(
        fwrite(sBufferCompressed, sizeof(u8), sLengthCompressed, sFile) == sLengthCompressed,
        __FUNCTION__, aFilename.c_str(), "Cannot write compressed data"
    )) return false;

    // Done, free buffers and files
    DynOS_Bin_Compress_Free();
    Print(" Done.");
    return true;
}

static BinFile *DynOS_Bin_Decompress_ModFs(const SysPath &aFilename) {
    DynOS_Bin_Compress_Init();

    // Read file data
    void *_Buffer = NULL;
    u32 _Size = 0;
    if (!mod_fs_read_file_from_uri(aFilename.c_str(), &_Buffer, &_Size)) {
        DynOS_Bin_Compress_Free();
        return NULL;
    }
    sBufferCompressed = (u8 *) _Buffer;
    sLengthCompressed = _Size;

    // Check file length
    u64 _LengthHeader = (u64) (sizeof(u64) + sizeof(u64));
    if (!DynOS_Bin_Compress_Check(
        sLengthCompressed >= _LengthHeader,
        __FUNCTION__, aFilename.c_str(), "Empty file"
    )) return NULL;

    // Compare with magic constant
    // If not equal, it's not a compressed file
    if (!DynOS_Bin_MagicMatches(_Buffer)) {
        BinFile *_BinFile = BinFile::OpenB(sBufferCompressed, sLengthCompressed);
        DynOS_Bin_Compress_Free();
        return _BinFile;
    }
    PrintNoNewLine("Decompressing file \"%s\"...", aFilename.c_str());

    // Read expected uncompressed file size
    sLengthUncompressed = DynOS_ReadU64LE(sBufferCompressed + 8);
    sLengthCompressed -= _LengthHeader;
    u8 *_BufferCompressed = sBufferCompressed + _LengthHeader;

    // Allocate memory for uncompressed buffer
    if (!DynOS_Bin_Compress_Check(
        (sBufferUncompressed = (u8 *) calloc(sLengthUncompressed, sizeof(u8))) != NULL,
        __FUNCTION__, aFilename.c_str(), "Cannot allocate memory for decompression"
    )) return NULL;

    // Uncompress data
    uLongf _LengthUncompressed = (uLongf)sLengthUncompressed;
    int uncompressRc = uncompress(sBufferUncompressed, &_LengthUncompressed, _BufferCompressed, sLengthCompressed);
    sLengthUncompressed = _LengthUncompressed;
    if (!DynOS_Bin_Compress_Check(
        uncompressRc == Z_OK,
        __FUNCTION__, aFilename.c_str(), "Cannot uncompress data"
    )) {
        PrintError("ERROR: uncompress rc: %d, length uncompressed: %lu, length compressed: %lu, length header: %lu", uncompressRc, sLengthUncompressed, sLengthCompressed, _LengthHeader);
        return NULL;
    }
    Print("uncompress rc: %d, length uncompressed: %lu, length compressed: %lu, length header: %lu", uncompressRc, sLengthUncompressed, sLengthCompressed, _LengthHeader);

    // Return uncompressed data as a BinFile
    BinFile *_BinFile = BinFile::OpenB(sBufferUncompressed, sLengthUncompressed);
    DynOS_Bin_Compress_Free();
    Print(" Done.");
    return _BinFile;
}

BinFile *DynOS_Bin_Decompress(const SysPath &aFilename) {
    DynOS_Bin_Compress_Init();

    // Check modfs
    if (is_mod_fs_file(aFilename.c_str())) {
        return DynOS_Bin_Decompress_ModFs(aFilename);
    }

    // Open input file
    if (!DynOS_Bin_Compress_Check(
        (sFile = f_open_r(aFilename.c_str())) != NULL,
        __FUNCTION__, aFilename.c_str(), "Cannot open file"
    )) return NULL;

    // Read magic bytes
    u8 magic[8] = { 0 };
    if (!DynOS_Bin_Compress_Check(
        f_read(magic, sizeof(magic), 1, sFile) == 1,
        __FUNCTION__, aFilename.c_str(), "Cannot read magic"
    )) return NULL;

    if (!DynOS_Bin_MagicMatches(magic)) {
        DynOS_Bin_Compress_Free();
        return BinFile::OpenR(aFilename.c_str());
    }
    PrintNoNewLine("Decompressing file \"%s\"...", aFilename.c_str());

    // Read expected uncompressed file size
    u8 sizeBytes[8] = { 0 };
    if (!DynOS_Bin_Compress_Check(
        f_read(sizeBytes, sizeof(sizeBytes), 1, sFile) == 1,
        __FUNCTION__, aFilename.c_str(), "Cannot read uncompressed file size"
    )) return NULL;
    sLengthUncompressed = DynOS_ReadU64LE(sizeBytes);

    // Retrieve file length
    if (!DynOS_Bin_Compress_Check(
        f_seek(sFile, 0, SEEK_END) == 0,
        __FUNCTION__, aFilename.c_str(), "Cannot retrieve file length"
    )) return NULL;

    // Check file length
    u64 _LengthHeader = (u64) (sizeof(u64) + sizeof(u64));
    if (!DynOS_Bin_Compress_Check(
        (sLengthCompressed = (u64) f_tell(sFile)) >= _LengthHeader,
        __FUNCTION__, aFilename.c_str(), "Empty file"
    )) return NULL;

    // Allocate memory for compressed buffer
    if (!DynOS_Bin_Compress_Check(
        (sBufferCompressed = (u8 *) calloc(sLengthCompressed - _LengthHeader, sizeof(u8))) != NULL,
        __FUNCTION__, aFilename.c_str(), "Cannot allocate memory for decompression"
    )) return NULL; else f_seek(sFile, _LengthHeader, SEEK_SET);

    // Read input data
    if (!DynOS_Bin_Compress_Check(
        f_read(sBufferCompressed, sizeof(u8), sLengthCompressed - _LengthHeader, sFile) == sLengthCompressed - _LengthHeader,
        __FUNCTION__, aFilename.c_str(), "Cannot read compressed data"
    )) return NULL; else DynOS_Bin_Compress_Close();

    // Allocate memory for uncompressed buffer
    if (!DynOS_Bin_Compress_Check(
        (sBufferUncompressed = (u8 *) calloc(sLengthUncompressed, sizeof(u8))) != NULL,
        __FUNCTION__, aFilename.c_str(), "Cannot allocate memory for decompression"
    )) return NULL;

    // Uncompress data
    uLongf _LengthUncompressed = (uLongf)sLengthUncompressed;
    int uncompressRc = uncompress(sBufferUncompressed, &_LengthUncompressed, sBufferCompressed, sLengthCompressed - _LengthHeader);
    sLengthUncompressed = _LengthUncompressed;
    if (!DynOS_Bin_Compress_Check(
        uncompressRc == Z_OK,
        __FUNCTION__, aFilename.c_str(), "Cannot uncompress data"
    )) {
        PrintError("ERROR: uncompress rc: %d, length uncompressed: %lu, length compressed: %lu, length header: %lu", uncompressRc, sLengthUncompressed, sLengthCompressed, _LengthHeader);
        return NULL;
    }
    Print("uncompress rc: %d, length uncompressed: %lu, length compressed: %lu, length header: %lu", uncompressRc, sLengthUncompressed, sLengthCompressed, _LengthHeader);

    // Return uncompressed data as a BinFile
    BinFile *_BinFile = BinFile::OpenB(sBufferUncompressed, sLengthUncompressed);
    DynOS_Bin_Compress_Free();
    Print(" Done.");
    return _BinFile;
}
