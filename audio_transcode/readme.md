# 打开input file



这个流程还是很好理解。

先用avformat_open_input打开文件，

得到input format context结构体实例。这个还只是分配了空间，结构体里的大部分内容还没有值。

这个时候，需要用avformat_find_stream_info。这个会把打开的音视频文件进行分析，解析出音视频参数，并自动填充到format context里去。

然后，我们就可以根据上面分析出来的内容，来判断input file是否合法，例如有没有音视频数据。

接下来，就是根据上面分析的信息，来find到对应的decoder。

用找打的decoder，来分配对应的codec context。

然后把stream里的codepar的信息，赋值给codec context。

最后，打开codec。

这样打开input file的流程就走完了。



# 打开output file

首先是分配一个format context。

一个format context，需要跟一个io context进行关联。

对于output，所以我们需要先分配一个io context，然后把它赋值给format context。

（对于input，io context不需要我们处理，默认有处理的。就是读取的）。

然后我们需要得到output format实例。

这个有两种方式来做到：

1、av_guess_format。让ffmpeg根据文件后缀名来猜测。

2、avformat_alloc_output_context2。用这个来分配format context，有个参数（字符串）可以指定输出的类型。

然后，我们使用得到的format context，new一个stream出来。



接下来，我们需要选定一种输出的编码。

所以需要先find到对应的codec。然后用codec打开codec context。

然后需要我们**手动设置**codec context的参数。还有部分数据来自input codec context。

每一种codec，都有自己支持的sample format。

例如aac的，只支持一种采样格式，就是FLTP（浮点），而不是S16这样的。



然后打开output的codec。

把codec context的值赋值给输出的stream里的codepar成员。



# 死循环里的处理

```
aac的frame size是1024个采样点。
	一个采样点，对于44.1K，双声道、s16le格式。是4个字节。
	
```

1、如果当前fifo里的nb_samples小于1024，则进行解码、转码，并把结果放入fifo。

2、如果fifo里的nb_sampels已经大于等于1024，则进行编码和写入文件

```
1、open_input_file
	目的：打开文件，填充fmt_ctx和codec_ctx。
	1、avformat_open_input
	2、avformat_find_stream_info
		这个是进一步得到文件里的stream信息。
	3、查看nb_streams是否是1路。
	4、avcodec_find_decoder
		根据stream里的编码参数里的id，看看能不能找到这个codec。
	5、avcodec_alloc_context3
		用上面找到的codec，分配codec_ctx。
	6、avcodec_parameters_to_context
		把stream里的codec参数，填充给codec_ctx。
	7、avcodec_open2
		打开codec
		
2、open_output_file
	1、avio_open
		获取AVIOContext。
	2、avformat_alloc_context
		分配output的format ctx
	3、把output fmt ctx的pb，赋值为AVIOContext指针。
	4、av_guess_format
		把output fmt ctx的oformat，通过文件名字来猜。
	5、output fmt ctx的url，赋值字符串给它。
	6、avcode_find_encoder AV_CODEC_ID_AAC
		找到aac的编码器。
	7、avformat_new_stream
		创建一个stream，通过这个stream，往output文件里写。
	8、avcodec_alloc_context3
		通过第6步拿到的编码器，这里分配codec ctx。
	9、然后对output codec ctx，进行赋值。
		指定channel、layout、采样率、码率、采样格式。
		stream ：
			设置timebase，den是采样率，num是1 。
	10、avcodec_open2
		打开output的codec。
	11、avcodec_parameters_from_context
		给第7步创建的stream的codepar成员，通过上面填充好的output codec ctx进行赋值。
		
3、init_resampler
	根据input和output，设置一个swresampler。
	1、分配。
	2、初始化。
4、init_fifo。
	分配一个AVAudioFifo。
	
5、write_output_file_header
	写文件头。
6、主循环。
	
```



# 调试记录

看看如何实现实时录音，同时往rtmp服务器传递。

现在录音可以了。

看看rtmp如何传递。

感觉当前的main循环的逻辑，不能适应rtmp的传输。

主循环，可以简化为read、write这2步。

https://github.com/leixiaohua1020/simplest_ffmpeg_streamer/blob/master/simplest_ffmpeg_streamer/simplest_ffmpeg_streamer.cpp

这个例子，是从flv到rtmp。

解决编译错误，把示例的flv下载放到本地，rtmp路径修改一下。

运行是可以的。

