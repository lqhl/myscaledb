#include "ReadBufferFromRemoteFSGather.h"

#include <IO/SeekableReadBuffer.h>

#include <Disks/IO/CachedOnDiskReadBufferFromFile.h>
#include <Common/logger_useful.h>
#include <iostream>
#include <base/hex.h>
#include <Interpreters/FilesystemCacheLog.h>
#include <Interpreters/Context.h>


namespace DB
{

ReadBufferFromRemoteFSGather::ReadBufferFromRemoteFSGather(
    ReadBufferCreator && read_buffer_creator_,
    const StoredObjects & blobs_to_read_,
    const ReadSettings & settings_)
    : ReadBuffer(nullptr, 0)
    , read_buffer_creator(std::move(read_buffer_creator_))
    , blobs_to_read(blobs_to_read_)
    , settings(settings_)
    , query_id(CurrentThread::isInitialized() && CurrentThread::get().getQueryContext() != nullptr ? CurrentThread::getQueryId() : "")
    , log(&Poco::Logger::get("ReadBufferFromRemoteFSGather"))
    , enable_cache_log(!query_id.empty() && settings.enable_filesystem_cache_log)
{
    if (!blobs_to_read.empty())
        current_object = blobs_to_read.front();

    with_cache = settings.remote_fs_cache
        && settings.enable_filesystem_cache
        && (!query_id.empty() || settings.read_from_filesystem_cache_if_exists_otherwise_bypass_cache);
}

SeekableReadBufferPtr ReadBufferFromRemoteFSGather::createImplementationBuffer(const StoredObject & object)
{
    if (current_buf != nullptr && !with_cache && enable_cache_log)
    {
        appendFilesystemCacheLog();
    }

    current_object = object;
    total_bytes_read_from_current_file = 0;
    const auto & object_path = object.absolute_path;

    size_t current_read_until_position = read_until_position ? read_until_position : object.bytes_size;
    auto current_read_buffer_creator = [=, this]() { return read_buffer_creator(object_path, current_read_until_position); };

    if (with_cache)
    {
        auto cache_key = settings.remote_fs_cache->hash(object_path);
        return std::make_shared<CachedOnDiskReadBufferFromFile>(
            object_path,
            cache_key,
            settings.remote_fs_cache,
            std::move(current_read_buffer_creator),
            settings,
            query_id,
            object.bytes_size,
            /* allow_seeks */false,
            /* use_external_buffer */true,
            read_until_position ? std::optional<size_t>(read_until_position) : std::nullopt);
    }

    return current_read_buffer_creator();
}

void ReadBufferFromRemoteFSGather::appendFilesystemCacheLog()
{
    if (current_object.absolute_path.empty())
        return;
    FilesystemCacheLogElement elem
    {
        .event_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()),
        .query_id = query_id,
        .source_file_path = current_object.absolute_path,
        .file_segment_range = { 0, current_object.bytes_size },
        .cache_type = FilesystemCacheLogElement::CacheType::READ_FROM_FS_BYPASSING_CACHE,
        .file_segment_size = total_bytes_read_from_current_file,
        .read_from_cache_attempted = false,
    };

    if (auto cache_log = Context::getGlobalContextInstance()->getFilesystemCacheLog())
        cache_log->add(elem);
}

IAsynchronousReader::Result ReadBufferFromRemoteFSGather::readInto(char * data, size_t size, size_t offset, size_t ignore)
{
    /**
     * Set `data` to current working and internal buffers.
     * Internal buffer with size `size`. Working buffer with size 0.
     */
    set(data, size);

    file_offset_of_buffer_end = offset;
    bytes_to_ignore = ignore;

    const auto result = nextImpl();

    if (result)
        return { working_buffer.size(), BufferBase::offset(), nullptr };

    return {0, 0, nullptr};
}

void ReadBufferFromRemoteFSGather::initialize()
{
    if (blobs_to_read.empty())
        return;

    /// One clickhouse file can be split into multiple files in remote fs.
    auto current_buf_offset = file_offset_of_buffer_end;
    for (size_t i = 0; i < blobs_to_read.size(); ++i)
    {
        const auto & object = blobs_to_read[i];

        if (object.bytes_size > current_buf_offset)
        {
            /// Do not create a new buffer if we already have what we need.
            if (!current_buf || current_buf_idx != i)
            {
                current_buf_idx = i;
                current_buf = createImplementationBuffer(object);
            }

            current_buf->seek(current_buf_offset, SEEK_SET);
            return;
        }

        current_buf_offset -= object.bytes_size;
    }
    current_buf_idx = blobs_to_read.size();
    current_buf = nullptr;
}

bool ReadBufferFromRemoteFSGather::nextImpl()
{
    /// Find first available buffer that fits to given offset.
    if (!current_buf)
        initialize();

    if (!current_buf)
        return false;

    if (readImpl())
        return true;

    if (!moveToNextBuffer())
        return false;

    return readImpl();
}

bool ReadBufferFromRemoteFSGather::moveToNextBuffer()
{
    /// If there is no available buffers - nothing to read.
    if (current_buf_idx + 1 >= blobs_to_read.size())
        return false;

    ++current_buf_idx;

    const auto & object = blobs_to_read[current_buf_idx];
    current_buf = createImplementationBuffer(object);

    return true;
}

bool ReadBufferFromRemoteFSGather::readImpl()
{
    swap(*current_buf);

    bool result = false;

    /**
     * Lazy seek is performed here.
     * In asynchronous buffer when seeking to offset in range [pos, pos + min_bytes_for_seek]
     * we save how many bytes need to be ignored (new_offset - position() bytes).
     */
    if (bytes_to_ignore)
    {
        total_bytes_read_from_current_file += bytes_to_ignore;
        current_buf->ignore(bytes_to_ignore);
        result = current_buf->hasPendingData();
        file_offset_of_buffer_end += bytes_to_ignore;
        bytes_to_ignore = 0;
    }

    if (!result)
        result = current_buf->next();

    if (blobs_to_read.size() == 1)
    {
        file_offset_of_buffer_end = current_buf->getFileOffsetOfBufferEnd();
    }
    else
    {
        /// For log family engines there are multiple s3 files for the same clickhouse file
        file_offset_of_buffer_end += current_buf->available();
    }

    swap(*current_buf);

    /// Required for non-async reads.
    if (result)
    {
        assert(available());
        nextimpl_working_buffer_offset = offset();
        total_bytes_read_from_current_file += available();
    }

    return result;
}

size_t ReadBufferFromRemoteFSGather::getFileOffsetOfBufferEnd() const
{
    return file_offset_of_buffer_end;
}

void ReadBufferFromRemoteFSGather::setReadUntilPosition(size_t position)
{
    if (position != read_until_position)
    {
        read_until_position = position;
        reset();
    }
}

void ReadBufferFromRemoteFSGather::reset()
{
    current_buf.reset();
}

String ReadBufferFromRemoteFSGather::getFileName() const
{
    return current_object.absolute_path;
}

size_t ReadBufferFromRemoteFSGather::getFileSize() const
{
    size_t size = 0;
    for (const auto & object : blobs_to_read)
        size += object.bytes_size;
    return size;
}

String ReadBufferFromRemoteFSGather::getInfoForLog()
{
    if (!current_buf)
        return "";

    return current_buf->getInfoForLog();
}

size_t ReadBufferFromRemoteFSGather::getImplementationBufferOffset() const
{
    if (!current_buf)
        return file_offset_of_buffer_end;

    return current_buf->getFileOffsetOfBufferEnd();
}

ReadBufferFromRemoteFSGather::~ReadBufferFromRemoteFSGather()
{
    if (!with_cache && enable_cache_log)
    {
        appendFilesystemCacheLog();
    }
}

}
