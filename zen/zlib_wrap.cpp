// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "zlib_wrap.h"
//Windows:     use the SAME zlib version that wxWidgets is linking against! //C:\Data\Projects\wxWidgets\Source\src\zlib\zlib.h
//Linux/macOS: use zlib system header for both wxWidgets and libcurl (zlib is required for HTTP, SFTP)
//             => don't compile wxWidgets with: --with-zlib=builtin
#include <zlib.h> //https://www.zlib.net/manual.html
#include <zen/scope_guard.h>

using namespace zen;


namespace
{
std::wstring getZlibErrorLiteral(int sc)
{
    switch (sc)
    {
            ZEN_CHECK_CASE_FOR_CONSTANT(Z_OK);
            ZEN_CHECK_CASE_FOR_CONSTANT(Z_STREAM_END);
            ZEN_CHECK_CASE_FOR_CONSTANT(Z_NEED_DICT);
            ZEN_CHECK_CASE_FOR_CONSTANT(Z_ERRNO);
            ZEN_CHECK_CASE_FOR_CONSTANT(Z_STREAM_ERROR);
            ZEN_CHECK_CASE_FOR_CONSTANT(Z_DATA_ERROR);
            ZEN_CHECK_CASE_FOR_CONSTANT(Z_MEM_ERROR);
            ZEN_CHECK_CASE_FOR_CONSTANT(Z_BUF_ERROR);
            ZEN_CHECK_CASE_FOR_CONSTANT(Z_VERSION_ERROR);

        default:
            return replaceCpy<std::wstring>(L"zlib error %x", L"%x", numberTo<std::wstring>(sc));
    }
}
}


size_t zen::impl::zlib_compressBound(size_t len)
{
    return ::compressBound(static_cast<uLong>(len)); //upper limit for buffer size, larger than input size!!!
}


size_t zen::impl::zlib_compress(const void* src, size_t srcLen, void* trg, size_t trgLen, int level) //throw SysError
{
    uLongf bufSize = static_cast<uLong>(trgLen);
    const int rv = ::compress2(static_cast<Bytef*>(trg),       //Bytef* dest,
                               &bufSize,                       //uLongf* destLen,
                               static_cast<const Bytef*>(src), //const Bytef* source,
                               static_cast<uLong>(srcLen),     //uLong sourceLen,
                               level);                         //int level
    // Z_OK: success
    // Z_MEM_ERROR: not enough memory
    // Z_BUF_ERROR: not enough room in the output buffer
    if (rv != Z_OK || bufSize > trgLen)
        throw SysError(formatSystemError("zlib compress2", getZlibErrorLiteral(rv), L""));

    return bufSize;
}


size_t zen::impl::zlib_decompress(const void* src, size_t srcLen, void* trg, size_t trgLen) //throw SysError
{
    uLongf bufSize = static_cast<uLong>(trgLen);
    const int rv = ::uncompress(static_cast<Bytef*>(trg),       //Bytef* dest,
                                &bufSize,                       //uLongf* destLen,
                                static_cast<const Bytef*>(src), //const Bytef* source,
                                static_cast<uLong>(srcLen));    //uLong sourceLen
    // Z_OK: success
    // Z_MEM_ERROR: not enough memory
    // Z_BUF_ERROR: not enough room in the output buffer
    // Z_DATA_ERROR: input data was corrupted or incomplete
    if (rv != Z_OK || bufSize > trgLen)
        throw SysError(formatSystemError("zlib uncompress", getZlibErrorLiteral(rv), L""));

    return bufSize;
}


class InputStreamAsGzip::Impl
{
public:
    Impl(const std::function<size_t(void* buffer, size_t bytesToRead)>& readBlock /*throw X*/) : //throw SysError; returning 0 signals EOF: Posix read() semantics
        readBlock_(readBlock)
    {
        const int windowBits = MAX_WBITS + 16; //"add 16 to windowBits to write a simple gzip header"

        //"memLevel=1 uses minimum memory but is slow and reduces compression ratio; memLevel=9 uses maximum memory for optimal speed.
        const int memLevel = 9; //test; 280 MB installer file: level 9 shrinks runtime by ~8% compared to level 8 (==DEF_MEM_LEVEL) at the cost of 128 KB extra memory
        static_assert(memLevel <= MAX_MEM_LEVEL);

        const int rv = ::deflateInit2(&gzipStream_,          //z_streamp strm
                                      3 /*see db_file.cpp*/, //int level
                                      Z_DEFLATED,            //int method
                                      windowBits,            //int windowBits
                                      memLevel,              //int memLevel
                                      Z_DEFAULT_STRATEGY);   //int strategy
        if (rv != Z_OK)
            throw SysError(formatSystemError("zlib deflateInit2", getZlibErrorLiteral(rv), L""));
    }

    ~Impl()
    {
        [[maybe_unused]] const int rv = ::deflateEnd(&gzipStream_);
        assert(rv == Z_OK);
    }

    size_t read(void* buffer, size_t bytesToRead) //throw SysError, X; return "bytesToRead" bytes unless end of stream!
    {
        if (bytesToRead == 0) //"read() with a count of 0 returns zero" => indistinguishable from end of file! => check!
            throw std::logic_error("Contract violation! " + std::string(__FILE__) + ':' + numberTo<std::string>(__LINE__));

        gzipStream_.next_out  = static_cast<Bytef*>(buffer);
        gzipStream_.avail_out = static_cast<uInt>(bytesToRead);

        for (;;)
        {
            if (gzipStream_.avail_in == 0 && !eof_)
            {
                if (bufIn_.size() < bytesToRead)
                    bufIn_.resize(bytesToRead);

                const size_t bytesRead = readBlock_(&bufIn_[0], bufIn_.size()); //throw X; returning 0 signals EOF: Posix read() semantics
                gzipStream_.next_in  = reinterpret_cast<z_const Bytef*>(&bufIn_[0]);
                gzipStream_.avail_in = static_cast<uInt>(bytesRead);
                if (bytesRead == 0)
                    eof_ = true;
            }

            const int rv = ::deflate(&gzipStream_, eof_ ? Z_FINISH : Z_NO_FLUSH);
            if (rv == Z_STREAM_END)
                return bytesToRead - gzipStream_.avail_out;
            if (rv != Z_OK)
                throw SysError(formatSystemError("zlib deflate", getZlibErrorLiteral(rv), L""));

            if (gzipStream_.avail_out == 0)
                return bytesToRead;
        }
    }

private:
    const std::function<size_t(void* buffer, size_t bytesToRead)> readBlock_; //throw X
    bool eof_ = false;
    std::vector<std::byte> bufIn_;
    z_stream gzipStream_ = {};
};


zen::InputStreamAsGzip::InputStreamAsGzip(const std::function<size_t(void* buffer, size_t bytesToRead)>& readBlock /*throw X*/) : pimpl_(std::make_unique<Impl>(readBlock)) {} //throw SysError
zen::InputStreamAsGzip::~InputStreamAsGzip() {}
size_t zen::InputStreamAsGzip::read(void* buffer, size_t bytesToRead) { return pimpl_->read(buffer, bytesToRead); } //throw SysError, X


std::string zen::compressAsGzip(const void* buffer, size_t bufSize) //throw SysError
{
    struct MemoryStreamAsGzip : InputStreamAsGzip
    {
        explicit MemoryStreamAsGzip(const std::function<size_t(void* buffer, size_t bytesToRead)>& readBlock /*throw X*/) : InputStreamAsGzip(readBlock) {} //throw SysError
        static size_t getBlockSize() { return 128 * 1024; } //InputStreamAsGzip has no idea what it's wrapping => has no getBlockSize() member!
    };

    MemoryStreamAsGzip gzipStream([&](void* bufIn, size_t bytesToRead) //throw SysError
    {
        const size_t bytesRead = std::min(bufSize, bytesToRead);
        std::memcpy(bufIn, buffer, bytesRead);
        buffer = static_cast<const char*>(buffer) + bytesRead;
        bufSize -= bytesRead;
        return bytesRead; //returning 0 signals EOF: Posix read() semantics
    });
    return bufferedLoad<std::string>(gzipStream); //throw SysError
}
