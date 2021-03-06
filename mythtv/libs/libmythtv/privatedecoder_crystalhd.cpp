#include "privatedecoder_crystalhd.h"
#include "myth_imgconvert.h"
#include "mythlogging.h"

#define LOC  QString("CrystalHD: ")
#define ERR  QString("CrystalHD Err: ")
#define WARN QString("CrystalHD Warn: ")

void FetcherThread::run(void)
{
    if (!m_dec)
        return;

    threadRegister("Fetcher");
    VERBOSE(VB_PLAYBACK, LOC + QString("Starting Fetcher thread."));
    m_dec->FetchFrames();
    VERBOSE(VB_PLAYBACK, LOC + QString("Stopping Fetcher thread."));
    threadDeregister();
}

PixelFormat bcmpixfmt_to_pixfmt(BC_OUTPUT_FORMAT fmt);
QString device_to_string(BC_DEVICE_TYPE device);
QString bcmerr_to_string(BC_STATUS err);
QString bcmpixfmt_to_string(BC_OUTPUT_FORMAT fmt);
QString pulldown_to_string(int pulldown);
QString decoderflags_to_string(int flags);
QString poutflags_to_string(int flags);

#define INIT_ST BC_STATUS st; bool ok = true;
#define CHECK_ST \
    ok &= (st == BC_STS_SUCCESS); \
    if (!ok) \
        VERBOSE(VB_IMPORTANT, ERR + QString("Error at %1:%2 (#%3, %4)") \
              .arg(__FILE__).arg( __LINE__).arg(st) \
              .arg(bcmerr_to_string(st)));

void PrivateDecoderCrystalHD::GetDecoders(render_opts &opts)
{
    opts.decoders->append("crystalhd");
    (*opts.equiv_decoders)["crystalhd"].append("nuppel");
    (*opts.equiv_decoders)["crystalhd"].append("ffmpeg");
    (*opts.equiv_decoders)["crystalhd"].append("dummy");
}

PrivateDecoderCrystalHD::PrivateDecoderCrystalHD()
  : m_device(NULL), m_device_type(BC_70012),
    m_pix_fmt(OUTPUT_MODE_INVALID), m_decoded_frames_lock(QMutex::Recursive),
    m_fetcher_thread(NULL), m_fetcher_pause(false), m_fetcher_paused(false),
    m_fetcher_stop(false),  m_frame(NULL), m_filter(NULL)
{
}

PrivateDecoderCrystalHD::~PrivateDecoderCrystalHD()
{
    if (m_fetcher_thread)
    {
        m_fetcher_pause = true;
        m_fetcher_stop = true;
        int tries = 0;
        while (!m_fetcher_thread->wait(100) && (tries++ < 50))
            VERBOSE(VB_PLAYBACK, WARN + "Waited 100ms for Fetcher to stop");

        if (m_fetcher_thread->isRunning())
            VERBOSE(VB_IMPORTANT, ERR + "Failed to stop Fetcher.");
        else
            VERBOSE(VB_PLAYBACK, LOC + "Stopped frame Fetcher.");
        delete m_fetcher_thread;
    }

    if (m_filter)
        av_bitstream_filter_close(m_filter);

    Reset();
    if (!m_device)
        return;

    INIT_ST
    if (m_device_type != BC_70015)
    {
        st = DtsFlushRxCapture(m_device, false);
        CHECK_ST
    }
    st = DtsStopDecoder(m_device);
    CHECK_ST
    st = DtsCloseDecoder(m_device);
    CHECK_ST
    DtsDeviceClose(m_device);
}

bool PrivateDecoderCrystalHD::Init(const QString &decoder,
                                   bool no_hardware_decode,
                                   AVCodecContext *avctx)
{
    if ((decoder != "crystalhd") || no_hardware_decode || !avctx)
        return false;
    if (getenv("NO_CRYSTALHD"))
        return false;

    static bool debugged = false;

    uint32_t well_documented = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW |
                               DTS_SKIP_TX_CHK_CPB |
                               DTS_PLAYBACK_DROP_RPT_MODE |
                               DTS_DFLT_RESOLUTION(vdecRESOLUTION_CUSTOM);
    INIT_ST
    st = DtsDeviceOpen(&m_device, well_documented);
    CHECK_ST
    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, ERR + "Failed to open CrystalHD device");
        return false;
    }

    _BC_INFO_CRYSTAL_ info;
    st = DtsCrystalHDVersion(m_device, &info);
    CHECK_ST
    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, ERR + "Failed to get device info.");
        return false;
    }

    m_device_type = (BC_DEVICE_TYPE)info.device;

    if (!debugged)
    {
        VERBOSE(VB_IMPORTANT, LOC + QString("Device: %1")
                .arg(device_to_string(m_device_type)));
        VERBOSE(VB_IMPORTANT, LOC + QString("Library : %1.%2.%3")
                .arg(info.dilVersion.dilMajor)
                .arg(info.dilVersion.dilMinor)
                .arg(info.dilVersion.version));
        VERBOSE(VB_IMPORTANT, LOC + QString("Driver  : %1.%2.%3")
                .arg(info.drvVersion.drvMajor)
                .arg(info.drvVersion.drvMinor)
                .arg(info.drvVersion.version));
        VERBOSE(VB_IMPORTANT, LOC + QString("Firmware: %1.%2.%3")
                .arg(info.fwVersion.fwMajor)
                .arg(info.fwVersion.fwMinor)
                .arg(info.fwVersion.version));
    }

    if (BC_70012 == m_device_type)
    {
        VERBOSE(VB_IMPORTANT, LOC + "BCM70012 device is currently unsupported.");
        return false;
    }

    BC_HW_CAPS hw_caps;
    uint32_t codecs;
    st = DtsGetCapabilities(m_device, &hw_caps);
    CHECK_ST
    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, ERR + "Failed to get device capabilities");
        return false;
    }

    BC_OUTPUT_FORMAT m_desired_fmt = (m_device_type == BC_70015) ?
                                     OUTPUT_MODE422_YUY2 : OUTPUT_MODE420;
    m_pix_fmt = OUTPUT_MODE_INVALID;
    for (int i = 0; i < hw_caps.ColorCaps.Count; i++)
    {
        if (m_desired_fmt == hw_caps.ColorCaps.OutFmt[i])
            m_pix_fmt = m_desired_fmt;
        if (!debugged)
        {
            VERBOSE(VB_PLAYBACK, LOC +
                    QString("Supported output format: %1")
                    .arg(bcmpixfmt_to_string(hw_caps.ColorCaps.OutFmt[i])));
        }
    }
    if (m_pix_fmt != m_desired_fmt)
    {
        VERBOSE(VB_PLAYBACK, ERR + "Failed to find correct output format.");
        return false;
    }
    VERBOSE(VB_PLAYBACK, LOC + QString("Using: %1")
            .arg(bcmpixfmt_to_string(m_pix_fmt)));

    codecs = hw_caps.DecCaps;
    if (!debugged)
    {
        VERBOSE(VB_PLAYBACK, LOC + QString("H.264 support: %1")
                .arg((bool)(codecs & BC_DEC_FLAGS_H264)));
        VERBOSE(VB_PLAYBACK, LOC + QString("MPEG2 support: %1")
                .arg((bool)(codecs & BC_DEC_FLAGS_MPEG2)));
        VERBOSE(VB_PLAYBACK, LOC + QString("VC1   support: %1")
                .arg((bool)(codecs & BC_DEC_FLAGS_VC1)));
        VERBOSE(VB_PLAYBACK, LOC + QString("MPEG4 support: %1")
                .arg((bool)(codecs & BC_DEC_FLAGS_M4P2)));
        debugged = true;
    }

    BC_MEDIA_SUBTYPE sub_type = BC_MSUBTYPE_INVALID;

    switch (avctx->codec_id)
    {
        case CODEC_ID_MPEG4:
            if (codecs & BC_DEC_FLAGS_M4P2)
                sub_type = BC_MSUBTYPE_DIVX;
            break;
        case CODEC_ID_MPEG1VIDEO:
            if (codecs & BC_DEC_FLAGS_MPEG2)
                sub_type = BC_MSUBTYPE_MPEG1VIDEO;
            break;
        case CODEC_ID_MPEG2VIDEO:
            if (codecs & BC_DEC_FLAGS_MPEG2)
                sub_type = BC_MSUBTYPE_MPEG2VIDEO;
            break;
        case CODEC_ID_VC1:
            if (codecs & BC_DEC_FLAGS_VC1)
            {
                if (avctx->codec_tag == MKTAG('W','V','C','1'))
                    sub_type = BC_MSUBTYPE_WVC1;
                else
                    sub_type = BC_MSUBTYPE_VC1;
            }
            break;
        case CODEC_ID_WMV3:
            if (codecs & BC_DEC_FLAGS_VC1)
                sub_type = BC_MSUBTYPE_WMV3;
            break;
        case CODEC_ID_H264:
            if (codecs & BC_DEC_FLAGS_H264)
            {
                if (avctx->extradata[0] == 0x01)
                {
                    if (!CreateFilter(avctx))
                    {
                        VERBOSE(VB_PLAYBACK, ERR +
                                "Failed to create stream filter");
                        return false;
                    }
                    sub_type = BC_MSUBTYPE_AVC1;
                }
                else
                    sub_type = BC_MSUBTYPE_H264;
            }
            break;
    }

    if (sub_type == BC_MSUBTYPE_INVALID)
    {
        VERBOSE(VB_PLAYBACK, ERR + QString("Codec %1 not supported")
                .arg(ff_codec_id_string(avctx->codec_id)));
        return false;
    }

    int nalsize = 4;
    if (avctx->codec_id == CODEC_ID_H264)
    {
        VERBOSE(VB_PLAYBACK, LOC +
                QString("H.264 Profile: %1 RefFrames: %2 Codec tag: %3")
                .arg(avctx->profile).arg(avctx->refs)
                .arg(fourcc_str(avctx->codec_tag)));
        if (avctx->extradata[0] == 1)
        {
            nalsize = (avctx->extradata[4] & 0x03) + 1;
            VERBOSE(VB_PLAYBACK, LOC + QString("avcC nal size: %1")
                    .arg(nalsize));
        }
    }

    BC_INPUT_FORMAT fmt;
    memset(&fmt, 0, sizeof(BC_INPUT_FORMAT));
    fmt.OptFlags       = 0x80000000 | vdecFrameRateUnknown;
    fmt.width          = avctx->coded_width;
    fmt.height         = avctx->coded_height;
    fmt.Progressive    = 1;
    fmt.FGTEnable      = 0;
    fmt.MetaDataEnable = 0;
    fmt.metaDataSz     = avctx->extradata_size;
    fmt.pMetaData      = avctx->extradata;
    fmt.startCodeSz    = nalsize;
    fmt.mSubtype       = sub_type;

    st = DtsSetInputFormat(m_device, &fmt);
    CHECK_ST
    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, ERR + "Failed to set decoder input format");
        return false;
    }

    st = DtsOpenDecoder(m_device, BC_STREAM_TYPE_ES);
    CHECK_ST
    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, ERR + "Failed to open CrystalHD decoder");
        return false;
    }

    st = DtsSetColorSpace(m_device, m_pix_fmt);
    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, ERR + "Failed to set decoder output format");
        return false;
    }

    st = DtsStartDecoder(m_device);
    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, ERR + "Failed to start decoder");
        return false;
    }

    st = DtsStartCapture(m_device);
    if (!ok)
    {
        VERBOSE(VB_IMPORTANT, ERR + "Failed to start capture");
        return false;
    }

    Reset();

    VERBOSE(VB_PLAYBACK, LOC + QString("Created decoder %1 %2x%3")
        .arg(ff_codec_id_string(avctx->codec_id))
        .arg(avctx->coded_width).arg(avctx->coded_height));
    return true;
}

bool PrivateDecoderCrystalHD::CreateFilter(AVCodecContext *avctx)
{
    int nalsize = (avctx->extradata[4] & 0x3) + 1;
    if (!nalsize || nalsize == 3 || nalsize > 4)
    {
        VERBOSE(VB_PLAYBACK, ERR + QString("Invalid nal size (%1)")
                .arg(nalsize));
        return false;
    }

    static const uint8_t testnal[] = { 0,0,0,2,0,0 };
    AVBitStreamFilterContext *bsfc =
            av_bitstream_filter_init("h264_mp4toannexb");
    if (!bsfc)
        return false;
    m_filter = bsfc;

    // and test extradata
    const uint8_t *test = testnal;
    int testsize  = 6;
    int outbuf_size = 0;
    uint8_t *outbuf = NULL;
    int res = av_bitstream_filter_filter(m_filter, avctx, NULL, &outbuf,
                                         &outbuf_size, test, testsize, 0);
    delete outbuf;
    return res > 0;
}

void inline free_frame(VideoFrame* frame)
{
    if (frame)
    {
        if (frame->buf)
            delete [] frame->buf;
        if (frame->priv[0])
            delete [] frame->priv[0];
        delete frame;
    }
}

void inline free_buffer(PacketBuffer* buffer)
{
    if (buffer)
    {
        if (buffer->buf)
            delete [] buffer->buf;
        delete buffer;
    }
}

bool PrivateDecoderCrystalHD::Reset(void)
{
    if (m_fetcher_thread)
    {
        m_fetcher_pause = true;
        int tries = 0;
        while (!m_fetcher_paused && (tries++ < 50))
            usleep(10000);
        if (!m_fetcher_paused)
            VERBOSE(VB_IMPORTANT, LOC + "Failed to pause fetcher thread");
    }

    QMutexLocker lock(&m_decoded_frames_lock);
    free_frame(m_frame);
    m_frame = NULL;

    for (int i = 0; i < m_decoded_frames.size(); i++)
        free_frame(m_decoded_frames[i]);
    m_decoded_frames.clear();

    for (int i = 0; i < m_packet_buffers.size(); i++)
        free_buffer(m_packet_buffers[i]);
    m_packet_buffers.clear();

    if (!m_device)
        return true;

    if (m_device_type != BC_70015)
    {
        INIT_ST
        st = DtsFlushInput(m_device, 2);
        CHECK_ST
    }
    return true;;
}

bool PrivateDecoderCrystalHD::HasBufferedFrames(void)
{
    m_decoded_frames_lock.lock();
    bool result = m_decoded_frames.size() > 0;
    m_decoded_frames_lock.unlock();
    return result;
}

int PrivateDecoderCrystalHD::ProcessPacket(AVStream *stream, AVPacket *pkt)
{
    int result = -1;
    AVCodecContext *avctx = stream->codec;
    if (!avctx)
        return result;

    PacketBuffer *buffer = new PacketBuffer();
    if (!buffer)
        return result;

    buffer->buf  = new unsigned char[pkt->size];
    buffer->size = pkt->size;
    buffer->pts  = pkt->pts;
    memcpy(buffer->buf, pkt->data, pkt->size);

    m_packet_buffers.insert(0, buffer);
    VERBOSE(VB_PLAYBACK|VB_EXTRA, LOC +
            QString("%1 packet buffers queued up").arg(m_packet_buffers.size()));

    while (m_packet_buffers.size() > 0)
    {

        PacketBuffer *buffer = m_packet_buffers.last();
        if (GetTxFreeSize(0) < buffer->size)
        {
            usleep(10000);
            return 0;
        }

        buffer = m_packet_buffers.takeLast();
        uint8_t* buf    = buffer->buf;
        int size        = buffer->size;
        bool free_buf   = false;
        int outbuf_size = 0;
        uint8_t *outbuf = NULL;

        if (m_filter)
        {
            int res = av_bitstream_filter_filter(m_filter, avctx, NULL, &outbuf,
                                                 &outbuf_size, buf, size, 0);
            if (res <= 0)
            {
                static int count = 0;
                if (count == 0)
                    VERBOSE(VB_IMPORTANT, ERR +
                            QString("Failed to convert packet (%1)").arg(res));
                count++;
                if (count > 200)
                    count = 0;
            }

            if (outbuf && (outbuf_size > 0))
            {
                free_buf = outbuf != buf;
                size = outbuf_size;
                buf  = outbuf;
            }
        }

        usleep(1000);
        uint64_t chd_timestamp = 0; // 100 nsec units
        if (buffer->pts != (int64_t)AV_NOPTS_VALUE) 
            chd_timestamp = (uint64_t)(av_q2d(stream->time_base) * buffer->pts * 10000000); 
        VERBOSE(VB_TIMESTAMP|VB_EXTRA, LOC + QString("decoder input timecode %1 ms (pts %2)")
                .arg(chd_timestamp / 10000).arg(buffer->pts));

        // TODO check for busy state
        INIT_ST
        st = DtsProcInput(m_device, buf, size, chd_timestamp, false);
        CHECK_ST

        if (free_buf)
            delete buf;

        free_buffer(buffer);
        if (!ok)
            VERBOSE(VB_IMPORTANT, ERR + "Failed to send packet to decoder.");
        result = buffer->size;
    }
    return result;
}

int PrivateDecoderCrystalHD::GetFrame(AVStream *stream,
                                      AVFrame *picture,
                                      int *got_picture_ptr,
                                      AVPacket *pkt)
{
    int result = -1;
    if (!stream || !m_device || !picture)
        return result;

    AVCodecContext *avctx = stream->codec;
    if (!avctx || !StartFetcherThread())
        return result;

    if (pkt && pkt->size)
    {
        result = ProcessPacket(stream, pkt);
        if (result < 0)
            return result;
    }

    m_decoded_frames_lock.lock();
    int available = m_decoded_frames.size();
    m_decoded_frames_lock.unlock();
    if (!available)
        return result;

    if (avctx->get_buffer(avctx, picture) < 0)
    {
        VERBOSE(VB_IMPORTANT, ERR +
                QString("%1 decoded frames available but no video buffers.")
                .arg(available));
        return -1;
    }

    m_decoded_frames_lock.lock();
    VideoFrame *frame = m_decoded_frames.takeLast();
    m_decoded_frames_lock.unlock();

    *got_picture_ptr = 1;
    picture->reordered_opaque = (int64_t)(frame->timecode / av_q2d(stream->time_base)
                                                          / 10000000);
    VERBOSE(VB_TIMESTAMP|VB_EXTRA, LOC + QString("decoder output timecode %1 ms (pts %2)")
            .arg(frame->timecode / 10000).arg(picture->reordered_opaque));
    picture->interlaced_frame = frame->interlaced_frame;
    picture->top_field_first  = frame->top_field_first;
    picture->repeat_pict      = frame->repeat_pict;
    copy((VideoFrame*)picture->opaque, frame);
    if (frame->priv[0] && frame->qstride)
    {
        memcpy(picture->atsc_cc_buf, frame->priv[0], frame->qstride);
        picture->atsc_cc_len = frame->qstride;
    }
    free_frame(frame);
    return result;
}

void PrivateDecoderCrystalHD::FetchFrames(void)
{
    INIT_ST
    bool valid = false;
    m_fetcher_paused = false;
    while (!m_fetcher_stop)
    {
        usleep(1000);
        if (m_fetcher_pause)
        {
            m_fetcher_paused = true;
            continue;
        }
        m_fetcher_paused = false;

        BC_DTS_STATUS status;
        st = DtsGetDriverStatus(m_device, &status);
        CHECK_ST

        if (!status.ReadyListCount)
            continue;

        BC_DTS_PROC_OUT out;
        memset(&out, 0, sizeof(BC_DTS_PROC_OUT));
        st = DtsProcOutputNoCopy(m_device, valid ? 2000 : 20, &out);

        if (BC_STS_FMT_CHANGE == st)
        {
            VERBOSE(VB_IMPORTANT, LOC + "Decoder reported format change.");
            CheckProcOutput(&out);
            valid = true;
            continue;
        }
        CHECK_ST

        if (!ok)
        {
            VERBOSE(VB_IMPORTANT, ERR + "Failed to fetch decoded frame");
            continue;
        }

        if (ok && valid && (out.PoutFlags & BC_POUT_FLAGS_PIB_VALID))
            FillFrame(&out);
        st = DtsReleaseOutputBuffs(m_device, NULL, false);
        CHECK_ST
    }
}

bool PrivateDecoderCrystalHD::StartFetcherThread(void)
{
    m_fetcher_pause = false;
    if (m_fetcher_thread)
        return true;

    m_fetcher_thread = new FetcherThread(this);
    if (!m_fetcher_thread)
        return false;

    m_fetcher_thread->start();
    return true;
}


void PrivateDecoderCrystalHD::FillFrame(BC_DTS_PROC_OUT *out)
{
    bool second_field = false;
    if (m_frame)
    {
        if (out->PicInfo.picture_number != m_frame->frameNumber)
        {
            VERBOSE(VB_PLAYBACK, WARN + "Missing second field");
            AddFrameToQueue();
        }
        else
        {
            second_field = true;
        }
    }

    int in_width   = out->PicInfo.width;
    int in_height  = out->PicInfo.height;
    int out_width  = (in_width + 15) & (~0xf);
    int out_height = in_height;
    int size       = ((out_width * (out_height + 1)) * 3) / 2;
    uint8_t* src   = out->Ybuff;

    if (!m_frame)
    {
        unsigned char* buf  = new unsigned char[size];
        m_frame = new VideoFrame();
        init(m_frame, FMT_YV12, buf, out_width, out_height, size);
        m_frame->timecode = (int64_t)out->PicInfo.timeStamp;
        m_frame->frameNumber = out->PicInfo.picture_number;
    }

    if (!m_frame)
        return;

    // line 21 data (608/708 captions)
    // this appears to be unimplemented in the driver
    if (out->UserData && out->UserDataSz)
    {
        int size = out->UserDataSz > 1024 ? 1024 : out->UserDataSz;
        m_frame->priv[0] = new unsigned char[size];
        memcpy(m_frame->priv[0], out->UserData, size);
        m_frame->qstride = size; // don't try this at home
    }

    PixelFormat out_fmt = PIX_FMT_YUV420P;
    PixelFormat in_fmt  = bcmpixfmt_to_pixfmt(m_pix_fmt);
    AVPicture img_in, img_out;
    avpicture_fill(&img_out, (uint8_t *)m_frame->buf, out_fmt,
                   out_width, out_height);
    avpicture_fill(&img_in, src, in_fmt,
                   in_width, in_height);

    if (!(out->PicInfo.flags & VDEC_FLAG_INTERLACED_SRC))
    {
        myth_sws_img_convert(&img_out, out_fmt, &img_in, in_fmt,
                             in_width, in_height);
        m_frame->interlaced_frame = 0;
        AddFrameToQueue();
    }
    else
    {
        img_out.linesize[0] *= 2;
        img_out.linesize[1] *= 2;
        img_out.linesize[2] *= 2;
        m_frame->top_field_first = out->PicInfo.pulldown == vdecTopBottom;
        int field = out->PoutFlags & BC_POUT_FLAGS_FLD_BOT;
        if (field)
        {
            img_out.data[0] += out_width;
            img_out.data[1] += out_width >> 1;
            img_out.data[2] += out_width >> 1;
        }
        myth_sws_img_convert(&img_out, out_fmt, &img_in,
                             in_fmt, in_width, in_height / 2);
        if (second_field)
            AddFrameToQueue();
    }
}

void PrivateDecoderCrystalHD::AddFrameToQueue(void)
{
    m_decoded_frames_lock.lock();
    m_decoded_frames.insert(0, m_frame);
    VERBOSE(VB_PLAYBACK|VB_EXTRA, LOC + QString("Decoded frame queue size %1")
            .arg(m_decoded_frames.size()));
    m_decoded_frames_lock.unlock();
    m_frame = NULL;
}

void PrivateDecoderCrystalHD::CheckProcOutput(BC_DTS_PROC_OUT *out)
{
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut Ybuf      : %1")
            .arg((uintptr_t)out->Ybuff));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut Ybuffsz   : %1")
            .arg(out->YbuffSz));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut Ybuffdnsz : %1")
            .arg(out->YBuffDoneSz));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut Ubuf      : %1")
            .arg((uintptr_t)out->UVbuff));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut Ubuffsz   : %1")
            .arg(out->UVbuffSz));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut Ubuffdnsz : %1")
            .arg(out->UVBuffDoneSz));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut StrideSz  : %1")
            .arg(out->StrideSz));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut Flags     : %1")
            .arg(poutflags_to_string(out->PoutFlags)));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut DiscCnt   : %1")
            .arg(out->discCnt));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut usrdatasz : %1")
            .arg(out->UserDataSz));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut DropFrames: %1")
            .arg(out->DropFrames));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut b422Mode  : %1")
            .arg(bcmpixfmt_to_string((BC_OUTPUT_FORMAT)out->b422Mode)));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut bPIBenc   : %1")
            .arg(out->bPibEnc));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut revertscra: %1")
            .arg(out->bRevertScramble));
    CheckPicInfo(out);
}

void PrivateDecoderCrystalHD::CheckPicInfo(BC_DTS_PROC_OUT *out)
{
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo timestamp: %1")
            .arg(out->PicInfo.timeStamp));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo picnumber: %1")
            .arg(out->PicInfo.picture_number));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo width    : %1")
            .arg(out->PicInfo.width));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo height   : %1")
            .arg(out->PicInfo.height));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo chromafmt: %1")
            .arg(out->PicInfo.chroma_format));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo pulldown : %1")
            .arg(pulldown_to_string(out->PicInfo.pulldown)));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo flags    : %1")
            .arg(decoderflags_to_string(out->PicInfo.flags)));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo framerate: %1")
            .arg(out->PicInfo.frame_rate));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo aspectrat: %1")
            .arg(out->PicInfo.colour_primaries));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo metapaylo: %1")
            .arg(out->PicInfo.picture_meta_payload));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo sess_num : %1")
            .arg(out->PicInfo.sess_num));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo ycom     : %1")
            .arg(out->PicInfo.ycom));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo customasp: %1")
            .arg(out->PicInfo.custom_aspect_ratio_width_height));
    VERBOSE(VB_PLAYBACK, LOC + QString("ProcOut PicInfo ndrop    : %1")
            .arg(out->PicInfo.n_drop));
}

void PrivateDecoderCrystalHD::CheckStatus(void)
{
    BC_DTS_STATUS status;
    status.cpbEmptySize = 0x00000000; // set bit 31 for real HW free size
    INIT_ST
    st = DtsGetDriverStatus(m_device, &status);
    CHECK_ST
    if (!ok)
        return;

    VERBOSE(VB_PLAYBACK, LOC + QString("ReadyListCount  : %1")
            .arg(status.ReadyListCount));
    VERBOSE(VB_PLAYBACK, LOC + QString("FreeListCount   : %1")
            .arg(status.FreeListCount));
    VERBOSE(VB_PLAYBACK, LOC + QString("PowerStateChange: %1")
            .arg(status.PowerStateChange));
    VERBOSE(VB_PLAYBACK, LOC + QString("FrameDropped    : %1")
            .arg(status.FramesDropped));
    VERBOSE(VB_PLAYBACK, LOC + QString("FramesCaptured  : %1")
            .arg(status.FramesCaptured));
    VERBOSE(VB_PLAYBACK, LOC + QString("FramesRepeated  : %1")
            .arg(status.FramesRepeated));
    VERBOSE(VB_PLAYBACK, LOC + QString("InputCount      : %1")
            .arg(status.InputCount));
    VERBOSE(VB_PLAYBACK, LOC + QString("InputTotalSize  : %1")
            .arg(status.InputTotalSize));
    VERBOSE(VB_PLAYBACK, LOC + QString("InputBusyCount  : %1")
            .arg(status.InputBusyCount));
    VERBOSE(VB_PLAYBACK, LOC + QString("PIBMissCount    : %1")
            .arg(status.PIBMissCount));
    VERBOSE(VB_PLAYBACK, LOC + QString("cpbEmptySize    : %1")
            .arg(status.cpbEmptySize));
    VERBOSE(VB_PLAYBACK, LOC + QString("NextTimeStamp   : %1")
            .arg(status.NextTimeStamp));
    VERBOSE(VB_PLAYBACK, LOC + QString("PicNumFlags     : %1")
            .arg(status.picNumFlags));
}

int PrivateDecoderCrystalHD::GetTxFreeSize(bool hwsel)
{
    BC_DTS_STATUS status;
    if (hwsel)
        status.cpbEmptySize = 0xC0000000; // set bit 31 for real HW free size
    else
        status.cpbEmptySize = 0x40000000; // set bit 30 for TX only status
    INIT_ST
    st = DtsGetDriverStatus(m_device, &status);
    CHECK_ST
    if (!ok)
        return -1;
    
    return status.cpbEmptySize;
}

QString device_to_string(BC_DEVICE_TYPE device)
{
    switch (device)
    {
        case BC_70012: return "BCM70012";
        case BC_70015: return "BCM70015";
    }
    return "Unknown";
}

QString bcmerr_to_string(BC_STATUS err)
{
    switch (err)
    {
        case BC_STS_INV_ARG:           return "Invalid argument";
        case BC_STS_BUSY:              return "Busy";
        case BC_STS_NOT_IMPL:          return "Not implemented";
        case BC_STS_PGM_QUIT:          return "PGM quit";
        case BC_STS_NO_ACCESS:         return "No access";
        case BC_STS_INSUFF_RES:        return "Insufficient resources";
        case BC_STS_IO_ERROR:          return "I/O error";
        case BC_STS_NO_DATA:           return "No data";
        case BC_STS_VER_MISMATCH:      return "Version mismatch";
        case BC_STS_TIMEOUT:           return "Timeout";
        case BC_STS_FW_CMD_ERR:        return "Command error";
        case BC_STS_DEC_NOT_OPEN:      return "Decoder not open";
        case BC_STS_ERR_USAGE:         return "Usage error";
        case BC_STS_IO_USER_ABORT:     return "I/O user abort";
        case BC_STS_IO_XFR_ERROR:      return "I/O transfer error";
        case BC_STS_DEC_NOT_STARTED:   return "Decoder not started";
        case BC_STS_FWHEX_NOT_FOUND:   return "FirmwareHex not found";
        case BC_STS_FMT_CHANGE:        return "Format change";
        case BC_STS_HIF_ACCESS:        return "HIF access";
        case BC_STS_CMD_CANCELLED:     return "Command cancelled";
        case BC_STS_FW_AUTH_FAILED:    return "Firmware authorisation failed";
        case BC_STS_BOOTLOADER_FAILED: return "Bootloader failed";
        case BC_STS_CERT_VERIFY_ERROR: return "Certificate verify error";
        case BC_STS_DEC_EXIST_OPEN:    return "Decoder exist open (?)";
        case BC_STS_PENDING:           return "Pending";
        case BC_STS_ERROR:             return "Unknown";
    }
    return "Unknown error";
}

QString bcmpixfmt_to_string(BC_OUTPUT_FORMAT fmt)
{
    switch (fmt)
    {
        case OUTPUT_MODE420:      return "YUV420P";
        case OUTPUT_MODE422_YUY2: return "YUYV422";
        case OUTPUT_MODE422_UYVY: return "UYVY422";
    }
    return "Unknown";
}

QString pulldown_to_string(int pulldown)
{
    switch (pulldown)
    {
        case vdecNoPulldownInfo:  return "Unknown";
        case vdecTop:             return "Top";
        case vdecBottom:          return "Bottom";
        case vdecTopBottom:       return "TopBottom";
        case vdecBottomTop:       return "BottomTop";
        case vdecTopBottomTop:    return "TopBottomTop";
        case vdecBottomTopBottom: return "BottomTopBottom";
        case vdecFrame_X2:        return "X2";
        case vdecFrame_X3:        return "X3";
        case vdecFrame_X1:        return "X1";
        case vdecFrame_X4:        return "X4";
    }
    return "Unknown";
}

QString decoderflags_to_string(int flags)
{
    QString res;
    if (flags & VDEC_FLAG_EOS)            res += "EndOfStream ";
    if (flags & VDEC_FLAG_FIELDPAIR)      res += "FieldPair ";
    if (flags & VDEC_FLAG_TOPFIELD)       res += "TopField ";
    if (flags & VDEC_FLAG_BOTTOMFIELD)    res += "BottomField ";
    if (flags & VDEC_FLAG_INTERLACED_SRC) res += "InterlacedSource ";
    if (flags & VDEC_FLAG_UNKNOWN_SRC)    res += "UnknownSource ";
    if (flags & VDEC_FLAG_BOTTOM_FIRST)   res += "BottomFirst ";
    if (flags & VDEC_FLAG_LAST_PICTURE)   res += "LastPicture ";
    if (flags & VDEC_FLAG_PICTURE_META_DATA_PRESENT) res += "MetaDataPresent ";
    return res;
}

QString poutflags_to_string(int flags)
{
    QString res;
    if (flags & BC_POUT_FLAGS_YV12)        res += "YV12 ";
    if (flags & BC_POUT_FLAGS_STRIDE)      res += "STRIDE ";
    if (flags & BC_POUT_FLAGS_SIZE)        res += "SIZE ";
    if (flags & BC_POUT_FLAGS_INTERLACED)  res += "INTERLACED ";
    if (flags & BC_POUT_FLAGS_INTERLEAVED) res += "INTERLEAVED ";
    if (flags & BC_POUT_FLAGS_STRIDE_UV)   res += "UVSTRIDE ";
    if (flags & BC_POUT_FLAGS_MODE)        res += "APPMODE ";
    if (flags & BC_POUT_FLAGS_FMT_CHANGE)  res += "FORMATCHANGED ";
    if (flags & BC_POUT_FLAGS_PIB_VALID)   res += "PIBVALID ";
    if (flags & BC_POUT_FLAGS_ENCRYPTED)   res += "ENCRYPTED ";
    if (flags & BC_POUT_FLAGS_FLD_BOT)     res += "FIELDBOTTOM ";
    return res;
}

PixelFormat bcmpixfmt_to_pixfmt(BC_OUTPUT_FORMAT fmt)
{
    switch (fmt)
    {
        case OUTPUT_MODE420:      return PIX_FMT_YUV420P;
        case OUTPUT_MODE422_YUY2: return PIX_FMT_YUYV422;
        case OUTPUT_MODE422_UYVY: return PIX_FMT_UYVY422;
    }
    return PIX_FMT_YUV420P;
}
