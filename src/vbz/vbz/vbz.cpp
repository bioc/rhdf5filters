#include "v0/vbz_streamvbyte.h"
#include "v1/vbz_streamvbyte.h"

#include "../third_party/gsl/gsl-lite.hpp"
#include <zstd.h>

#include <cassert>
#include <cstddef>
#include <iostream>
#include <memory>

// include last - it uses c headers which can mess things up.
#include "vbz.h"

namespace {
// util for using malloc with unique_ptr
struct free_delete
{
    void operator()(void* x) { free(x); }
};
    
gsl::span<char> make_data_buffer(void* data, vbz_size_t size)
{
    return gsl::make_span(static_cast<char*>(data), size);
}

gsl::span<char const> make_data_buffer(void const* data, vbz_size_t size)
{
    return gsl::make_span(static_cast<char const*>(data), size);
}
    
void copy_buffer(
    gsl::span<char const> source,
    gsl::span<char> dest)
{
    std::copy(source.begin(), source.end(), dest.begin());
}

struct VbzSizedHeader
{
    vbz_size_t original_size;
};

}

extern "C" {

bool vbz_is_error(vbz_size_t result_value)
{
    return result_value >= VBZ_FIRST_ERROR;
}

char const* vbz_error_string(vbz_size_t error_value)
{
    if (VBZ_ZSTD_ERROR == error_value) return "VBZ_ZSTD_ERROR";
    if (VBZ_STREAMVBYTE_INPUT_SIZE_ERROR == error_value) return "VBZ_STREAMVBYTE_INPUT_SIZE_ERROR";
    if (VBZ_STREAMVBYTE_INTEGER_SIZE_ERROR == error_value) return "VBZ_STREAMVBYTE_INTEGER_SIZE_ERROR";
    if (VBZ_STREAMVBYTE_DESTINATION_SIZE_ERROR == error_value) return "VBZ_STREAMVBYTE_DESTINATION_SIZE_ERROR";
    if (VBZ_STREAMVBYTE_STREAM_ERROR == error_value) return "VBZ_STREAMVBYTE_STREAM_ERROR";
    if (VBZ_VERSION_ERROR == error_value) return "VBZ_VERSION_ERROR";

    return "VBZ_UNKNOWN_ERROR";
}

vbz_size_t vbz_max_compressed_size(
    vbz_size_t source_size,
    CompressionOptions const* options)
{
    vbz_size_t max_size = source_size;
    if (options->integer_size != 0 || options->perform_delta_zig_zag)
    {
        auto size_fn = vbz_max_streamvbyte_compressed_size_v0;
        if (options->vbz_version == 1)
        {
            size_fn = vbz_max_streamvbyte_compressed_size_v1;
        }
        else if (options->vbz_version != 0)
        {
            return VBZ_VERSION_ERROR;
        }
        
        max_size = vbz_size_t(size_fn(options->integer_size, max_size));
        if (vbz_is_error(max_size))
        {
            return max_size;
        }
    }

    if (options->zstd_compression_level != 0)
    {
        max_size = vbz_size_t(ZSTD_compressBound(max_size));
    }

    // Always include sized header for simplicity.
    return max_size + sizeof(VbzSizedHeader);
}

vbz_size_t vbz_compress(
    void const* source,
    vbz_size_t source_size,
    void* destination,
    vbz_size_t destination_capacity,
    CompressionOptions const* options)
{
    auto current_source = make_data_buffer(source, source_size);
    auto dest_buffer = make_data_buffer(destination, destination_capacity);

    if (options->zstd_compression_level == 0 && options->integer_size == 0)
    {
        copy_buffer(current_source, dest_buffer);
        return source_size;
    }

    // optional intermediate buffer - allocated if needed later, but stored for
    // duration of call.
    std::unique_ptr<void, free_delete> intermediate_storage;
    
    if (options->integer_size != 0)
    {
        auto size_fn = vbz_max_streamvbyte_compressed_size_v0;
        if (options->vbz_version == 1)
        {
            size_fn = vbz_max_streamvbyte_compressed_size_v1;
        }
        else if (options->vbz_version != 0)
        {
            return VBZ_VERSION_ERROR;
        }
        
        auto max_stream_v_byte_size = size_fn(
            options->integer_size,
            vbz_size_t(current_source.size())
        );
        if (vbz_is_error(max_stream_v_byte_size))
        {
            return max_stream_v_byte_size;
        }
        
        auto streamvbyte_dest = dest_buffer;
        if (options->zstd_compression_level != 0)
        {
            intermediate_storage.reset(malloc(max_stream_v_byte_size));
            streamvbyte_dest = make_data_buffer(intermediate_storage.get(), max_stream_v_byte_size);
        }
        else
        {
            assert(max_stream_v_byte_size <= destination_capacity);
        }

        auto compress_fn = vbz_delta_zig_zag_streamvbyte_compress_v0;
        if (options->vbz_version == 1)
        {
            compress_fn = vbz_delta_zig_zag_streamvbyte_compress_v1;
        }
        else if (options->vbz_version != 0)
        {
            return VBZ_VERSION_ERROR;
        }
        
        auto compressed_size = compress_fn(
            current_source.data(),
            vbz_size_t(current_source.size()),
            streamvbyte_dest.data(),
            vbz_size_t(streamvbyte_dest.size()),
            options->integer_size,
            options->perform_delta_zig_zag
        );

        current_source = make_data_buffer(streamvbyte_dest.data(), compressed_size);
    }

    if (options->zstd_compression_level == 0)
    {
        // destination already written to above.
        return vbz_size_t(current_source.size());
    }
    
    auto compressed_size = ZSTD_compress(
        dest_buffer.data(),
        vbz_size_t(dest_buffer.size()),
        current_source.data(),
        vbz_size_t(current_source.size()),
        options->zstd_compression_level
    );
    if (ZSTD_isError(compressed_size))
    {
        return VBZ_ZSTD_ERROR;
    }

    
    return vbz_size_t(compressed_size);
}

vbz_size_t vbz_decompress(
    const void* source,
    vbz_size_t source_size,
    void* destination,
    vbz_size_t destination_size,
    CompressionOptions const* options)
{
    auto current_source = make_data_buffer(source, source_size);
    auto dest_buffer = make_data_buffer(destination, destination_size);

    // If nothing is enabled, just do a copy between buffers and return.
    if (options->zstd_compression_level == 0 && options->integer_size == 0)
    {
        copy_buffer(current_source, dest_buffer);
        return source_size;
    }

    // optional intermediate buffer - allocated if needed later, but stored for
    // duration of call.
    std::unique_ptr<void, free_delete> intermediate_storage;
    
    if (options->zstd_compression_level != 0)
    {
        auto max_zstd_decompressed_size = ZSTD_getFrameContentSize(source, source_size);
        if (ZSTD_isError(max_zstd_decompressed_size))
        {
            return VBZ_ZSTD_ERROR;
        }

        auto zstd_dest = dest_buffer;
        if (options->integer_size != 0)
        {
            intermediate_storage.reset(malloc(max_zstd_decompressed_size));
            zstd_dest = make_data_buffer(intermediate_storage.get(), (vbz_size_t)max_zstd_decompressed_size);
        }
        else
        {
            assert(max_zstd_decompressed_size <= destination_size);
        }

        auto compressed_size = ZSTD_decompress(
            zstd_dest.data(),
            zstd_dest.size(),
            current_source.data(),
            current_source.size()
        );
        if (ZSTD_isError(compressed_size))
        {
            return VBZ_ZSTD_ERROR;
        }
        current_source = make_data_buffer(zstd_dest.data(), vbz_size_t(compressed_size));
    }

    // if streamvbyte is disabled, return early.
    if (options->integer_size == 0)
    {
        return vbz_size_t(current_source.size());
    }

    auto decompress_fn = vbz_delta_zig_zag_streamvbyte_decompress_v0;
    if (options->vbz_version == 1)
    {
        decompress_fn = vbz_delta_zig_zag_streamvbyte_decompress_v1;
    }
    else if (options->vbz_version != 0)
    {
        return VBZ_VERSION_ERROR;
    }
    
    return decompress_fn(
        current_source.data(),
        vbz_size_t(current_source.size()),
        dest_buffer.data(),
        vbz_size_t(dest_buffer.size()),
        options->integer_size,
        options->perform_delta_zig_zag
    );
}

vbz_size_t vbz_compress_sized(
    void const* source,
    vbz_size_t source_size,
    void* destination,
    vbz_size_t destination_capacity,
    CompressionOptions const* options)
{
    auto dest_buffer = make_data_buffer(destination, destination_capacity);

    // Extract header information
    auto& dest_header = dest_buffer.subspan(0, sizeof(VbzSizedHeader)).as_span<VbzSizedHeader>()[0];
    dest_header.original_size = source_size;

    // Compress data info remaining dest buffer
    auto dest_compressed_data = dest_buffer.subspan(sizeof(VbzSizedHeader));
    auto compressed_size = vbz_compress(
        source,
        source_size,
        dest_compressed_data.data(),
        vbz_size_t(dest_compressed_data.size()),
        options
    );
    
    return compressed_size + sizeof(VbzSizedHeader);
}

vbz_size_t vbz_decompress_sized(
    void const* source,
    vbz_size_t source_size,
    void* destination,
    vbz_size_t destination_capacity,
    CompressionOptions const* options)
{
    auto source_buffer = make_data_buffer(source, source_size);

    if (source_buffer.size() < sizeof(VbzSizedHeader))
    {
        return VBZ_STREAMVBYTE_DESTINATION_SIZE_ERROR;
    }

    // Extract header information
    auto const& source_header = source_buffer.subspan(0, sizeof(VbzSizedHeader)).as_span<VbzSizedHeader const>()[0];
    if (destination_capacity < source_header.original_size)
    {
        return VBZ_STREAMVBYTE_DESTINATION_SIZE_ERROR;
    }

    // Compress data info remaining dest buffer
    auto src_compressed_data = source_buffer.subspan(sizeof(VbzSizedHeader));
    return vbz_decompress(
        src_compressed_data.data(),
        vbz_size_t(src_compressed_data.size()),
        destination,
        source_header.original_size,
        options
    );
}

vbz_size_t vbz_decompressed_size(
    void const* source,
    vbz_size_t source_size,
    CompressionOptions const* options)
{
    auto source_buffer = make_data_buffer(source, source_size);

    auto const& source_header = source_buffer.subspan(0, sizeof(VbzSizedHeader)).as_span<VbzSizedHeader const>()[0];
    return source_header.original_size;
}


}
