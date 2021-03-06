//--------------------------------------------------------------------------
// Copyright (C) 2014-2016 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// http_stream_splitter_reassemble.cc author Tom Peters <thopeter@cisco.com>

#include <assert.h>
#include <sys/types.h>

#include "file_api/file_flows.h"
#include "http_enum.h"
#include "http_field.h"
#include "http_test_manager.h"
#include "http_test_input.h"
#include "http_inspect.h"
#include "http_stream_splitter.h"

using namespace HttpEnums;

void HttpStreamSplitter::chunk_spray(HttpFlowData* session_data, uint8_t* buffer,
    const uint8_t* data, unsigned length) const
{
    ChunkState& curr_state = session_data->chunk_state[source_id];
    uint32_t& expected = session_data->chunk_expected_length[source_id];
    bool& is_broken_chunk = session_data->is_broken_chunk[source_id];
    uint32_t& num_good_chunks = session_data->num_good_chunks[source_id];

    if (is_broken_chunk && (num_good_chunks == 0))
        curr_state = CHUNK_BAD;

    for (uint32_t k=0; k < length; k++)
    {
        switch (curr_state)
        {
        case CHUNK_NUMBER:
            if (data[k] == '\r')
                curr_state = CHUNK_HCRLF;
            else if (data[k] == ';')
                curr_state = CHUNK_OPTIONS;
            else if (is_sp_tab[data[k]])
                curr_state = CHUNK_WHITESPACE;
            else
                expected = expected * 16 + as_hex[data[k]];
            break;
        case CHUNK_OPTIONS:
        case CHUNK_WHITESPACE:
            // No practical difference between white space and options in reassemble()
            if (data[k] == '\r')
                curr_state = CHUNK_HCRLF;
            break;
        case CHUNK_HCRLF:
            if (expected > 0)
                curr_state = CHUNK_DATA;
            else
            {
                // Terminating zero-length chunk
                assert(k+1 == length);
                curr_state = CHUNK_NUMBER;
            }
            break;
        case CHUNK_DATA:
          {
            const uint32_t skip_amount = (length-k <= expected) ? length-k : expected;
            const bool at_start = (session_data->body_octets[source_id] == 0) &&
                (session_data->section_offset[source_id] == 0);
            decompress_copy(buffer, session_data->section_offset[source_id], data+k, skip_amount,
                session_data->compression[source_id], session_data->compress_stream[source_id],
                at_start, session_data->infractions[source_id], session_data->events[source_id]);
            if ((expected -= skip_amount) == 0)
                curr_state = CHUNK_DCRLF1;
            k += skip_amount-1;
            break;
          }
        case CHUNK_DCRLF1:
            curr_state = CHUNK_DCRLF2;
            break;
        case CHUNK_DCRLF2:
            if (is_broken_chunk && (--num_good_chunks == 0))
                curr_state = CHUNK_BAD;
            else
            {
                curr_state = CHUNK_NUMBER;
                expected = 0;
            }
            break;
        case CHUNK_BAD:
          {
            const uint32_t skip_amount = length-k;
            const bool at_start = (session_data->body_octets[source_id] == 0) &&
                (session_data->section_offset[source_id] == 0);
            decompress_copy(buffer, session_data->section_offset[source_id], data+k, skip_amount,
                session_data->compression[source_id], session_data->compress_stream[source_id],
                at_start, session_data->infractions[source_id], session_data->events[source_id]);
            k += skip_amount-1;
            break;
          }
        case CHUNK_ZEROS:
            // Not a possible state in reassemble(). Here to avoid compiler warning.
            assert(false);
            break;
        }
    }
}

void HttpStreamSplitter::decompress_copy(uint8_t* buffer, uint32_t& offset, const uint8_t* data,
    uint32_t length, HttpEnums::CompressId& compression, z_stream*& compress_stream,
    bool at_start, HttpInfractions& infractions, HttpEventGen& events)
{
    if ((compression == CMP_GZIP) || (compression == CMP_DEFLATE))
    {
        compress_stream->next_in = (Bytef*)data;
        compress_stream->avail_in = length;
        compress_stream->next_out = buffer + offset;
        compress_stream->avail_out = MAX_OCTETS - offset;
        int ret_val = inflate(compress_stream, Z_SYNC_FLUSH);

        if ((ret_val == Z_OK) || (ret_val == Z_STREAM_END))
        {
            offset = MAX_OCTETS - compress_stream->avail_out;
            if (compress_stream->avail_in > 0)
            {
                // There are two ways not to consume all the input
                if (ret_val == Z_STREAM_END)
                {
                    // The zipped data stream ended but there is more input data
                    infractions += INF_GZIP_EARLY_END;
                    events.create_event(EVENT_GZIP_FAILURE);
                    const uInt num_copy =
                        (compress_stream->avail_in <= compress_stream->avail_out) ?
                        compress_stream->avail_in : compress_stream->avail_out;
                    memcpy(buffer + offset, data, num_copy);
                    offset += num_copy;
                }
                else
                {
                    assert(compress_stream->avail_out == 0);
                    // The data expanded too much
                    infractions += INF_GZIP_OVERRUN;
                    events.create_event(EVENT_GZIP_OVERRUN);
                }
                compression = CMP_NONE;
                inflateEnd(compress_stream);
                delete compress_stream;
                compress_stream = nullptr;
            }
            return;
        }
        else if ((compression == CMP_DEFLATE) && at_start && (ret_val == Z_DATA_ERROR))
        {
            // Some incorrect implementations of deflate don't use the expected header. Feed a
            // dummy header to zlib and retry the inflate.
            static constexpr char zlib_header[2] = { 0x78, 0x01 };

            inflateReset(compress_stream);
            compress_stream->next_in = (Bytef*)zlib_header;
            compress_stream->avail_in = sizeof(zlib_header);
            inflate(compress_stream, Z_SYNC_FLUSH);

            // Start over at the beginning
            decompress_copy(buffer, offset, data, length, compression, compress_stream, false,
                infractions, events);
            return;
        }
        else
        {
            infractions += INF_GZIP_FAILURE;
            events.create_event(EVENT_GZIP_FAILURE);
            compression = CMP_NONE;
            inflateEnd(compress_stream);
            delete compress_stream;
            compress_stream = nullptr;
            // Since we failed to uncompress the data, fall through
        }
    }

    // The following precaution is necessary because mixed compressed and uncompressed data can
    // cause the buffer to overrun even though we are not decompressing right now
    if (length > MAX_OCTETS - offset)
    {
        length = MAX_OCTETS - offset;
        infractions += INF_GZIP_OVERRUN;
        events.create_event(EVENT_GZIP_OVERRUN);
    }
    memcpy(buffer + offset, data, length);
    offset += length;
}

const StreamBuffer* HttpStreamSplitter::reassemble(Flow* flow, unsigned total, unsigned,
    const uint8_t* data, unsigned len, uint32_t flags, unsigned& copied)
{
    static THREAD_LOCAL StreamBuffer http_buf;

    copied = len;

    assert(total <= MAX_OCTETS);

    HttpFlowData* session_data = (HttpFlowData*)flow->get_flow_data(HttpFlowData::http_flow_id);
    assert(session_data != nullptr);

#ifdef REG_TEST
    if (HttpTestManager::use_test_output())
    {
        if (HttpTestManager::use_test_input())
        {
            if (!(flags & PKT_PDU_TAIL))
            {
                return nullptr;
            }
            bool tcp_close;
            uint8_t* test_buffer;
            HttpTestManager::get_test_input_source()->reassemble(&test_buffer, len, source_id,
                tcp_close);
            if (tcp_close)
            {
                finish(flow);
            }
            if (test_buffer == nullptr)
            {
                // Source ID does not match test data, no test data was flushed, or there is no
                // more test data
                return nullptr;
            }
            data = test_buffer;
            total = len;
        }
        else
        {
            printf("Reassemble from flow data %" PRIu64 " direction %d total %u length %u\n",
                session_data->seq_num, source_id, total, len);
            fflush(stdout);
        }
    }
#endif

    if (session_data->section_type[source_id] == SEC__NOT_COMPUTE)
    {   // FIXIT-M In theory this check should not be necessary
        return nullptr;
    }

    // FIXIT-P stream should be ehanced to do discarding for us. For now flush-then-discard here
    // is how scan() handles things we don't need to examine.
    if (session_data->section_type[source_id] == SEC_DISCARD)
    {
#ifdef REG_TEST
        if (HttpTestManager::use_test_output())
        {
            fprintf(HttpTestManager::get_output_file(), "Discarded %u octets\n\n", len);
            fflush(HttpTestManager::get_output_file());
        }
#endif
        if (flags & PKT_PDU_TAIL)
        {
            session_data->section_type[source_id] = SEC__NOT_COMPUTE;

            // When we are skipping through a message body beyond flow depth this is the end of
            // the line. Here we do the message section's normal job of updating the flow for the
            // next stage.
            if (session_data->cutter[source_id] == nullptr)
            {
                if (session_data->type_expected[source_id] == SEC_BODY_CL)
                {
                    session_data->half_reset(source_id);
                }
                else if (session_data->type_expected[source_id] == SEC_BODY_CHUNK)
                {
                    session_data->trailer_prep(source_id);
                }
            }
        }
        return nullptr;
    }

    HttpModule::increment_peg_counts(PEG_REASSEMBLE);

    uint8_t*& buffer = session_data->section_buffer[source_id];

    const bool is_body = (session_data->section_type[source_id] == SEC_BODY_CHUNK) ||
                         (session_data->section_type[source_id] == SEC_BODY_CL) ||
                         (session_data->section_type[source_id] == SEC_BODY_OLD);
    if (buffer == nullptr)
    {
        // The type of buffer used is based on section type. All body sections reuse a single
        // static buffer. Other sections use a dynamic buffer that may be saved for a while.
        // Changes here must be mirrored below where the buffer is passed to HttpInspect::process
        // and in ~HttpFlowData where the buffer will be deleted if it has not been processed.
        if (is_body)
        {
            buffer = HttpInspect::body_buffer;
        }
        else
        {
            buffer = new uint8_t[total];
        }
    }

    if (session_data->section_type[source_id] != SEC_BODY_CHUNK)
    {
        const bool at_start = (session_data->body_octets[source_id] == 0) &&
             (session_data->section_offset[source_id] == 0);
        decompress_copy(buffer, session_data->section_offset[source_id], data, len,
            session_data->compression[source_id], session_data->compress_stream[source_id],
            at_start, session_data->infractions[source_id], session_data->events[source_id]);
    }
    else
    {
        chunk_spray(session_data, buffer, data, len);
    }

    if (flags & PKT_PDU_TAIL)
    {
        const Field& send_to_detection = my_inspector->process(buffer,
            session_data->section_offset[source_id] - session_data->num_excess[source_id], flow,
            source_id, !is_body);
        // delete[] not necessary because HttpMsgSection is now responsible.
        buffer = nullptr;

        session_data->section_offset[source_id] = 0;

        // The detection section of a message is the first body section, unless there is no body
        // section in which case it is the headers. The detection section is always returned to the
        // framework and forwarded to detection even if it is empty. Other body sections and the
        // trailer section are only forwarded if nonempty. The start line section and header
        // sections other than the detection section are never forwarded.
        if (((send_to_detection.length > 0) && (HttpInspect::get_latest_is() != IS_NONE)) ||
            ((send_to_detection.length == 0) && (HttpInspect::get_latest_is() == IS_DETECTION)))
        {
            // FIXIT-M kludge until we work out issues with returning an empty buffer
            if (send_to_detection.length > 0)
            {
                http_buf.data = send_to_detection.start;
                http_buf.length = send_to_detection.length;
            }
            else
            {
                http_buf.data = (const uint8_t*)"";
                http_buf.length = 1;
            }
#ifdef REG_TEST
            if (HttpTestManager::use_test_output())
            {
                fprintf(HttpTestManager::get_output_file(), "Sent to detection %u octets\n\n",
                    http_buf.length);
                fflush(HttpTestManager::get_output_file());
            }
#endif
            return &http_buf;
        }
        my_inspector->clear(session_data, source_id);
    }
    return nullptr;
}

