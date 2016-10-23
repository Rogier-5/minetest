/*
Minetest
Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "serialization.h"

#include "util/serialize.h"
#include "util/string.h"
#if defined(_WIN32) && !defined(WIN32_NO_ZLIB_WINAPI)
	#define ZLIB_WINAPI
#endif
#include <zlib.h>
#include "zstd.h"
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <settings.h>
#include <time.h>

extern float g_decomptime;
extern u32 g_compdata;

extern float g_comptime;
extern u32 g_decompdata;

/* report a zlib or i/o error */
void zerr(int ret)
{
    dstream<<"zerr: ";
    switch (ret) {
    case Z_ERRNO:
        if (ferror(stdin))
            dstream<<"error reading stdin"<<std::endl;
        if (ferror(stdout))
            dstream<<"error writing stdout"<<std::endl;
        break;
    case Z_STREAM_ERROR:
        dstream<<"invalid compression level"<<std::endl;
        break;
    case Z_DATA_ERROR:
        dstream<<"invalid or incomplete deflate data"<<std::endl;
        break;
    case Z_MEM_ERROR:
        dstream<<"out of memory"<<std::endl;
        break;
    case Z_VERSION_ERROR:
        dstream<<"zlib version mismatch!"<<std::endl;
		break;
	default:
		dstream<<"return value = "<<ret<<std::endl;
    }
}

void compressZlib(SharedBuffer<u8> data, std::ostream &os, int level)
{
	z_stream z;
	const s32 bufsize = 16384;
	char output_buffer[bufsize];
	int status = 0;
	int ret;

	z.zalloc = Z_NULL;
	z.zfree = Z_NULL;
	z.opaque = Z_NULL;

	ret = deflateInit(&z, level);
	if(ret != Z_OK)
		throw SerializationError("compressZlib: deflateInit failed");

	// Point zlib to our input buffer
	z.next_in = (Bytef*)&data[0];
	z.avail_in = data.getSize();
	// And get all output
	for(;;)
	{
		z.next_out = (Bytef*)output_buffer;
		z.avail_out = bufsize;

		status = deflate(&z, Z_FINISH);
		if(status == Z_NEED_DICT || status == Z_DATA_ERROR
				|| status == Z_MEM_ERROR)
		{
			zerr(status);
			throw SerializationError("compressZlib: deflate failed");
		}
		int count = bufsize - z.avail_out;
		if(count)
			os.write(output_buffer, count);
		// This determines zlib has given all output
		if(status == Z_STREAM_END)
			break;
	}

	deflateEnd(&z);
}

void compressZlib(const std::string &data, std::ostream &os, int level)
{
	SharedBuffer<u8> databuf((u8*)data.c_str(), data.size());
	compressZlib(databuf, os, level);
}

void decompressZlib(std::istream &is, std::ostream &os)
{
	z_stream z;
	const s32 bufsize = 16384;
	char input_buffer[bufsize];
	char output_buffer[bufsize];
	int status = 0;
	int ret;
	int bytes_read = 0;
	int input_buffer_len = 0;

	z.zalloc = Z_NULL;
	z.zfree = Z_NULL;
	z.opaque = Z_NULL;

	ret = inflateInit(&z);
	if(ret != Z_OK)
		throw SerializationError("decompressZlib: inflateInit failed");

	z.avail_in = 0;

	//dstream<<"initial fail="<<is.fail()<<" bad="<<is.bad()<<std::endl;

	for(;;)
	{
		z.next_out = (Bytef*)output_buffer;
		z.avail_out = bufsize;

		if(z.avail_in == 0)
		{
			z.next_in = (Bytef*)input_buffer;
			is.read(input_buffer, bufsize);
			input_buffer_len = is.gcount();
			z.avail_in = input_buffer_len;
			//dstream<<"read fail="<<is.fail()<<" bad="<<is.bad()<<std::endl;
		}
		if(z.avail_in == 0)
		{
			//dstream<<"z.avail_in == 0"<<std::endl;
			break;
		}

		//dstream<<"1 z.avail_in="<<z.avail_in<<std::endl;
		status = inflate(&z, Z_NO_FLUSH);
		//dstream<<"2 z.avail_in="<<z.avail_in<<std::endl;
		bytes_read += is.gcount() - z.avail_in;
		//dstream<<"bytes_read="<<bytes_read<<std::endl;

		if(status == Z_NEED_DICT || status == Z_DATA_ERROR
				|| status == Z_MEM_ERROR)
		{
			zerr(status);
			throw SerializationError("decompressZlib: inflate failed");
		}
		int count = bufsize - z.avail_out;
		//dstream<<"count="<<count<<std::endl;
		if(count)
			os.write(output_buffer, count);
		if(status == Z_STREAM_END)
		{
			//dstream<<"Z_STREAM_END"<<std::endl;

			//dstream<<"z.avail_in="<<z.avail_in<<std::endl;
			//dstream<<"fail="<<is.fail()<<" bad="<<is.bad()<<std::endl;
			// Unget all the data that inflate didn't take
			is.clear(); // Just in case EOF is set
			for(u32 i=0; i < z.avail_in; i++)
			{
				is.unget();
				if(is.fail() || is.bad())
				{
					dstream<<"unget #"<<i<<" failed"<<std::endl;
					dstream<<"fail="<<is.fail()<<" bad="<<is.bad()<<std::endl;
					throw SerializationError("decompressZlib: unget failed");
				}
			}

			break;
		}
	}

	inflateEnd(&z);
}

void compressBrotli(SharedBuffer<u8> data, std::ostream &os, u8 quality)
{
	BrotliEncoderState* b;
	const s32 bufsize = 16384;
	char buffer[bufsize];
	size_t output_bufsize;
	u8* output_buffer;
	size_t input_bufsize;
	const u8* input_buffer;

	b = BrotliEncoderCreateInstance(NULL, NULL, NULL);
	if (!b)
		throw SerializationError("compressBrotli: BrotliEncoderCreateInstance failed");
	BrotliEncoderSetParameter(b, BROTLI_PARAM_QUALITY, quality);
	BrotliEncoderSetParameter(b, BROTLI_PARAM_LGWIN, 22); // default from command-line tool

	// Point brotli to our input buffer
	input_buffer = &data[0];
	input_bufsize = data.getSize();
	// And get all output
	for (;;) {
		output_buffer = (u8*) buffer;
		output_bufsize = bufsize;

		int status = BrotliEncoderCompressStream(
			b, BROTLI_OPERATION_FINISH,
			&input_bufsize, &input_buffer,
			&output_bufsize, &output_buffer, NULL);
		if (!status)
			throw SerializationError("compressBrotli: BrotliEncoderCompressStream failed");
		int count = bufsize - output_bufsize;
		if (count)
			os.write(buffer, count);

		if (!input_bufsize && !BrotliEncoderHasMoreOutput(b))
			break;
	}

	BrotliEncoderDestroyInstance(b);
}

void compressBrotli(const std::string &data, std::ostream &os, u8 quality)
{
	SharedBuffer<u8> databuf((u8*)data.c_str(), data.size());
	compressBrotli(databuf, os, quality);
}

void decompressBrotli(std::istream &is, std::ostream &os)
{
	BrotliDecoderState* b;
	const s32 bufsize = 16384;
	char input_buffer[bufsize];
	char output_buffer[bufsize];
	size_t output_bufsize;
	u8* output_bufp;
	size_t input_bufsize;
	const u8* input_bufp;

	b = BrotliDecoderCreateInstance(NULL, NULL, NULL);
	if(!b)
		throw SerializationError("decompressBrotli: BrotliCreateState failed");

	input_bufsize = 0;
	input_bufp = (u8*) input_buffer;

	BrotliDecoderResult status = BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT;
	for (;;) {
		output_bufp = (u8*) output_buffer;
		output_bufsize = bufsize;

		if (input_bufsize == 0 || status == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
			memmove(input_buffer, input_bufp, input_bufsize);
			input_bufp = (u8*) input_buffer;
			is.read(input_buffer + input_bufsize, bufsize - input_bufsize);
			input_bufsize += is.gcount();
			if (status == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && is.gcount() == 0)
				throw SerializationError("decompressBrotli: Expected more data - stream is corrupted");
		}

		status = BrotliDecoderDecompressStream(
			b,
			&input_bufsize, &input_bufp,
			&output_bufsize, &output_bufp, NULL);
		if (status == BROTLI_DECODER_RESULT_ERROR) {
			BrotliDecoderErrorCode code = BrotliDecoderGetErrorCode(b);
			throw SerializationError(std::string("decompressBrotli: BrotliDecompressStream failed: ")
				+ BrotliDecoderErrorString(code));
		}
		int count = bufsize - output_bufsize;
		if (count)
			os.write(output_buffer, count);

		if (status == BROTLI_DECODER_RESULT_SUCCESS){
			// Unget all the data that inflate didn't take
			is.clear(); // Just in case EOF is set
			for (u32 i = 0; i < input_bufsize; i++) {
				is.unget();
				if (is.fail() || is.bad())
					throw SerializationError("decompressBrotli: unget failed");
			}

			break;
		}
	}

	BrotliDecoderDestroyInstance(b);
}

void compressZstd(SharedBuffer<u8> data, std::ostream &os, int level)
{
	ZSTD_CStream *stream = ZSTD_createCStream();
	ZSTD_initCStream(stream, level);

	size_t result;

	ZSTD_inBuffer in_buf;
	in_buf.src = (Bytef*)&data[0];
	in_buf.size = data.getSize();
	in_buf.pos = 0;

	const s32 buf_size = 16384;
	char buf[buf_size];

	ZSTD_outBuffer out_buf;
	out_buf.dst = buf;
	out_buf.size = buf_size;
	out_buf.pos = 0;

	// Write the buffer and put whatever compressed stuff we got
	// into the out buffer.
	while (in_buf.pos < in_buf.size) {
		result = ZSTD_compressStream(stream, &out_buf, &in_buf);
		if (ZSTD_isError(result)) {
			ZSTD_freeCStream(stream);
			throw SerializationError("compressZstd: compression failed: "
				+ std::string(ZSTD_getErrorName(result)));
		}
		if (out_buf.pos) {
			os.write(buf, out_buf.pos);
			out_buf.pos = 0;
		}
	}

	// Now finalize the stream.
	for (;;) {
		result = ZSTD_endStream(stream, &out_buf);
		if (ZSTD_isError(result)) {
			ZSTD_freeCStream(stream);
			throw SerializationError("compressZstd: compression failed: "
				+ std::string(ZSTD_getErrorName(result)));
		}
		if (out_buf.pos) {
			os.write(buf, out_buf.pos);
			out_buf.pos = 0;
		}
		// If the internal buffer is cleared
		if (result == 0) {
			break;
		}
	}

	ZSTD_freeCStream(stream);
}

void compressZstd(const std::string &data, std::ostream &os, int level)
{
	SharedBuffer<u8> databuf((u8*)data.c_str(), data.size());
	compressZstd(databuf, os, level);
}

void decompressZstd(std::istream &is, std::ostream &os)
{
	ZSTD_DStream *stream = ZSTD_createDStream();
	ZSTD_initDStream(stream);
	size_t result;

	const s32 in_buf_size = 16384;
	char in_buf_d[in_buf_size];

	const s32 out_buf_size = 16384;
	char out_buf_d[out_buf_size];

	is.read(in_buf_d, in_buf_size);

	ZSTD_inBuffer in_buf;
	in_buf.src = in_buf_d;
	in_buf.size = is.gcount();
	in_buf.pos = 0;

	ZSTD_outBuffer out_buf;
	out_buf.dst = out_buf_d;
	out_buf.size = out_buf_size;
	out_buf.pos = 0;

	while ((result = ZSTD_decompressStream(stream, &out_buf, &in_buf)) > 0) {
		if (ZSTD_isError(result)) {
			ZSTD_freeDStream(stream);
			throw SerializationError("decompressZstd: decompression failed: "
				+ std::string(ZSTD_getErrorName(result)));
		}

		// Reset the position to 0 if the input buffer has been fully read
		if (in_buf.size == in_buf.pos) {
			in_buf.size = 0;
			in_buf.pos = 0;
		}

		// Refill the input buffer
		is.read(&in_buf_d[in_buf.size], in_buf_size - in_buf.size);
		in_buf.size += is.gcount();

		// Flush the output buffer
		if (out_buf.pos) {
			os.write(out_buf_d, out_buf.pos);
			out_buf.pos = 0;
		}
	}

	// Flush the output buffer
	if (out_buf.pos) {
		os.write(out_buf_d, out_buf.pos);
		out_buf.pos = 0;
	}

	// Unget all stuff that the decompressor didn't read
	is.clear(); // Just in case EOF is set
	while (in_buf.size > in_buf.pos) {
		is.unget();
		if (is.fail() || is.bad()) {
			throw SerializationError("decompressZstd: unget failed");
		}
		in_buf.size--;
	}

	ZSTD_freeDStream(stream);
}

static void _compress(SharedBuffer<u8> data, std::ostream &os, u8 version)
{
	if (version >= 26) {
		std::string compression_name = lowercase(g_settings->get("compression"));
		int compression_param = g_settings->getS32("compression_param");
		if (compression_name == "none") {
			writeU8(os, 0);
			os << serializeLongString(std::string((const char *)&data[0], data.getSize()));
		} else if (compression_name == "zlib") {
			writeU8(os, 1);
			compressZlib(data, os, compression_param);
		} else if (compression_name == "zstd") {
			writeU8(os, 2);
			compressZstd(data, os, compression_param);
		} else if (compression_name == "brotli") {
			writeU8(os, 3);
			compressBrotli(data, os, compression_param);
		} else {
			throw SerializationError(std::string("compress: invalid / unsupported compression format: ") + compression_name);
		}
		return;
	} else if (version >= 11) {

		compressZlib(data, os);
		return;
	}

	if(data.getSize() == 0)
		return;

	// Write length (u32)

	u8 tmp[4];
	writeU32(tmp, data.getSize());
	os.write((char*)tmp, 4);

	// We will be writing 8-bit pairs of more_count and byte
	u8 more_count = 0;
	u8 current_byte = data[0];
	for(u32 i=1; i<data.getSize(); i++)
	{
		if(
			data[i] != current_byte
			|| more_count == 255
		)
		{
			// write count and byte
			os.write((char*)&more_count, 1);
			os.write((char*)&current_byte, 1);
			more_count = 0;
			current_byte = data[i];
		}
		else
		{
			more_count++;
		}
	}
	// write count and byte
	os.write((char*)&more_count, 1);
	os.write((char*)&current_byte, 1);
}

void compress(SharedBuffer<u8> data, std::ostream &os, u8 version)
{
	clock_t t = clock();
	_compress(data, os, version);
	t = clock() - t;
	g_comptime += (float)t / CLOCKS_PER_SEC;
	g_decompdata += data.getSize();
}

void compress(const std::string &data, std::ostream &os, u8 version)
{
	SharedBuffer<u8> databuf((u8*)data.c_str(), data.size());
	compress(databuf, os, version);
}


static void _decompress(std::istream &is, std::ostream &os, u8 version)
{
	if (version >= 26) {
		u8 format = readU8(is);
		switch (format) {
			case 0:
				os << deSerializeLongString(is);
				break;
			case 1:
				decompressZlib(is, os);
				break;
			case 2:
				decompressZstd(is, os);
				break;
			case 3:
				decompressBrotli(is, os);
				break;
			default:
				throw SerializationError(std::string("decompress: unsupported compression format: ") + std::to_string(format));
				break;
		}

		return;
	} else if (version >= 11) {

		decompressZlib(is, os);
		return;
	}

	// Read length (u32)

	u8 tmp[4];
	is.read((char*)tmp, 4);
	u32 len = readU32(tmp);

	// We will be reading 8-bit pairs of more_count and byte
	u32 count = 0;
	for(;;)
	{
		u8 more_count=0;
		u8 byte=0;

		is.read((char*)&more_count, 1);

		is.read((char*)&byte, 1);

		if(is.eof())
			throw SerializationError("decompress: stream ended halfway");

		for(s32 i=0; i<(u16)more_count+1; i++)
			os.write((char*)&byte, 1);

		count += (u16)more_count+1;

		if(count == len)
			break;
	}
}

void decompress(std::istream &is, std::ostream &os, u8 version)
{
	clock_t t = clock();
	std::streampos l = is.tellg();
	_decompress(is, os, version);
	t = clock() - t;
	l = is.tellg() - l;
	g_decomptime += (float)t / CLOCKS_PER_SEC;
	g_compdata += l;
}
