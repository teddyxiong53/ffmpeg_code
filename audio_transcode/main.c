

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <inttypes.h>

#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include "mylog.h"
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include "libavdevice/avdevice.h"
#include <signal.h>

static int64_t pts;
typedef void (*sighandler_t)(int);

static int open_input_file(char *filename, AVFormatContext **input_format_context, AVCodecContext **input_codec_context)
{
    int ret;
    AVCodec *codec;
    AVCodecContext *codec_context;
    AVInputFormat *input_format = NULL;
    if(strcmp(filename, "default") == 0) {
        input_format = av_find_input_format("alsa");
        if(!input_format) {
            myloge("can not find alsa format");
            return -1;
        }
    }
    
    ret = avformat_open_input(input_format_context, filename, input_format, NULL);
    if(ret) {
        myloge("open input fail, %s", av_err2str(ret));
        return -1;
    }
    ret = avformat_find_stream_info(*input_format_context, NULL);
    if(ret) {
        myloge("find stream info fail, %s", av_err2str(ret));
        avformat_close_input(input_format_context);
        return -1;
    }
    if((*input_format_context)->nb_streams != 1) {
        myloge("stream number is not 1");
        avformat_close_input(input_format_context);
        return -1;
    }
    codec = avcodec_find_decoder((*input_format_context)->streams[0]->codecpar->codec_id);
    if(!codec) {
        myloge("can not find the code in ffmpeg");
        avformat_close_input(input_format_context);
        return -1;
    }
    mylogd("input find codec: name:%s, stream info: format:%d, sample_rate:%d, channels:%d, layout:%d, bitrate:%d", codec->name,
        (*input_format_context)->streams[0]->codecpar->format, 
        (*input_format_context)->streams[0]->codecpar->sample_rate,
        (*input_format_context)->streams[0]->codecpar->channels,
        (*input_format_context)->streams[0]->codecpar->channel_layout,
        (*input_format_context)->streams[0]->codecpar->bit_rate
        );
    codec_context = avcodec_alloc_context3(codec);
    if(!codec_context) {
        myloge("alloc codec context fail");
        avformat_close_input(input_format_context);
        return -1;
    }
    ret = avcodec_parameters_to_context(codec_context, (*input_format_context)->streams[0]->codecpar);
    if(ret) {
        myloge("parameter to context fail");
        avcodec_free_context(&codec_context);
        avformat_close_input(input_format_context);
        return -1;
    }
    ret = avcodec_open2(codec_context, codec, NULL);
    if(ret) {
        myloge("open codec fail");
        avcodec_free_context(codec_context);
        avformat_close_input(input_format_context);
        return -1;
    }
    *input_codec_context = codec_context;

    mylogd("open input file ok");
    return 0;
}

static int open_output_file(char *filename, AVCodecContext *input_codec_context, AVFormatContext **output_format_context, AVCodecContext **output_codec_context)
{
    int ret;
    AVIOContext *io_context;
    int is_rtmp = 0;
    if(strstr(filename, "rtmp") != NULL) {
        mylogd("output is rtmp");
        is_rtmp = 1;
    }
    ret = avio_open(&io_context, filename, AVIO_FLAG_WRITE);
    if(ret) {
        myloge("avio open fail");
        return -1;
    }
    AVFormatContext *format_context = avformat_alloc_context();
    if(!format_context) {
        myloge("alloc output fmt context fail");
        goto err0;
    }
    format_context->pb = io_context;
    char *output_format_name = NULL;
    if(!is_rtmp) {
        
    } else {
        //rtmp，就不要猜格式了，直接指定为flv。
        output_format_name = "flv";
    }

    format_context->oformat = av_guess_format(NULL, filename, NULL);
    if(!format_context->oformat) {
        myloge("find outout format fail");
        goto err1;
    }
    mylogd("guess output format:%s", format_context->oformat->name);
        
    format_context->url = av_strdup(filename);
    if(!format_context->url) {
        myloge("dup filename to url fail");
        goto err1;
    }
    AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if(!codec ) {
        myloge("find aac encoder fail");
        goto err1;
    }
    AVStream *stream = avformat_new_stream(format_context, NULL);//这里第二个为什么不能给codec呢？而要给NULL？
    if(!stream) {
        myloge("new stream fail");
        goto err1;
    }
    AVCodecContext *codec_context = avcodec_alloc_context3(codec);
    if(!codec_context) {
        myloge("alloc avcodec context fail");
        //new出来的stream，会在fmt context free的时候被释放。
        
        goto err1;
    }
    codec_context->channels = 2;//1;
    codec_context->channel_layout = av_get_default_channel_layout(2);
    codec_context->sample_fmt = codec->sample_fmts[0];
    mylogd("sample fmt:%d\n", codec_context->sample_fmt);
    codec_context->sample_rate = input_codec_context->sample_rate;
    
    codec_context->bit_rate = 96000;//32000;
    codec_context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    stream->time_base.den = input_codec_context->sample_rate;;
    stream->time_base.num = 1;
    if (format_context->oformat->flags & AVFMT_GLOBALHEADER)
        codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ret = avcodec_open2(codec_context, codec, NULL);
    if(ret) {
        myloge("open codec fail");
        goto err1;
    }

    ret = avcodec_parameters_from_context(stream->codecpar, codec_context );//fixbug:
    if(ret)  {
        myloge("parameter fail");
        goto err1;
    }
    *output_codec_context = codec_context;
    *output_format_context = format_context;
    return 0;
err1:
    avformat_free_context(format_context);
err0:
    avio_close(io_context);
    io_context = NULL;
    return -1;
}

static int init_resampler(AVCodecContext * input_codec_context, AVCodecContext * output_codec_context, SwrContext * * resample_context)
{
    int ret;
    SwrContext *context;
    context = swr_alloc_set_opts(NULL, 
        av_get_default_channel_layout(output_codec_context->channels) , 
        output_codec_context->sample_fmt, 
        output_codec_context->sample_rate, 
        av_get_default_channel_layout(input_codec_context->channels), 
        input_codec_context->sample_fmt,
        input_codec_context->sample_rate,
        0, NULL);
   if(!context) {
        myloge("alloc swr fail");
        return -1;
   }
   ret = swr_init(context);
   if(ret < 0) {
        myloge("swr init fail");
        swr_free(&context);
        return -1;
   }
   *resample_context = context;
    return 0;
}
static int init_fifo(AVAudioFifo **fifo, AVCodecContext *codec_context)
{
    *fifo = av_audio_fifo_alloc(codec_context->sample_fmt, codec_context->channels, 1);
    if(*fifo == NULL) {
        myloge("alloc fifo fail");
        return -1;
    }
    return 0;
}
static int write_output_file_header(AVFormatContext * format_context)
{
    int ret;
    ret = avformat_write_header(format_context, NULL);
    if(ret < 0) {
        myloge("write header fail");
        return -1;
    }
    return 0;
}
static void init_packet(AVPacket *packet)
{
    av_init_packet(packet);
    packet->data = NULL;
    packet->size = 0;
}
static int decode_audio_frame(AVFrame * frame, 
    AVFormatContext * input_format_context, 
    AVCodecContext * input_codec_context, 
    int * data_present, 
    int * finished)
{
    AVPacket input_packet;
    int ret;
    init_packet(&input_packet);
    ret = av_read_frame(input_format_context, &input_packet);
    if(ret < 0) {
        if(ret == AVERROR_EOF) {
            mylogd("decode audio get EOF");
            *finished = 1;
        } else {
            myloge("av_read_frame fail, %s", av_err2str(ret));
            return -1;
        }
    }
    ret = avcodec_send_packet(input_codec_context, &input_packet);
    if(ret < 0) {
        myloge("can not send packet for decoding:%s", av_err2str(ret));
        return -1;
    }
    ret = avcodec_receive_frame(input_codec_context, frame);
    if(ret == AVERROR(EAGAIN)) {
        ret = 0;
        mylogd("decode get EAGAIN");
        goto cleanup;
    } else if(ret == AVERROR_EOF) {
        ret = 0;
        mylogd("decode get EOF");
        *finished = 1;
        goto cleanup;
    } else if(ret < 0) {
        myloge("decode error:%s", av_err2str(ret));
        goto cleanup;
    } else {
        //这个是正常分支。
        *data_present = 1;
        //mylogd("ret:%d,frame->nb_samples:%d", ret, frame->nb_samples);
        goto cleanup;
    }
    
 cleanup:
    av_packet_unref(&input_packet);
    return ret;
    
}
static int init_converted_samples(uint8_t * * * converted_input_samples, AVCodecContext * output_codec_context, int frame_size)
{
    int ret;
    *converted_input_samples = calloc(output_codec_context->channels, sizeof(**converted_input_samples));
    if(!(*converted_input_samples)) {
        myloge("malloc fail");
        return AVERROR(ENOMEM);
    }
    ret = av_samples_alloc(*converted_input_samples, NULL, output_codec_context->channels, frame_size, output_codec_context->sample_fmt, 0);
    if(ret < 0) {
        myloge("sample alloc fail");
        av_freep(&(*converted_input_samples)[0]);
        free(*converted_input_samples);
        return ret;
    }
    return 0;
}
static int convert_samples(const uint8_t * * input_data, uint8_t * * converted_data, const int frame_size, SwrContext * resample_context)
{
    int ret;
    ret = swr_convert(resample_context, converted_data, frame_size, input_data, frame_size);
    if(ret < 0) {
        myloge("convert fail, %s", av_err2str(ret));
        return -1;
    }
    return 0;
}

static int add_samples_to_fifo(AVAudioFifo * fifo, uint8_t * * converted_input_samples, const int frame_size)
{
    int ret;
    ret = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo)+frame_size);
    if(ret < 0) {
        myloge("realloc fail");
        return -1;
    }
    ret = av_audio_fifo_write(fifo, (void **)converted_input_samples, frame_size);
    if(ret < frame_size) {
        myloge("can not write to fifo");
        return -1;
    }
    return 0;
}

static int read_decode_convert_and_store(AVAudioFifo * fifo, 
    AVFormatContext * input_format_context, 
    AVCodecContext * input_codec_context, 
    AVCodecContext * output_codec_context, 
    SwrContext * resampler_context, 
    int * finished)
{
    AVFrame *input_frame = NULL;
    uint8_t **converted_input_samples = NULL;
    
    input_frame = av_frame_alloc();
    if(input_frame == NULL) {
        myloge("av frame alloc fail");
        goto err1;
    }
    int data_present = 0;
    int ret;
    //解码
    ret = decode_audio_frame(input_frame, input_format_context, input_codec_context, &data_present, finished);
    if(*finished) {
        mylogd("decode finish");
        ret = 0;
        goto err1;
    }
    //mylogd("data_present:%d", data_present);
    //有正常得到数据。
    if(data_present) {
        ret = init_converted_samples(&converted_input_samples, output_codec_context, input_frame->nb_samples);
        if(ret < 0) {
            //mylogd("");
            goto err1;
        }
        ret = convert_samples((const uint8_t **)input_frame->extended_data, converted_input_samples, input_frame->nb_samples, resampler_context);
        if(ret < 0) {
            //mylogd("");
            goto err1;
        }
        ret = add_samples_to_fifo(fifo, converted_input_samples, input_frame->nb_samples);
        if(ret < 0) {
            //mylogd("");
            goto err1;
        }
        ret = 0;
    }
    ret = 0;
    
err1:
    if(converted_input_samples) {
        av_freep(&converted_input_samples[0]);
        free(converted_input_samples);
    }
    av_frame_free(&input_frame);
    
    return ret;
}

static int write_output_file_trailer(AVFormatContext * output_format_context)
{
    int ret;
    ret = av_write_trailer(output_format_context);
    if(ret < 0) {
        myloge("write trailer fail");
        return -1;
    }
    return 0;
}
static int init_output_frame(AVFrame **frame, AVCodecContext *output_codec_context, int frame_size)
{
    int ret;
    *frame = av_frame_alloc();
    if(!(*frame)) {
        myloge("malloc frame fail");
        return -1;
    }
    AVFrame *p = *frame;
    p->nb_samples = frame_size;
    p->channel_layout = output_codec_context->channel_layout;
    p->format = output_codec_context->sample_fmt;
    p->sample_rate = output_codec_context->sample_rate;

    ret = av_frame_get_buffer(*frame, 0);
    if(ret < 0) {
        myloge("get buffer fail");
        av_frame_free(frame);
        return -1;
    }
    return 0;
}

static int encode_audio_frame(AVFrame * frame, 
    AVFormatContext * output_format_context, 
    AVCodecContext * output_codec_context,
    int * data_present)
{
    AVPacket output_packet;
    init_packet(&output_packet);
    int ret;
    if(frame) {
        frame->pts = pts;
        pts += frame->nb_samples;
    }
    //mylogd("");
    ret = avcodec_send_frame(output_codec_context, frame);
    if(ret == AVERROR_EOF) {
        ret = 0;
        mylogd("encode frame to eof");
        goto cleanup;
    } else if(ret < 0) {
        myloge("send frame fail, %s", av_err2str(ret));
        return ret;
    }
    //mylogd("");
    ret = avcodec_receive_packet(output_codec_context, &output_packet);
    if(ret == AVERROR(EAGAIN)) {
        ret = 0;
        mylogd("encode get EAGAIN");
        goto cleanup;
    } else if(ret == AVERROR_EOF) {
        ret = 0;
        mylogd("encode get EOF");
        goto cleanup;
    } else if(ret < 0) {
        myloge("receive frame fail, %s", av_err2str(ret));
        goto cleanup;
    } else {
        *data_present = 1;
    }
    //mylogd("");
    ret = av_write_frame(output_format_context, &output_packet);
    if((*data_present) && (ret < 0)) {
        myloge("can not write frame");
        goto cleanup;
    }
    
cleanup: 
    //mylogd("");
    av_packet_unref(&output_packet);
    return ret;
}
static int load_encode_and_write(AVAudioFifo * fifo, 
    AVFormatContext * output_format_context, 
    AVCodecContext * output_codec_context)
{
    AVFrame *output_frame;
    int frame_size =   FFMIN(av_audio_fifo_size(fifo), output_codec_context->frame_size);
    int data_written;
    int ret;
    ret = init_output_frame(&output_frame, output_codec_context, frame_size);
    if(ret < 0) {
        myloge("init output frame fail");
        return -1;
    }
    ret = av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size);
    //mylogd("read from fifo:%d, frame_size:%d", ret, frame_size);
    if(ret < frame_size) {
        myloge("can not read data from fifo");
        av_frame_free(&output_frame);
        return -1;
    }
    ret = encode_audio_frame(output_frame,  output_format_context, output_codec_context, &data_written);
    if(ret < 0) {
        myloge("encode fail");
        av_frame_free(&output_frame);
        return -1;
    }
    av_frame_free(&output_frame);
    return 0;
}
int got_sigint = 0;
//信号的捕获是必要的，因为对于alsa录音，不存在eof，导致循环一直无法跳出，文件无法写入。
static void sighandler(int signo)
{
    mylogd("signo:%d", signo);
    got_sigint = 1;
}
void ffmpeg_init()
{
    av_log_set_level(AV_LOG_DEBUG);
    av_register_all();
    avdevice_register_all();
    avformat_network_init();
    signal(SIGINT, sighandler);
}
int main(int argc, char **argv)
{
    AVFormatContext *input_format_context = NULL, *output_format_context = NULL;
    AVCodecContext *input_codec_context= NULL, *output_codec_context= NULL;
    SwrContext *resample_context= NULL;
    AVAudioFifo *fifo = NULL;
    
    if(argc < 3) {
        mylogd("usage:%s input_file output_file\n", argv[0]);
        return -1;
    }
    ffmpeg_init();
    char *input_file = argv[1];
    char *output_file = argv[2];
    int ret;
    ret = open_input_file(input_file, &input_format_context, &input_codec_context);
    if(ret) {
        myloge("open input file fail");
        return -1;
    }
    ret = open_output_file(output_file, input_codec_context, &output_format_context, &output_codec_context);
    if(ret) {
        myloge("open output file fail");
        goto cleanup;
    }
    mylogd("output_format_context:%d", output_format_context->streams[0]->codecpar->codec_id);
    ret = init_resampler(input_codec_context, output_codec_context, &resample_context);
    if(ret) {
        myloge("init resampler  fail");
        goto cleanup;
    }
    ret = init_fifo(&fifo, output_codec_context);
    if(ret < 0) {
        myloge("init fifo fail");
        goto cleanup;
    }        
    ret = write_output_file_header(output_format_context);
    if(ret < 0) {
        myloge("write output file header fail");
        goto cleanup;
    } 
    while(1) {
        int output_frame_size = output_codec_context->frame_size;
        int finished = 0;
        if(got_sigint) {
            break;
        }
        //mylogd("av_audio_fifo_size(fifo):%d", av_audio_fifo_size(fifo));
        while(av_audio_fifo_size(fifo) < output_frame_size) {
            //读、解码、转换、存储四步一起。
            ret = read_decode_convert_and_store(fifo, input_format_context, input_codec_context, output_codec_context, resample_context, &finished);
            if(ret < 0) {
                goto cleanup;
            }
            if(finished) {
                break;
            }
        }
        while(av_audio_fifo_size(fifo) >= output_frame_size || 
               (finished && (av_audio_fifo_size(fifo) > 0)))
        {
            //mylogd("before encode and write");
            ret = load_encode_and_write(fifo, output_format_context,output_codec_context);
            if(ret < 0) {
                myloge("encode and write fail");
                goto cleanup;
            }
        }
        if(finished) {
            int data_written = 0;
            do {
                data_written = 0;
                ret = encode_audio_frame(NULL, output_format_context, output_codec_context, &data_written);
                if(ret) {
                    goto cleanup;
                }
            } while(data_written);
            break;//完成了，就进行break循环的操作。
        }
    }
    mylogd("before write tail");
    //写文件尾部。
    write_output_file_trailer(output_format_context);
    ret = 0;

cleanup:
    if(fifo) {
        av_audio_fifo_free(fifo);
        
    }
    swr_free(&resample_context);
    if(output_codec_context) {
        avcodec_free_context(&output_codec_context);
    }
    if(output_format_context) {
        avio_closep(&output_format_context->pb);
        avformat_free_context(output_format_context);
    }
    if(input_codec_context) {
        avcodec_free_context(&input_codec_context);
    }
    if(input_format_context) {
        avformat_free_context(input_format_context);
    }
    
    return ret;
}

