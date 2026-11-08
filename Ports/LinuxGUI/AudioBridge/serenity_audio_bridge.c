// SPDX-License-Identifier: BSD-2-Clause
/*
 * serenity-audio-bridge - play the PCM that serenity_compat's /dev/audio/0
 * receives, on the host's real sound card.
 *
 * The kernel side already provides a blocking, real-time sink; this daemon only
 * has to move bytes from that device into ALSA. Keeping it in userspace means
 * the driver stays tiny and the backend can be swapped (ALSA, PipeWire, a file)
 * without touching the kernel.
 */

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define SOUNDCARD_IOCTL_GET_SAMPLE_RATE 56

#define CHUNK_BYTES 4096
#define DEFAULT_RATE 44100

static volatile sig_atomic_t s_quit;

static void on_signal(int signal_number)
{
	(void)signal_number;
	s_quit = 1;
}

static unsigned int device_rate(int fd)
{
	uint32_t rate = 0;

	if (ioctl(fd, SOUNDCARD_IOCTL_GET_SAMPLE_RATE, &rate) < 0 || rate == 0)
		return DEFAULT_RATE;
	return rate;
}

static int open_device(void)
{
	static char const *candidates[] = { "/dev/audio/0", "/dev/serenity_audio0" };
	size_t i;

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
		int fd = open(candidates[i], O_RDONLY);

		if (fd >= 0) {
			fprintf(stderr, "audio-bridge: using %s\n", candidates[i]);
			return fd;
		}
	}

	return -1;
}

static int open_pcm(snd_pcm_t **pcm, unsigned int rate)
{
	/* "default" goes through PipeWire on a desktop session. The Serenity
	 * session user has no PipeWire instance of its own, so fall back to the
	 * hardware device directly (that user is in the audio group). */
	static char const *candidates[] = { "default", "hw:0,0", "plughw:0,0" };
	size_t i;

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
		int error = snd_pcm_open(pcm, candidates[i], SND_PCM_STREAM_PLAYBACK, 0);

		if (error < 0) {
			fprintf(stderr, "audio-bridge: cannot open ALSA %s: %s\n",
				candidates[i], snd_strerror(error));
			*pcm = NULL;
			continue;
		}

		/* 200ms of latency: enough headroom for a VM without adding echo. */
		error = snd_pcm_set_params(*pcm, SND_PCM_FORMAT_S16_LE,
					   SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1,
					   200000);
		if (error < 0) {
			fprintf(stderr, "audio-bridge: cannot configure %s: %s\n",
				candidates[i], snd_strerror(error));
			snd_pcm_close(*pcm);
			*pcm = NULL;
			continue;
		}

		printf("audio-bridge: playing %u Hz S16_LE stereo on %s\n", rate, candidates[i]);
		fflush(stdout);
		return 0;
	}

	return -1;
}

int main(int argc, char **argv)
{
	unsigned char buffer[CHUNK_BYTES];
	snd_pcm_t *pcm = NULL;
	unsigned int rate;
	int fd;
	time_t last_rate_check = 0;

	(void)argc;
	(void)argv;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	fd = open_device();
	if (fd < 0) {
		fprintf(stderr, "audio-bridge: no /dev/audio device (is serenity_compat loaded?)\n");
		return 1;
	}

	rate = device_rate(fd);
	if (open_pcm(&pcm, rate) < 0) {
		/* Do not tight-loop through systemd's restart: the card can be held
		 * by PipeWire from another session for a while. */
		sleep(5);
		return 1;
	}

	while (!s_quit) {
		ssize_t got;
		ssize_t offset = 0;
		time_t now;

		got = read(fd, buffer, sizeof(buffer));
		if (got < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "audio-bridge: read: %s\n", strerror(errno));
			break;
		}
		if (got == 0) {
			/* Nothing buffered yet; don't spin. */
			struct timespec pause = { 0, 2 * 1000 * 1000 };

			nanosleep(&pause, NULL);
			continue;
		}

		while (offset < got) {
			snd_pcm_sframes_t frames, written;

			frames = (got - offset) / 4;
			written = snd_pcm_writei(pcm, buffer + offset, frames);
			if (written < 0) {
				written = snd_pcm_recover(pcm, (int)written, 1);
				if (written < 0) {
					fprintf(stderr, "audio-bridge: ALSA: %s\n",
						snd_strerror((int)written));
					goto done;
				}
				continue;
			}
			offset += written * 4;
		}

		/* AudioServer changes the rate when a client asks for it. */
		now = time(NULL);
		if (now != last_rate_check) {
			unsigned int current = device_rate(fd);

			last_rate_check = now;
			if (current != rate) {
				snd_pcm_drop(pcm);
				snd_pcm_close(pcm);
				pcm = NULL;
				rate = current;
				if (open_pcm(&pcm, rate) < 0)
					goto done;
			}
		}
	}

done:
	if (pcm)
		snd_pcm_close(pcm);
	close(fd);
	return 0;
}
