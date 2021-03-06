/*
 * Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <errno.h>
#include <stdarg.h>

#include "mongoc-error.h"
#include "mongoc-buffer-private.h"


#ifndef MONGOC_BUFFER_DEFAULT_SIZE
#define MONGOC_BUFFER_DEFAULT_SIZE 1024
#endif


#define SPACE_FOR(_b, _sz) (((ssize_t)(_b)->datalen - (ssize_t)(_b)->off - (ssize_t)(_b)->len) >= _sz)


/**
 * _mongoc_buffer_init:
 * @buffer: A mongoc_buffer_t to initialize.
 * @buf: A data buffer to attach to @buffer.
 * @buflen: The size of @buflen.
 * @realloc_func: A function to resize @buf.
 *
 * Initializes @buffer for use. If additional space is needed by @buffer, then
 * @realloc_func will be called to resize @buf.
 *
 * @buffer takes ownership of @buf and will realloc it to zero bytes when
 * cleaning up the data structure.
 */
void
_mongoc_buffer_init (mongoc_buffer_t   *buffer,
                     bson_uint8_t      *buf,
                     size_t             buflen,
                     bson_realloc_func  realloc_func)
{
   bson_return_if_fail(buffer);
   bson_return_if_fail(buf || !buflen);

   if (!realloc_func) {
      realloc_func = bson_realloc;
   }

   if (!buf || !buflen) {
      buf = realloc_func(NULL, MONGOC_BUFFER_DEFAULT_SIZE);
      buflen = MONGOC_BUFFER_DEFAULT_SIZE;
   }

   memset(buffer, 0, sizeof *buffer);

   buffer->data = buf;
   buffer->datalen = buflen;
   buffer->len = 0;
   buffer->off = 0;
   buffer->realloc_func = realloc_func;
}


/**
 * _mongoc_buffer_destroy:
 * @buffer: A mongoc_buffer_t.
 *
 * Cleanup after @buffer and release any allocated resources.
 */
void
_mongoc_buffer_destroy (mongoc_buffer_t *buffer)
{
   bson_return_if_fail(buffer);

   if (buffer->data && buffer->realloc_func) {
      buffer->realloc_func(buffer->data, 0);
   }

   memset(buffer, 0, sizeof *buffer);
}


/**
 * _mongoc_buffer_clear:
 * @buffer: A mongoc_buffer_t.
 * @zero: If the memory should be zeroed.
 *
 * Clears a buffers contents and resets it to initial state. You can request
 * that the memory is zeroed, which might be useful if you know the contents
 * contain security related information.
 */
void
_mongoc_buffer_clear (mongoc_buffer_t *buffer,
                      bson_bool_t      zero)
{
   bson_return_if_fail(buffer);

   if (zero) {
      memset(buffer->data, 0, buffer->datalen);
   }

   buffer->off = 0;
   buffer->len = 0;
}


/**
 * mongoc_buffer_append_from_stream:
 * @buffer; A mongoc_buffer_t.
 * @stream: The stream to read from.
 * @size: The number of bytes to read.
 * @timeout_msec: The number of milliseconds to wait or -1 for the default
 * @error: A location for a bson_error_t, or NULL.
 *
 * Reads from stream @size bytes and stores them in @buffer. This can be used
 * in conjunction with reading RPCs from a stream. You read from the stream
 * into this buffer and then scatter the buffer into the RPC.
 *
 * Returns: TRUE if successful; otherwise FALSE and @error is set.
 */
bson_bool_t
_mongoc_buffer_append_from_stream (mongoc_buffer_t *buffer,
                                   mongoc_stream_t *stream,
                                   size_t           size,
                                   bson_int32_t     timeout_msec,
                                   bson_error_t    *error)
{
   bson_uint8_t *buf;
   ssize_t ret;

   bson_return_val_if_fail(buffer, FALSE);
   bson_return_val_if_fail(stream, FALSE);
   bson_return_val_if_fail(size, FALSE);

   BSON_ASSERT(buffer->datalen);

   if (!SPACE_FOR(buffer, size)) {
      if (buffer->len) {
         memmove(&buffer->data[0], &buffer->data[buffer->off], buffer->len);
      }
      buffer->off = 0;
      if (!SPACE_FOR(buffer, size)) {
         buffer->datalen = bson_next_power_of_two(size);
         buffer->data = buffer->realloc_func(buffer->data, buffer->datalen);
      }
   }

   buf = &buffer->data[buffer->off + buffer->len];
   ret = mongoc_stream_read(stream, buf, size, size, timeout_msec);
   if (ret != size) {
      bson_set_error(error,
                     MONGOC_ERROR_STREAM,
                     MONGOC_ERROR_STREAM_SOCKET,
                     "Failed to read %llu bytes from socket.",
                     (unsigned long long)size);
      return FALSE;
   }

   buffer->len += ret;

   return TRUE;
}


/**
 * _mongoc_buffer_fill:
 * @buffer: A mongoc_buffer_t.
 * @stream: A stream to read from.
 * @min_bytes: The minumum number of bytes to read.
 * @error: A location for a bson_error_t or NULL.
 *
 * Attempts to fill the entire buffer, or at least @min_bytes.
 *
 * Returns: The number of buffered bytes, or -1 on failure.
 */
ssize_t
_mongoc_buffer_fill (mongoc_buffer_t *buffer,
                     mongoc_stream_t *stream,
                     size_t           min_bytes,
                     bson_int32_t     timeout_msec,
                     bson_error_t    *error)
{
   ssize_t ret;
   size_t avail_bytes;

   bson_return_val_if_fail(buffer, FALSE);
   bson_return_val_if_fail(stream, FALSE);
   bson_return_val_if_fail(min_bytes >= 0, FALSE);

   BSON_ASSERT(buffer->data);
   BSON_ASSERT(buffer->datalen);

   if (min_bytes <= buffer->len) {
      return buffer->len;
   }

   min_bytes -= buffer->len;

   if (buffer->len) {
      memmove(&buffer->data[0], &buffer->data[buffer->off], buffer->len);
   }

   buffer->off = 0;

   if (!SPACE_FOR(buffer, min_bytes)) {
      buffer->datalen = bson_next_power_of_two(buffer->len + min_bytes);
      buffer->data = bson_realloc(buffer->data, buffer->datalen);
   }

   avail_bytes = buffer->datalen - buffer->len;

   ret = mongoc_stream_read(stream,
                            &buffer->data[buffer->off + buffer->len],
                            avail_bytes, min_bytes, timeout_msec);

   if (ret == -1) {
      return -1;
   }

   buffer->len += ret;

   return (buffer->len < min_bytes) ? -1 : buffer->len;
}
