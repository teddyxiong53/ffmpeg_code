

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

static int open_input_file(char *filename, AVFormatContext **input_fmt_ctx, AVCodecContext **input_codec_ctx)
{
    int ret;
    AVCodec *codec;
    AVCodecContext *avctx;
    ret = avformat_open_input(input_fmt_ctx, filename, NULL, NULL);
    if(ret) {
        myloge("open input fail");
        return -1;
    }
    ret = avformat_find_stream_info(*input_fmt_ctx, NULL);
    if(ret) {
        myloge("find stream info fail");
        avformat_close_input(input_fmt_ctx);
        return -1;
    }
    if((*input_fmt_ctx)->nb_streams != 1) {
        myloge("stream number is not 1");
        avformat_close_input(input_fmt_ctx);
        return -1;
    }
    codec = avcodec_find_decoder((*input_fmt_ctx)->streams[0]->codecpar->codec_id);
    if(!codec) {
        myloge("can not find the code in ffmpeg");
        avformat_close_input(input_fmt_ctx);
        return -1;
    }
    avctx = avcodec_alloc_context3(codec);
    if(!avctx) {
        myloge("alloc codec ctx fail");
        avformat_close_input(input_fmt_ctx);
        return -1;
    }
    ret = avcodec_parameters_to_context(avctx, (*input_fmt_ctx)->streams[0]->codecpar);
    if(ret) {
        myloge("parameter to ctx fail");
        avcodec_free_context(&avctx);
        avformat_close_input(input_fmt_ctx);
        return -1;
    }
    ret = avcodec_open2(avctx, codec, NULL);
    if(ret) {
        myloge("open codec fail");
        avcodec_free_context(avctx);
        avformat_close_input(input_fmt_ctx);
        return -1;
    }
    *input_codec_ctx = avctx;

    mylogd("open input file ok");
    return 0;
}

static int open_output_file(char *filename, AVCodecContext *input_codec_ctx, AVFormatContext **output_fmt_ctx, AVCodecContext **output_codec_fmt)
{
    int ret;
    AVIOContext *io_ctx;
    ret = avio_open(&io_ctx, filename, AVIO_FLAG_WRITE);
    if(ret) {
        myloge("avio open fail");
        return -1;
    }
    AVFormatContext *avctx = avformat_alloc_context();
    if(!avctx) {
        myloge("alloc output fmt ctx fail");
        goto err0;
    }
    avctx->pb = io_ctx;
    avctx->oformat = av_guess_format(NULL, filename, NULL);
    if(!avctx->oformat) {
        myloge("find outout format fail");
        goto err1;
    }
    avctx->url = av_strdup(filename);
    if(!avctx->url) {
        myloge("dup filename to url fail");
        goto err1;
    }
    AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if(!codec ) {
        myloge("find aac encoder fail");
        goto err1;
    }
    AVStream *stream = avformat_new_stream(avctx, codec);
    if(!stream) {
        myloge("new stream fail");
        goto err1;
    }
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if(!codec_ctx) {
        myloge("alloc avcodec ctx fail");
        //new出来的stream，会在fmt ctx free的时候被释放。
        
        goto err1;
    }
    codec_ctx->channels = 1;
    codec_ctx->channel_layout = av_get_default_channel_layout(1);
    codec_ctx->sample_fmt = codec->sample_fmts[0];
    mylogd("sample fmt:%d\n", codec_ctx->sample_fmt);
    codec_ctx->sample_rate = input_codec_ctx->sample_rate;
    
    codec_ctx->bit_rate = 32000;
    stream->time_base.den = 16000;
    stream->time_base.num = 1;

    ret = avcodec_open2(codec_ctx, codec, NULL);
    if(ret) {
        myloge("open codec fail");
        goto err1;
    }

    ret = avcodec_parameters_from_context(stream->codecpar, codec );
    if(ret)  {
        myloge("parameter fail");
        goto err1;
    }
    *output_codec_fmt = codec_ctx;
    *output_fmt_ctx = avctx;
    return 0;
err1:
    avformat_free_context(avctx);
err0:
    avio_close(io_ctx);
    io_ctx = NULL;
    return -1;
}

static int init_resampler(AVCodecContext * input_codec_context, AVCodecContext * output_codec_context, SwrContext * * resample_context)
{
    int ret;
    SwrContext *ctx;
    ctx = swr_alloc_set_opts(NULL, 
        av_get_default_channel_layout(output_codec_context->channels) , 
        output_codec_context->sample_fmt, 
        output_codec_context->sample_rate, 
        av_get_default_channel_layout(input_codec_context->channels), 
        input_codec_context->sample_fmt,
        input_codec_context->sample_rate,
        0, NULL);
   if(!ctx) {
        myloge("alloc swr fail");
        return -1;
   }
   ret = swr_init(ctx);
   if(ret < 0) {
        myloge("swr init fail");
        swr_free(&ctx);
        return -1;
   }
    return 0;
}
static int init_fifo(AVAudioFifo **fifo, AVCodecContext *codec_ctx)
{
    *fifo = av_audio_fifo_alloc(codec_ctx->sample_fmt, codec_ctx->channels, 1);
    if(*fifo == NULL) {
        myloge("alloc fifo fail");
        return -1;
    }
    return 0;
}
static int write_output_file_header(AVFormatContext * fmt_ctx)
{
    int ret;
    ret = avformat_write_header(fmt_ctx, NULL);
    if(ret < 0) {
        myloge("write header fail");
        return -1;
    }
    return 0;
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
    ret = av_read_frame(input_format_context, input_packet);
    if(ret < 0) {
        if(ret == AVERROR_EOF) {
            *finished = 1;
        } else {
            myloge("av_read_frame fail, %s", av_err2str(ret));
            return -1;
        }
    }
    ret = avcodec_send_packet(input_codec_context, input_packet);
    if(ret < 0) {
        myloge("can not send packet for decoding:%s", av_err2str(ret));
        return -1;
    }
    ret = avcodec_receive_frame(input_codec_context, frame);
    if(ret == AVERROR(EAGAIN)) {
        ret = 0;
        goto cleanup;
    } else if(ret == AVERROR_EOF) {
        ret = 0;
        *finished = 1;
        goto cleanup;
    } else if(ret < 0) {
        myloge("decode error:%s", av_err2str(ret));
        goto cleanup;
    } else {
        //这个是正常分支。
        *data_present = 1;
        goto cleanup;
    }
    
 cleanup:
    av_packet_unref(input_packet);
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
    AVFrame *input_frame;
    uint8_t **converted_input_samples;
    
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
    //有正常得到数据。
    if(data_present) {
        ret = init_converted_samples(&converted_input_samples, output_codec_context, input_frame->nb_samples);
        if(ret < 0) {
            goto err1;
        }
        ret = convert_samples((const uint8_t **)input_frame->extended_data, converted_input_samples, input_frame->nb_samples, resampler_context);
        if(ret < 0) {
            goto err1;
        }
        ret = add_samples_to_fifo(fifo, converted_input_samples, input_frame->nb_samples);
        if(ret < 0) {
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
    av_frame_free(input_frame);
    
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

    while(1) {
        int output_frame_size = output_codec_context->frame_size;
        int finished = 0;
        while(av_audio_fifo_size(fifo) < output_frame_size) {
            //读、解码、转换、存储四步一起。
            ret = read_decode_convert_and_store(fifo, input_format_context, input_codec_context, output_codec_context, resample_context, &finished);
            if(ret < 0) {
                return cleanup;
            }
            if(finished) {
                break;
            }
        }
        while(av_audio_fifo_size(fifo) >= output_frame_size || 
               (finished && (av_audio_fifo_size(fifo) > 0)))
        {
            ret = load_encode_and_write(fifo, output_format_context,output_codec_context);
            if(ret < 0) {
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

