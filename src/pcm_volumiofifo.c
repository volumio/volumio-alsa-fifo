/*
 *  PCM - Volumio FIFO plugin
 *
 *  Copyright (c) 2022 by Volumio SRL
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as
 *   published by the Free Software Foundation; either version 2.1 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#define _GNU_SOURCE
#include <limits.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>

typedef struct snd_pcm_volumiofifo {
	snd_pcm_ioplug_t io;
	char debug;
	char *fifo_name;
	char clear_on_drop;
	snd_pcm_uframes_t lead_in_frames;
	int fifo_out_fd;
	int fifo_in_fd;
	int timer_fd;
	snd_pcm_sframes_t ptr;
	snd_pcm_uframes_t boundary;
	int drained;
} snd_pcm_volumiofifo_t;

static int _snd_pcm_volumiofifo_set_timer(snd_pcm_volumiofifo_t *volumio, int on) {
	struct itimerspec timer;

	timer.it_value.tv_sec = 0;
	timer.it_value.tv_nsec = on == 1 ? 25 * 1000000 : 0;

	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_nsec = on == 1 ? 25 * 1000000 : 0;

	return timerfd_settime(volumio->timer_fd, 0, &timer, NULL);
}

/* Called outside lock */
static int snd_pcm_volumiofifo_prepare(snd_pcm_ioplug_t *io) {
	snd_pcm_volumiofifo_t *volumio = io->private_data;

	int err = 0;

	if(volumio->debug)
		SNDERR("PCM prepare called. PCM state is %s", snd_pcm_state_name(io->state));

	if(volumio->fifo_out_fd == -1 || volumio->fifo_in_fd == -1) {
		err = -EBADFD;
	}

	volumio->drained = 0;
	volumio->ptr = io->hw_ptr;

	char tmp[snd_pcm_sw_params_sizeof()];
	snd_pcm_sw_params_t *params = (snd_pcm_sw_params_t*) tmp;

	err = snd_pcm_sw_params_current(io->pcm, params);

	if(err == 0) {
		err = snd_pcm_sw_params_get_boundary(params, &volumio->boundary);
	}

	if(volumio->debug)
		SNDERR("PCM %s boundary is %lu frames", snd_pcm_name(io->pcm), volumio->boundary);

	if(err == 0) {
		err = _snd_pcm_volumiofifo_set_timer(volumio, 0);
	} else {
		_snd_pcm_volumiofifo_set_timer(volumio, 0);
	}

	return err;
}

static inline int _snd_pcm_volumiofifo_chunk_size(snd_pcm_ioplug_t *io) {
	return PIPE_BUF - (PIPE_BUF % snd_pcm_frames_to_bytes(io->pcm, 1));
}

/**
 * Transfer as much as possible to the fifo, up to the provided size
 *
 * Returns the frames transferred, 0 if nothing transfered or -ve on error
 */
static snd_pcm_sframes_t _snd_pcm_volumiofifo_transfer(snd_pcm_ioplug_t *io, snd_pcm_volumiofifo_t *volumio,
		void* buf, snd_pcm_uframes_t size) {

	snd_pcm_sframes_t written = 0;
	int err = 0;

	int size_bytes = snd_pcm_frames_to_bytes(io->pcm, size);
	int chunk_size = _snd_pcm_volumiofifo_chunk_size(io);

	int written_bytes = 0;

	do {
		int to_write = size_bytes - written_bytes;
		err = write(volumio->fifo_out_fd, buf + written_bytes,
				to_write > chunk_size ? chunk_size : to_write);
		if (err == -1) {
			if (errno == EAGAIN) {
				if (volumio->debug >= 2)
					SNDERR("PCM %s has filled the fifo %s. Receieved EAGAIN",
							snd_pcm_name(io->pcm), volumio->fifo_name);
				break;
			} else {
				SNDERR("Write to pcm %s failed with err %d",
						snd_pcm_name(io->pcm), err);
				if (written_bytes == 0) {
					written = -EPIPE;
				}
				break;
			}
		} else {
			written_bytes += err;
		}
		written = snd_pcm_bytes_to_frames(io->pcm, written_bytes);
	} while (written_bytes < size_bytes);

	return written;
}

/**
 * Transfer as much as possible to the fifo, up to the provided size,
 * copes with wrapping at the buffer boundary
 *
 * Returns the frames transferred, 0 if nothing transfered or -ve on error
 *
 * Must be called in lock to avoid duplicate writes and messing up the pointer
 */
static snd_pcm_sframes_t _snd_pcm_volumiofifo_transfer_wrap(snd_pcm_ioplug_t *io, snd_pcm_volumiofifo_t *volumio, snd_pcm_uframes_t size) {

	snd_pcm_sframes_t written = 0;

	snd_pcm_uframes_t offset = volumio->ptr % io->buffer_size;
	snd_pcm_uframes_t remaining = io->buffer_size - offset;

	if (volumio->debug >= 2)
			SNDERR("PCM %s is requesting %d frames to be transferred with %d frames before wrapping.",
									snd_pcm_name(io->pcm), size, remaining);

	const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);
	char *buf = (char *)areas->addr + ((areas->first + areas->step * offset) / 8);

	if((offset + size) > io->buffer_size) {
		snd_pcm_uframes_t remaining = io->buffer_size - offset;
		written = _snd_pcm_volumiofifo_transfer(io, volumio, buf, remaining);
		if(written == remaining) {
			if (volumio->debug >= 2)
					SNDERR("PCM %s wrote up to the end of the area. Wrapping and attempting %d more frames.",
								snd_pcm_name(io->pcm), size - remaining);
			buf = areas->addr + (areas->first / 8);
			written = _snd_pcm_volumiofifo_transfer(io, volumio, buf, size - remaining);

			if(written >= 0) {
				written += remaining;
			} else {
				written = remaining;
			}
		}
	} else {
		written = _snd_pcm_volumiofifo_transfer(io, volumio, buf, size);
	}

	if (volumio->debug >= 2)
		SNDERR("PCM %s has transferred %d frames to the fifo %s.",
								snd_pcm_name(io->pcm), written, volumio->fifo_name);

	return written;
}

/* Must be called in lock to avoid duplicate writes and messing up the pointer */
static int _snd_pcm_volumiofifo_advance(snd_pcm_ioplug_t *io, snd_pcm_volumiofifo_t *volumio) {

	if(volumio->debug > 1)
		SNDERR("PCM %s is trying to advance its hw pointer. PCM state is %s",
			snd_pcm_name(io->pcm), snd_pcm_state_name(io->state));

	switch(io->state) {
		case SND_PCM_STATE_RUNNING:
		case SND_PCM_STATE_DRAINING:
			// If running or draining then base the pointer on the state of the buffer
			break;
		case SND_PCM_STATE_XRUN:
			volumio->ptr = -EPIPE;
			break;
		default:
			// Default is not to move the pointer
		return 0;
	}

	if(volumio->ptr < 0) {
		if(volumio->debug > 1)
			SNDERR("PCM %s cannot advance its hw pointer as the pointer is %lld.",
				snd_pcm_name(io->pcm), volumio->ptr);
		// We have already hit an overrun
		return 0;
	}

	snd_pcm_uframes_t available = snd_pcm_ioplug_avail(io, volumio->ptr, io->appl_ptr);
	snd_pcm_sframes_t buffered = io->buffer_size - available;


	if(buffered > 0) {
		snd_pcm_sframes_t written = 0;

		if(io->state == SND_PCM_STATE_RUNNING) {
			written = _snd_pcm_volumiofifo_transfer_wrap(io, volumio, buffered);
		} else if (io->state == SND_PCM_STATE_DRAINING && volumio->drained == 0) {
			written = _snd_pcm_volumiofifo_transfer_wrap(io, volumio, buffered);
			if(written == buffered) {
				volumio->drained = 1;
				// Hold back one frame of the pointer so that draining waits
				// for the fifo to empty
				written -=1;
			}
		}

		if(written < 0) {
			SNDERR("PCM %s failed to advance its hw pointer.",
				snd_pcm_name(io->pcm));
			volumio->ptr = -EPIPE;
			return written;
		} else {
			volumio->ptr += written;
			if(volumio->ptr >= volumio->boundary) {
				volumio->ptr -= volumio->boundary;
			}
		}
	}

	return 0;
}

/* Called in lock */
static int snd_pcm_volumiofifo_start(snd_pcm_ioplug_t *io) {
	snd_pcm_volumiofifo_t *volumio = io->private_data;

	int err = 0;

	if(volumio->debug)
		SNDERR("PCM %s start called. PCM state is %s", snd_pcm_name(io->pcm), snd_pcm_state_name(io->state));

	// Set running before advancing the pointer
	err = snd_pcm_ioplug_set_state(io, SND_PCM_STATE_RUNNING);
	if(err == 0) {
		// Start filling the fifo now

		if(volumio->lead_in_frames > 0) {
			// Use a silent lead-in initially to help avoid
			// completely draining immediately
			char buf[snd_pcm_frames_to_bytes(io->pcm, volumio->lead_in_frames)];

			err = snd_pcm_format_set_silence(io->format, buf, io->period_size * io->channels);
			if(err == 0) {
				_snd_pcm_volumiofifo_transfer(io, volumio, buf, io->period_size);
			}
		}

		if(err == 0) {
			err = _snd_pcm_volumiofifo_advance(io, volumio);
		}
	}
	return err;
}

/* Called in lock */
static snd_pcm_sframes_t snd_pcm_volumiofifo_transfer(snd_pcm_ioplug_t *io,
	      const snd_pcm_channel_area_t *areas,
	      snd_pcm_uframes_t offset,
	      snd_pcm_uframes_t size)
{
	snd_pcm_volumiofifo_t *volumio = io->private_data;

	int err = 0;

	if(volumio->debug)
		SNDERR("PCM %s transfer called. PCM state is %s", snd_pcm_name(io->pcm), snd_pcm_state_name(io->state));

	err = _snd_pcm_volumiofifo_advance(io, volumio);

	return err == 0 ? size : err;
}

static int snd_pcm_volumiofifo_clear_pipe(snd_pcm_ioplug_t *io) {
	snd_pcm_volumiofifo_t *volumio = io->private_data;

	int err = 0;

	int chunk_size = _snd_pcm_volumiofifo_chunk_size(io);

	char *buf = malloc(chunk_size);
	if(buf == NULL) {
		err = -ENOMEM;
	}

	if(err == 0) {
		for(;;) {
			err = read(volumio->fifo_in_fd, buf, chunk_size);
			if(err < 0) {
				if(errno == EAGAIN) {
					err = 0;
				} else {
					err = -errno;
				}
				break;
			} else if(err == 0) {
				break;
			} else {
				err = 0;
			}
		}
	}
	free(buf);
	return err;
}

/* Called in lock */
static int snd_pcm_volumiofifo_stop(snd_pcm_ioplug_t *io) {
	snd_pcm_volumiofifo_t *volumio = io->private_data;
	int err = 0;

	if(volumio->debug)
		SNDERR("PCM stop called. PCM state is %s", snd_pcm_state_name(io->state));

	if(volumio->fifo_in_fd == -1) {
		err = -EPIPE;
	} else if(volumio->clear_on_drop == 1){
		if(volumio->debug)
			SNDERR("PCM %s is clearing fifo %s", snd_pcm_name(io->pcm), volumio->fifo_name);
		err = snd_pcm_volumiofifo_clear_pipe(io);
	}
	if(err == 0) {
		err = _snd_pcm_volumiofifo_set_timer(volumio, 0);
	} else {
		_snd_pcm_volumiofifo_set_timer(volumio, 0);
	}

	return err;
}

static void snd_pcm_volumiofifo_close_fd(int *fd) {
	if(*fd != -1)
		close(*fd);
	*fd = -1;
}

/* Called outside lock */
static int snd_pcm_volumiofifo_close(snd_pcm_ioplug_t *io)
{
	snd_pcm_volumiofifo_t *volumio = io->private_data;

	if(volumio->debug)
		SNDERR("PCM close called. State is %s", snd_pcm_state_name(io->state));

	if (volumio->fifo_name != NULL) {
		free(volumio->fifo_name);
		volumio->fifo_name = NULL;
	}

	snd_pcm_volumiofifo_close_fd(&volumio->fifo_out_fd);
	snd_pcm_volumiofifo_close_fd(&volumio->fifo_in_fd);
	snd_pcm_volumiofifo_close_fd(&volumio->timer_fd);

	free(volumio);

	return 0;
}

/*
 * Calculate the current position based on the total written and the
 * amount of data in the buffer;
 *
 * Called in lock
 */
static snd_pcm_sframes_t snd_pcm_volumiofifo_pointer(snd_pcm_ioplug_t *io)
{
	snd_pcm_volumiofifo_t *volumio = io->private_data;
	int err = 0;

	if(volumio->debug >= 2)
		SNDERR("PCM pointer called. State is %s", snd_pcm_state_name(io->state));


	if(volumio->fifo_out_fd == -1 || volumio->fifo_in_fd == -1) {
		volumio->ptr = -EBADFD;
		return -EBADFD;
	}

	if(io->state == SND_PCM_STATE_XRUN) {
		volumio->ptr = -EPIPE;
		return -EPIPE;
	}

	if(io->state == SND_PCM_STATE_DRAINING && volumio->drained == 1) {
		struct pollfd pfd;
		pfd.fd = volumio->fifo_in_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		err = poll(&pfd, 1, 0);

		if(err < 0) {
			SNDERR("Unable to query the fifo status. Error was %d", errno);
			volumio->ptr = -EPIPE;
			return -EPIPE;
		}

		if((pfd.revents & POLLIN) == 0) {
			if(volumio->debug > 1) {
				SNDERR("Draining complete for PCM %s.",
						snd_pcm_name(io->pcm));
			}
			volumio->ptr = -EPIPE;
		} else {
			if(volumio->debug > 1) {
				SNDERR("PCM %s must wait for the fifo %s to drain",
						snd_pcm_name(io->pcm), volumio->fifo_name);
			}
		}
	} else if (io->state == SND_PCM_STATE_RUNNING || io->state == SND_PCM_STATE_DRAINING) {
		err = _snd_pcm_volumiofifo_advance(io, volumio);
		if(err < 0) {
			SNDERR("PCM %s is unable to advance the pointer. Error was %d",
					snd_pcm_name(io->pcm), errno);
			volumio->ptr = -EPIPE;
		}
	}

	if(volumio->debug > 1) {
		SNDERR("Moving pointer for PCM %s from %d to %d. Application pointer is %d",
				snd_pcm_name(io->pcm), io->hw_ptr, volumio->ptr, io->appl_ptr);
	}

	return volumio->ptr;
}

/* Called outside lock */
static int snd_pcm_volumiofifo_poll_descriptors_count(snd_pcm_ioplug_t *io)
{
	snd_pcm_volumiofifo_t *volumio = io->private_data;

	if(volumio->debug >= 2)
		SNDERR("PCM poll descriptors count called. State is %s", snd_pcm_state_name(io->state));

	return 1;
}

/* Called outside lock */
static int snd_pcm_volumiofifo_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfds, unsigned int nfds)
{
	snd_pcm_volumiofifo_t *volumio = io->private_data;
	int err = 0;

	if(volumio->debug >= 2)
		SNDERR("PCM poll descriptors called. State is %s", snd_pcm_state_name(io->state));

	if(nfds == 1) {
		if(io->state == SND_PCM_STATE_DRAINING && volumio->drained == 1) {
			err = _snd_pcm_volumiofifo_set_timer(volumio, 1);
			pfds[0].fd = volumio->timer_fd;
			pfds[0].events = POLLIN;
			pfds[0].revents = 0;
		} else {
			pfds[0].fd = volumio->fifo_out_fd;
			pfds[0].events = POLLOUT;
			pfds[0].revents = 0;
		}
		if (err == 0)
			err = 1;
		else
			err = -errno;
	} else {
		err = -EINVAL;
	}

	return err;
}

/* Called outside lock */
static int snd_pcm_volumiofifo_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfds, unsigned int nfds, unsigned short *revents)
{
	snd_pcm_volumiofifo_t *volumio = io->private_data;
	int err = 0;

	if(volumio->debug >= 2)
		SNDERR("PCM %s revents called. State is %s", snd_pcm_name(io->pcm), snd_pcm_state_name(io->state));

	if(nfds != 1 || (pfds[0].fd != volumio->fifo_out_fd && pfds[0].fd != volumio->timer_fd)) {
		return -EINVAL;
	}

	switch(io->state) {
		case SND_PCM_STATE_RUNNING:
		case SND_PCM_STATE_DRAINING:
			// Use hwsync then ioplug_avail rather than avail because this
			// avoids updating the pointer twice, improving performance
			err = snd_pcm_hwsync(io->pcm);
			if(err == 0) {
				// Use the io hw_ptr as we have just done a sync
				err = snd_pcm_ioplug_avail(io, io->hw_ptr, io->appl_ptr);
			}
			break;
		default :
			err = io->period_size;
	}

	if(err >= io->period_size) {
		if(volumio->debug >= 2)
			SNDERR("PCM revents POLLOUT");
		*revents = POLLOUT;
		err = 0;
	} else if(err > 0) {
		if(volumio->debug >= 2)
			SNDERR("PCM revents skipping this wakeup");
		*revents = 0;
		err = 0;
	}

	return err;
}

static const snd_pcm_ioplug_callback_t volumiofifo_playback_callback = {
	.prepare = snd_pcm_volumiofifo_prepare,
	.start = snd_pcm_volumiofifo_start,
	.transfer = snd_pcm_volumiofifo_transfer,
	.stop = snd_pcm_volumiofifo_stop,
	.pointer = snd_pcm_volumiofifo_pointer,
	.close = snd_pcm_volumiofifo_close,
	.poll_descriptors_count = snd_pcm_volumiofifo_poll_descriptors_count,
	.poll_descriptors = snd_pcm_volumiofifo_poll_descriptors,
	.poll_revents = snd_pcm_volumiofifo_poll_revents,
};

SND_PCM_PLUGIN_DEFINE_FUNC(volumiofifo)
{
	snd_config_iterator_t i, next;
	const char *fifo_name = 0;
	unsigned int formats[64];
	int format_count = 0, format_append = 0, clear_on_drop = 1;
	long debug = 0, lead_in_frames = 0;
	int err;
	snd_pcm_volumiofifo_t *volumio = NULL;

	const snd_pcm_access_t access_list[] = {
		SND_PCM_ACCESS_RW_INTERLEAVED,
		SND_PCM_ACCESS_MMAP_INTERLEAVED
	};

	if(stream == SND_PCM_STREAM_CAPTURE) {
		SNDERR("The Volumio ALSA fifo plugin is playback only");
		err = -EINVAL;
		goto error;
	}

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);
		const char *id;
		const char *tmp;
		if (snd_config_get_id(n, &id) < 0)
			continue;
		if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 || strcmp(id, "hint") == 0)
			continue;
		if (strcmp(id, "debug") == 0) {
			if (snd_config_get_integer(n, &debug) < 0) {
				SNDERR("Invalid type for %s", id);
				err = -EINVAL;
				goto error;
			}
			continue;
		}
		if (strcmp(id, "fifo") == 0) {
			if (snd_config_get_string(n, &fifo_name) < 0) {
				SNDERR("Invalid type for %s", id);
				err = -EINVAL;
				goto error;
			}
			continue;
		}
		if (strcmp(id, "format_append") == 0) {
			if (snd_config_get_string(n, &tmp) < 0) {
				SNDERR("Invalid type for %s", id);
				err = -EINVAL;
				goto error;
			}
			if(strcmp(tmp, "true") == 0) {
				format_append = 1;
			}
			continue;
		}
		if (strncmp(id, "format_", 7) == 0) {
			format_count++;
			if(format_count > 63) {
				SNDERR("Too many formats declared");
				err = -EINVAL;
				goto error;
			}
			if (snd_config_get_string(n, &tmp) < 0) {
				SNDERR("Invalid type for %s", id);
				err = -EINVAL;
				goto error;
			}

			formats[format_count - 1] = snd_pcm_format_value(tmp);
			if(formats[format_count -1] == SND_PCM_FORMAT_UNKNOWN) {
				SNDERR("The value %s for key %s is not a valid format", tmp, id);
				err = -EINVAL;
				goto error;
			}
			continue;
		}
		if (strcmp(id, "clear_on_drop") == 0) {
			if (snd_config_get_string(n, &tmp) < 0) {
				SNDERR("Invalid type for %s", id);
				err = -EINVAL;
				goto error;
			}
			if(strcmp(tmp, "true") == 0) {
				clear_on_drop = 1;
			} else {
				clear_on_drop = 0;
			}
			continue;
		}
		if (strcmp(id, "lead_in_frames") == 0) {
			if (snd_config_get_integer(n, &lead_in_frames) < 0) {
				SNDERR("Invalid type for %s", id);
				err = -EINVAL;
				goto error;
			}
			if(lead_in_frames < 0 || lead_in_frames > 16384) {
				SNDERR("Lead in frames must be >= 0 and <= 16384");
			}
			continue;
		}
		SNDERR("Unknown field %s", id);
		err = -EINVAL;
		goto error;
	}

	if(!fifo_name) {
		SNDERR("A control fifo location must be provided");
		err = -EINVAL;
		goto error;
	}

	if(format_count == 0 || format_append) {
		if(format_count > 25) {
			SNDERR("Too many sound formats specified");
			err = -EINVAL;
			goto error;
		}

		formats[format_count++] = SND_PCM_FORMAT_S8;
		formats[format_count++] = SND_PCM_FORMAT_U8;
		formats[format_count++] = SND_PCM_FORMAT_S16_LE;
		formats[format_count++] = SND_PCM_FORMAT_S16_BE;
		formats[format_count++] = SND_PCM_FORMAT_U16_LE;
		formats[format_count++] = SND_PCM_FORMAT_U16_BE;
		formats[format_count++] = SND_PCM_FORMAT_S24_LE;
		formats[format_count++] = SND_PCM_FORMAT_S24_BE;
		formats[format_count++] = SND_PCM_FORMAT_U24_LE;
		formats[format_count++] = SND_PCM_FORMAT_U24_BE;
		formats[format_count++] = SND_PCM_FORMAT_S24_3LE;
		formats[format_count++] = SND_PCM_FORMAT_S24_3BE;
		formats[format_count++] = SND_PCM_FORMAT_U24_3LE;
		formats[format_count++] = SND_PCM_FORMAT_U24_3BE;
		formats[format_count++] = SND_PCM_FORMAT_S32_LE;
		formats[format_count++] = SND_PCM_FORMAT_S32_BE;
		formats[format_count++] = SND_PCM_FORMAT_U32_LE;
		formats[format_count++] = SND_PCM_FORMAT_U32_BE;
		formats[format_count++] = SND_PCM_FORMAT_FLOAT_LE;
		formats[format_count++] = SND_PCM_FORMAT_FLOAT_BE;
		formats[format_count++] = SND_PCM_FORMAT_FLOAT64_LE;
		formats[format_count++] = SND_PCM_FORMAT_FLOAT64_BE;
		formats[format_count++] = SND_PCM_FORMAT_S20_LE;
		formats[format_count++] = SND_PCM_FORMAT_S20_BE;
		formats[format_count++] = SND_PCM_FORMAT_U20_LE;
		formats[format_count++] = SND_PCM_FORMAT_U20_BE;
		formats[format_count++] = SND_PCM_FORMAT_S20_3LE;
		formats[format_count++] = SND_PCM_FORMAT_S20_3BE;
		formats[format_count++] = SND_PCM_FORMAT_U20_3LE;
		formats[format_count++] = SND_PCM_FORMAT_U20_3BE;
		formats[format_count++] = SND_PCM_FORMAT_S18_3LE;
		formats[format_count++] = SND_PCM_FORMAT_S18_3BE;
		formats[format_count++] = SND_PCM_FORMAT_U18_3LE;
		formats[format_count++] = SND_PCM_FORMAT_U18_3BE;

	}

	volumio = calloc(1, sizeof(*volumio));
	if (! volumio) {
		SNDERR("cannot allocate");
		err = -ENOMEM;
		goto error;
	}

	// Inputs
	volumio->fifo_name = NULL;
	volumio->debug = debug <= 0 ? 0 : debug >= 127 ? 127 : debug;
	volumio->clear_on_drop = clear_on_drop;
	volumio->lead_in_frames = lead_in_frames;

	// Generated
	volumio->fifo_out_fd = -1;
	volumio->fifo_in_fd = -1;
	volumio->timer_fd = -1;
	volumio->drained = 0;

	volumio->fifo_name = strdup(fifo_name);
	if (volumio->fifo_name == NULL) {
		SNDERR("cannot allocate");
		err = -ENOMEM;
		goto error;
	}

	volumio->fifo_in_fd = open(volumio->fifo_name, O_NONBLOCK | O_RDONLY);

	if(volumio->fifo_in_fd < 0) {
		SNDERR("Failed to open output fifo %s", volumio->fifo_name);
		err = -errno;
		goto error;
	}

	volumio->fifo_out_fd = open(volumio->fifo_name, O_NONBLOCK | O_WRONLY);

	if(volumio->fifo_out_fd < 0) {
		SNDERR("Failed to open output fifo %s", volumio->fifo_name);
		err = -errno;
		goto error;
	}

	volumio->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

	if(volumio->timer_fd < 0) {
		SNDERR("Failed to create timer fd");
		err = -errno;
		goto error;
	}

	volumio->io.version = SND_PCM_IOPLUG_VERSION;
	volumio->io.name = "Volumio ALSA Fifo Plugin";
	volumio->io.callback = &volumiofifo_playback_callback;
	volumio->io.private_data = volumio;
	volumio->io.mmap_rw = 1;
	volumio->io.flags = SND_PCM_IOPLUG_FLAG_BOUNDARY_WA;

	err = snd_pcm_ioplug_create(&volumio->io, name, stream, mode);
	if (err < 0)
		goto error;

	err = snd_pcm_ioplug_set_param_minmax(&volumio->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, 512, 262144);
	if (err < 0)
		goto error;
	err = snd_pcm_ioplug_set_param_minmax(&volumio->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, 1024, 524288);
	if (err < 0)
		goto error;
	err = snd_pcm_ioplug_set_param_minmax(&volumio->io, SND_PCM_IOPLUG_HW_RATE, 8000, 384000);
	if (err < 0)
		goto error;
	err = snd_pcm_ioplug_set_param_minmax(&volumio->io, SND_PCM_IOPLUG_HW_CHANNELS, 1, 16);
	if (err < 0)
		goto error;

	err = snd_pcm_ioplug_set_param_list(&volumio->io, SND_PCM_IOPLUG_HW_FORMAT, format_count, formats);
	if (err < 0)
		goto error;

	err = snd_pcm_ioplug_set_param_list(&volumio->io, SND_PCM_IOPLUG_HW_ACCESS, 2, access_list);
	if (err < 0)
		goto error;

	*pcmp = volumio->io.pcm;

	return 0;

 error:
    if(volumio) {
		if (volumio->fifo_name != NULL) {
			free(volumio->fifo_name);
			volumio->fifo_name = NULL;
		}

		snd_pcm_volumiofifo_close_fd(&volumio->fifo_out_fd);
		snd_pcm_volumiofifo_close_fd(&volumio->fifo_in_fd);
		snd_pcm_volumiofifo_close_fd(&volumio->timer_fd);

		if(volumio->io.pcm)
			snd_pcm_ioplug_delete(&volumio->io);
		else
			free(volumio);
    }
	return err;
}

SND_PCM_PLUGIN_SYMBOL(volumiofifo);
