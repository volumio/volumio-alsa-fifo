# The volumiofifo ALSA plugin

The `volumiofifo` plugin is an ALSA io plugin which writes to a named pipe (also known as a fifo). This "output" fifo can then be used as input to some other audio program, such as snapcast.

## How to use the volumiofifo ALSA plugin

A minimal configuration for the `volumiofifo` plugin would be something like:

```
pcm.volumioOutputFIFO {
    type volumiofifo
    fifo "/tmp/output/fifo"
}
```

This takes all incoming audio and writes it to the fifo at path **fifo**

### Managing the audio format

When using a fifo to exchange audio data all information about the channel count, sample rate and sample format is usually lost. It is therefore important to make sure that the audio coming into the fifo is in the right format. This is normally achieved using a `plug` with a constrained slave immediately before the fifo:

```
# This PCM converts to 48000:2:16 if needed before sending on to the volumioOutputFIFO
pcm.volumioMultiRoom {
    type plug
    slave {
        pcm "volumioOutputFIFO"
        rate 48000 
        format "S16_LE"            
        channels 2        
    }
    rate_converter "speexrate_medium"
}

pcm.volumioOutputFIFO {
    type volumiofifo
    fifo "/tmp/output/fifo"
}
```

### Advanced format management

Every ALSA plugin has the opportunity to define the audio formats that it supports. This in turn defines the audio formats that are available to audio sources playing into the ALSA pipeline. As an ouput plugin (one which terminates an ALSA pipeline and passes the audio data outside of ALSA) the `volumiofifo` plugin is not restricted by what comes next in the pipeline, and therefore could support any data format.

By default the `volumiofifo` plugin permits any linear (signed or unsigned) or floating point sample format which is supported by ALSA. If you wish to support other formats (e.g. DSD), or simply to restrict the supported formats, then this can be configured by adding the formats using `format_xxx` as a configuration key.

```
pcm.volumioOutputFIFO {
    type volumiofifo
    fifo "/tmp/output/fifo"
    format_1 "DSD_U8"
    format_2 "DSD_U16_LE"
    ...
}
```

When specifying formats the volumiofifo plugin will, by default, only support those formats at runtime. If you wish to extend the built in list rather than replace it then the `format_append` flag can be used

```
pcm.volumioOutputFIFO {
    type volumiofifo
    fifo "/tmp/output/fifo"
    format_append "true"
    format_1 "DSD_U8"
    format_2 "DSD_U16_LE"
    ...
}
```

## Why not use the file plugin

The ALSA file plugin can be used with a fifo, however its behaviour is not ideal with respect to startup ordering (it can fail to start if nobody is reading the fifo yet). The file plugin also does not cope with the fifo being full with no reader. The file plugin can also have issues on `drain` and `drop` as it attempts to write a header.

The `volumiofifo` plugin avoids these issues. It also avoids the need for a slave PCM, making it a better choice for a final output, and dramatically improving performance due to its better use of poll descriptors.

## Troubleshooting

There are several configuration options that can be used to help diagnose problems, or to help keep audio running smoothly

### Enabling debug

Debug logging can be enabled using the `debug` configuration key. The default is `0` which gives no low level debug. The value `1` will give a manageable amount of output, but will not track all calls. The value `2` will track all calls, but not internal pointer management. The value `3` will track all calls and the state of the internal pointer.

Note that debug `2` and `3` can be very verbose, and this may affect performance.

```
pcm.volumioOutputFIFO {
    type volumiofifo
    fifo "/tmp/output/fifo"
    debug 1
}
```

### Preventing dropouts or XRUN when starting playback

Sometimes using the `volumiofifo` plugin can introduce an audio dropout or an XRUN when starting playback. This happens when the fifo is initially empty, and so when the ALSA PCM starts the buffer is drained very rapidly filling the FIFO.

There are a few things that you can do to help with this:

* Increase the source buffer size - the fifo has a default size of 64kB - if the ALSA buffer is small then this may be larger than the ALSA buffer, potentially causing an immediate XRUN

* Configure the FIFO to add some lead-in silence. In the case where there is no XRUN some audio sources (e.g. MPD) will add periods of silence if the audio cannot be downloaded/decoded fast enough to keep the buffer full. This can be particularly noticeable when streaming live audio, where there may not be enough data to refill the buffer. Adding lead in silence is simple:

```
pcm.volumioOutputFIFO {
    type volumiofifo
    fifo "/tmp/output/fifo"
    lead_in_frames 8192
}
```

Lead in silence is measured in frames. It is recommended set an amount which does not completely fill the fifo, for 16 bit stereo a value of `15360` or less, for 24 bit stereo a value of `7680` or less. A value of `0` (the default) disables lead in silence


### Disabling rapid drop if pausing or skipping causes audio artifacts

The `volumiofifo` plugin attempts to drain the fifo when `drop` is called on the PCM. This ensures that audio is dropped as rapidly as possible. Care is taken to only clear whole samples, however some audio stuttering may occur on some systems. If this happens to you then you can disable the rapid drop by setting `clear_on_drop` to `false`

```
pcm.volumioOutputFIFO {
    type volumiofifo
    fifo "/tmp/output/fifo"
    clear_on_drop "false"
}
```

## A Detailed breakdown of how this plugin works

The `volumiofifo` plugin uses the ALSA ioplug API to implement a user-space audio output. Effectively this makes the `volumiofifo` plugin a virtual soundcard. The `volumiofifo` plugin is therefore a "terminal" state in the ALSA pipeline, even if the audio stream is being routed back into ALSA by whatever is consuming from the named pipe.

### Audio format support

The `volumiofifo` plugin configures itself at startup. The plugin itself doesn't interpret the audio (other than keeping track of the frame boundaries) and could therefore support any fixed-width format, any rate or any channel count. The one restriction that is imposed is that the frames be interleaved as this hugely simplifies the implementation and improves performance.

As described above, the set of supported formats can be customised in configuration, but by default all fixed-width PCM formats are supported.

### The buffer and the pointer

All ALSA playback occurs through a buffer, the size of which is determined by the client playing audio. This buffer is automatically created and managed by the ALSA ioplug API, and allows the `volumiofifo` plugin to behave as though all audio is written using mmap. This greatly simplfies the internal implementation, and means that no buffer management is needed in the plugin.

The pointer represents how far through the buffer the `volumiofifo` plugin has played. Whenever the client sends data or calls snd_pcm_hwsync (this may be automatic) then the pointer is updated. If the `volumiofifo` plugin is not in `RUNNING` or `DRAINING` state then the pointer does not move and the update is finished, otherwise the `volumiofifo` plugin attempts to write data to the named pipe. Writes are always an integer number of frames and less than `PIPE_BUF` bytes to ensure that they are atomic and do not leave the buffer in an invalid state. The source of the write is the ALSA buffer - the start of the write is the location pointed to by the `volumiofifo` pointer - the end of the write must never go past the ALSA application pointer (this would be an overrun). After a successful write the `volumiofifo` pointer is advanced by the number of frames that were written to the named pipe.

As the pointer advances this automatically opens space in the ALSA buffer for more data. The only point where care must be taken is when draining. When draining the ALSA library will automatically clean up when the `volumiofifo` pointer reaches the end of the buffer (the application pointer). We therefore hold the `volumiofifo` pointer back by one frame before the end of the buffer when draining, holding the pcm open, until the named pipe has completely emptied. This prevents the pcm from finishing before audio playback finishes.

### The poll descriptors

In normal playback the `volumiofifo` plugin uses a write descriptor to determine when the named pipe is writeable. This means that the plugin is efficiently woken when more data can be written. There may be some idle wake ups. This happens when the named pipe has space for some data, which is written, but it does not move the pointer enough to free up a full period in the ALSA buffer. This is normal behaviour for ALSA and is tolerated by clients.

When draining the poll descriptor changes. This is because the named pipe will become writeable 100% of the time while the client waits for data to drain. This would be highly inefficient and cause a busy spin. The `volumiofifo` plugin therefore switches to a timerfd once draining has begun. This notifies the client periodically, rather than when there is space in the named pipe. Each wakeup is used by the plugin to check the state of the pipe and to see if the drain has completed.

### Clear on drop

When a pcm is dropped it is supposed to rapidly clear any pending data. For the `volumiofifo` plugin this could be assumed to include data in the named pipe. Depending as to whether data in the pipe is considered to be "played" or "buffered" different behaviour is required. The `volumiofifo` plugin can therefore be configured to `clear_on_drop` meaning that it eagerly drains the named pipe when dropped (the pipe data is buffered) or to leave the data in the pipe (the pipe data is played).

### Lead in frames

When the `volumiofifo` plugin is in PREPARED state the data is queued in the ALSA buffer. Once the pcm starts the ALSA buffer is copied into the named pipe. As the named pipe is likely to be empty at start this results in a large number of frames being copied, and a big drop in the buffered data. This sudden change can cause issues with some clients as they struggle to refill the buffer, typically a short period of silence just after playback starts.

To ameliorate this situation the `volumiofifo` plugin can be told to play `lead_in_frames`. If enabled the fifo will generate the configured number of frames of silence and play them into the named pipe at startup. This reduces pressure on the ALSA buffer by partially filling the named pipe.


## Building the plugin

The plugin is written in C with CMAKE as a build system.

```
cmake .
make
```
