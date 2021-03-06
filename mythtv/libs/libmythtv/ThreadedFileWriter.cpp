// ANSI C headers
#include <cstdio>
#include <cstdlib>
#include <cerrno>

// Unix C headers
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <string.h>

// Qt headers
#include <QString>
#include <QThread>

// MythTV headers
#include "ThreadedFileWriter.h"
#include "mythverbose.h"
#include "mythlogging.h"

#include "mythtimer.h"
#include "compat.h"

/// \brief Runs ThreadedFileWriter::DiskLoop(void)
void TFWWriteThread::run(void)
{
    threadRegister("TFWWrite");
#ifndef USING_MINGW
    // don't exit program if file gets larger than quota limit..
    signal(SIGXFSZ, SIG_IGN);
#endif
    m_parent->DiskLoop();
    threadDeregister();
}

/// \brief Runs ThreadedFileWriter::SyncLoop(void)
void TFWSyncThread::run(void)
{
    threadRegister("TFWSync");
    m_parent->SyncLoop();
    threadDeregister();
}

const uint ThreadedFileWriter::kMaxBufferSize = 128 * 1024 * 1024;
const uint ThreadedFileWriter::kMinWriteSize = 64 * 1024;

/** \class ThreadedFileWriter
 *  \brief This class supports the writing of recordings to disk.
 *
 *   This class allows us manage the buffering when writing to
 *   disk. We write to the kernel image of the disk using one
 *   thread, and sync the kernel's image of the disk to hardware
 *   using another thread. The goal here so to block as little as
 *   possible when the classes using this class want to add data
 *   to the stream.
 */

#define LOC QString("TFW(%1:%2): ").arg(filename).arg(fd)
#define LOC_WARN QString("TFW(%1:%2), Warning: ").arg(filename).arg(fd)
#define LOC_ERR QString("TFW(%1:%2), Error: ").arg(filename).arg(fd)

/** \fn ThreadedFileWriter::ThreadedFileWriter(const QString&,int,mode_t)
 *  \brief Creates a threaded file writer.
 */
ThreadedFileWriter::ThreadedFileWriter(const QString &fname,
                                       int pflags, mode_t pmode) :
    // file stuff
    filename(fname),                     flags(pflags),
    mode(pmode),                         fd(-1),
    // state
    flush(false),                        in_dtor(false),
    ignore_writes(false),                tfw_min_write_size(kMinWriteSize),
    totalBufferUse(0),
    // threads
    writeThread(NULL),                   syncThread(NULL)
{
    filename.detach();
}

/** \fn ThreadedFileWriter::Open(void)
 *  \brief Opens the file we will be writing to.
 *  \return true if we successfully open the file.
 */
bool ThreadedFileWriter::Open(void)
{
    ignore_writes = false;

    if (filename == "-")
        fd = fileno(stdout);
    else
    {
        QByteArray fname = filename.toLocal8Bit();
        fd = open(fname.constData(), flags, mode);
    }

    if (fd < 0)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR +
                QString("Opening file '%1'.").arg(filename) + ENO);
        return false;
    }
    else
    {
        VERBOSE(VB_FILE, LOC + "Open() successful");

#ifdef USING_MINGW
        _setmode(fd, _O_BINARY);
#endif
        writeThread = new TFWWriteThread(this);
        writeThread->start();
        syncThread = new TFWSyncThread(this);
        syncThread->start();

        return true;
    }
}

/** \fn ThreadedFileWriter::~ThreadedFileWriter()
 *  \brief Commits all writes and closes the file.
 */
ThreadedFileWriter::~ThreadedFileWriter()
{
    Flush();

    {  /* tell child threads to exit */
        QMutexLocker locker(&buflock);
        in_dtor = true;
        bufferSyncWait.wakeAll();
        bufferHasData.wakeAll();
    }

    if (writeThread)
    {
        writeThread->wait();
        delete writeThread;
        writeThread = NULL;
    }

    while (!writeBuffers.empty())
    {
        delete writeBuffers.front();
        writeBuffers.pop_front();
    }

    while (!emptyBuffers.empty())
    {
        delete emptyBuffers.front();
        emptyBuffers.pop_front();
    }

    if (syncThread)
    {
        syncThread->wait();
        delete syncThread;
        syncThread = NULL;
    }

    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
}

/** \fn ThreadedFileWriter::Write(const void*, uint)
 *  \brief Writes data to the end of the write buffer
 *
 *  \param data  pointer to data to write to disk
 *  \param count size of data in bytes
 */
uint ThreadedFileWriter::Write(const void *data, uint count)
{
    if (count == 0)
        return 0;

    QMutexLocker locker(&buflock);

    if (ignore_writes)
        return count;

    if (totalBufferUse + count > kMaxBufferSize)
    {
        VERBOSE(VB_IMPORTANT, LOC_ERR + "Maximum buffer size exceeded."
                "\n\t\t\tfile will be truncated, no further writing "
                "will be done."
                "\n\t\t\tThis generally indicates your disk performance "
                "\n\t\t\tis insufficient to deal with the number of on-going "
                "\n\t\t\trecordings, or you have a disk failure.");
        ignore_writes = true;
        return count;
    }

    TFWBuffer *buf = NULL;

    if (!writeBuffers.empty() &&
        (writeBuffers.back()->data.size() + count) < kMinWriteSize)
    {
        buf = writeBuffers.back();
        writeBuffers.pop_back();
    }
    else
    {
        if (!emptyBuffers.empty())
        {
            buf = emptyBuffers.front();
            emptyBuffers.pop_front();
            buf->data.clear();
        }
        else
        {
            buf = new TFWBuffer();
        }
    }

    totalBufferUse += count;
    const char *cdata = (const char*) data;
    buf->data.insert(buf->data.end(), cdata, cdata+count);
    buf->lastUsed = QDateTime::currentDateTime();

    writeBuffers.push_back(buf);

    bufferHasData.wakeAll();

    VERBOSE(VB_FILE|VB_EXTRA,
            LOC + QString("Write(*, %1) total %2 cnt %3")
            .arg(count,4).arg(totalBufferUse).arg(writeBuffers.size()));

    return count;
}

/** \fn ThreadedFileWriter::Seek(long long pos, int whence)
 *  \brief Seek to a position within stream; May be unsafe.
 *
 *   This method is unsafe if Start() has been called and
 *   the call us not preceeded by StopReads(). You probably
 *   want to follow Seek() with a StartReads() in this case.
 *
 *   This method assumes that we don't seek very often. It does
 *   not use a high performance approach... we just block until
 *   the write thread empties the buffer.
 */
long long ThreadedFileWriter::Seek(long long pos, int whence)
{
    QMutexLocker locker(&buflock);
    flush = true;
    while (!writeBuffers.empty())
    {
        bufferHasData.wakeAll();
        if (!bufferEmpty.wait(locker.mutex(), 2000))
        {
            VERBOSE(VB_IMPORTANT, LOC +
                    QString("Taking a long time to flush.. buffer size %1")
                    .arg(totalBufferUse));
        }
    }
    flush = false;
    return lseek(fd, pos, whence);
}

/** \fn ThreadedFileWriter::Flush(void)
 *  \brief Allow DiskLoop() to flush buffer completely ignoring low watermark.
 */
void ThreadedFileWriter::Flush(void)
{
    QMutexLocker locker(&buflock);
    flush = true;
    while (!writeBuffers.empty())
    {
        bufferHasData.wakeAll();
        if (!bufferEmpty.wait(locker.mutex(), 2000))
        {
            VERBOSE(VB_IMPORTANT, LOC +
                    QString("Taking a long time to flush.. buffer size %1")
                    .arg(totalBufferUse));
        }
    }
    flush = false;
}

/** \brief Flush data written to the file descriptor to disk.
 *
 *  This prevents freezing up Linux disk access on a running
 *  CFQ, AS, or Deadline as the disk write schedulers. It does
 *  this via two mechanism. One is a data sync using the best
 *  mechanism available (fdatasync then fsync). The second is
 *  by telling the kernel we do not intend to use the data just
 *  written anytime soon so other processes time-slices will
 *  not be used to deal with our excess dirty pages.
 *
 *  \note We used to also use sync_file_range on Linux, however
 *  this is incompatible with newer filesystems such as BRTFS and
 *  does not actually sync any blocks that have not been allocated
 *  yet so it was never really appropriate for ThreadedFileWriter.
 *
 *  \note We use standard posix calls for this, so any operating
 *  system supporting the calls will benefit, but this has been
 *  designed with Linux in mind. Other OS's may benefit from
 *  revisiting this function.
 */
void ThreadedFileWriter::Sync(void)
{
    if (fd >= 0)
    {
#if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
        // fdatasync tries to avoid updating metadata, but will in
        // practice always update metadata if any data is written
        // as the file will usually have grown.
        fdatasync(fd);
#else
        fsync(fd);
#endif
    }
}

/** \fn ThreadedFileWriter::SetWriteBufferMinWriteSize(uint)
 *  \brief Sets the minumum number of bytes to write to disk in a single write.
 *         This is ignored during a Flush(void)
 */
void ThreadedFileWriter::SetWriteBufferMinWriteSize(uint newMinSize)
{
    QMutexLocker locker(&buflock);
    if (newMinSize > 0)
        tfw_min_write_size = newMinSize;
    bufferHasData.wakeAll();
}

/** \fn ThreadedFileWriter::SyncLoop(void)
 *  \brief The thread run method that calls Sync(void).
 */
void ThreadedFileWriter::SyncLoop(void)
{
    QMutexLocker locker(&buflock);
    while (!in_dtor)
    {
        locker.unlock();

        Sync();

        locker.relock();
        bufferSyncWait.wait(&buflock, 1000);
    }
}

/** \fn ThreadedFileWriter::DiskLoop(void)
 *  \brief The thread run method that actually calls writes to disk.
 */
void ThreadedFileWriter::DiskLoop(void)
{
    QMutexLocker locker(&buflock);

    // Even if the bytes buffered is less than the minimum write
    // size we do want to write to the OS buffers periodically.
    // This timer makes sure we do.
    MythTimer minWriteTimer;
    minWriteTimer.start();

    while (!in_dtor)
    {
        if (ignore_writes)
        {
            while (!writeBuffers.empty())
            {
                delete writeBuffers.front();
                writeBuffers.pop_front();
            }
            while (!emptyBuffers.empty())
            {
                delete emptyBuffers.front();
                emptyBuffers.pop_front();
            }
            bufferEmpty.wakeAll();
            bufferHasData.wait(locker.mutex());
            continue;
        }

        if (writeBuffers.empty())
        {
            bufferEmpty.wakeAll();
            bufferHasData.wait(locker.mutex(), 1000);
            TrimEmptyBuffers();
            continue;
        }

        int mwte = minWriteTimer.elapsed();
        if (!flush && (mwte < 250) && (totalBufferUse < kMinWriteSize))
        {
            bufferHasData.wait(locker.mutex(), 250 - mwte);
            TrimEmptyBuffers();
            continue;
        }

        TFWBuffer *buf = writeBuffers.front();
        writeBuffers.pop_front();
        totalBufferUse -= buf->data.size();
        minWriteTimer.start();

        //////////////////////////////////////////

        const void *data = &(buf->data[0]);
        uint sz = buf->data.size();

        bool write_ok = true;
        uint tot = 0;
        uint errcnt = 0;

        VERBOSE(VB_FILE|VB_EXTRA, LOC + QString("write(%1) cnt %2 total %3")
                .arg(sz).arg(writeBuffers.size())
                .arg(totalBufferUse));

        MythTimer writeTimer;
        writeTimer.start();

        while ((tot < sz) && !in_dtor)
        {
            locker.unlock();

            int ret = write(fd, (char *)data + tot, sz - tot);

            if (ret < 0)
            {
                if (EAGAIN == errno)
                {
                    VERBOSE(VB_IMPORTANT, LOC + "Got EAGAIN.");
                }
                else
                {
                    errcnt++;
                    VERBOSE(VB_IMPORTANT, LOC_ERR + "File I/O " +
                            QString(" errcnt: %1").arg(errcnt) + ENO);
                }

                if ((errcnt >= 3) || (ENOSPC == errno) || (EFBIG == errno))
                {
                    locker.relock();
                    write_ok = false;
                    break;
                }
            }
            else
            {
                tot += ret;
            }

            locker.relock();

            if (!in_dtor)
                bufferHasData.wait(locker.mutex(), 50);
        }

        //////////////////////////////////////////

        buf->lastUsed = QDateTime::currentDateTime();
        emptyBuffers.push_back(buf);

        if (writeTimer.elapsed() > 1000)
        {
            VERBOSE(VB_IMPORTANT, LOC_WARN +
                    QString("write(%1) cnt %2 total %3 -- "
                            "took a long time, %4 ms")
                    .arg(sz).arg(writeBuffers.size())
                    .arg(totalBufferUse).arg(writeTimer.elapsed()));
        }

        if (!write_ok && ((EFBIG == errno) || (ENOSPC == errno)))
        {
            QString msg;
            switch (errno)
            {
                case EFBIG:
                    msg =
                        "Maximum file size exceeded by '%1'"
                        "\n\t\t\t"
                        "You must either change the process ulimits, configure"
                        "\n\t\t\t"
                        "your operating system with \"Large File\" support, "
                        "or use"
                        "\n\t\t\t"
                        "a filesystem which supports 64-bit or 128-bit files."
                        "\n\t\t\t"
                        "HINT: FAT32 is a 32-bit filesystem.";
                    break;
                case ENOSPC:
                    msg =
                        "No space left on the device for file '%1'"
                        "\n\t\t\t"
                        "file will be truncated, no further writing "
                        "will be done.";
                    break;
            }

            VERBOSE(VB_IMPORTANT, LOC_ERR + msg.arg(filename));
            ignore_writes = true;
        }
    }
}

void ThreadedFileWriter::TrimEmptyBuffers(void)
{
    QDateTime cur = QDateTime::currentDateTime();
    QDateTime cur_m_60 = cur.addSecs(-60);

    QList<TFWBuffer*>::iterator it = emptyBuffers.begin();
    while (it != emptyBuffers.end())
    {
        if (((*it)->lastUsed < cur_m_60) ||
            ((*it)->data.capacity() > 3 * (*it)->data.size() &&
             (*it)->data.capacity() > 64 * 1024))
        {
            delete *it;
            it = emptyBuffers.erase(it);
            continue;
        }
        ++it;
    }
}
