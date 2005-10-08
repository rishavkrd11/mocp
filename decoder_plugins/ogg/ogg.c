/*
 * MOC - music on console
 * Copyright (C) 2002 - 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <vorbis/vorbisfile.h>
#include <vorbis/codec.h>

#define DEBUG

#include "main.h"
#include "log.h"
#include "decoder.h"
#include "io.h"
#include "audio.h"

struct ogg_data
{
	struct io_stream *stream;
	OggVorbis_File vf;
	int last_section;
	int bitrate;
	int duration;
	struct decoder_error error;
	int ok; /* was this stream successfully opened? */

	int tags_change; /* the tags were changed from the last call of
			    ogg_current_tags */
	struct file_tags *tags;
};

static void get_comment_tags (OggVorbis_File *vf, struct file_tags *info)
{
	int i;
	vorbis_comment *comments;

	comments = ov_comment (vf, -1);
	for (i = 0; i < comments->comments; i++) {
		if (!strncasecmp(comments->user_comments[i], "title=",
				 strlen ("title=")))
			info->title = xstrdup(comments->user_comments[i]
					+ strlen ("title="));
		else if (!strncasecmp(comments->user_comments[i],
					"artist=", strlen ("artist=")))
			info->artist = xstrdup (
					comments->user_comments[i]
					+ strlen ("artiat="));
		else if (!strncasecmp(comments->user_comments[i],
					"album=", strlen ("album=")))
			info->album = xstrdup (
					comments->user_comments[i]
					+ strlen ("album="));
		else if (!strncasecmp(comments->user_comments[i],
					"tracknumber=",
					strlen ("tracknumber=")))
			info->track = atoi (comments->user_comments[i]
					+ strlen ("tracknumber="));
		else if (!strncasecmp(comments->user_comments[i],
					"track=", strlen ("track=")))
			info->track = atoi (comments->user_comments[i]
					+ strlen ("track="));
	}
}

/* Return a malloc()ed description of an ov_*() error. */
static char *ogg_strerror (const int code)
{
	char *err;
	
	switch (code) {
		case OV_EREAD:
			err = "read error";
			break;
		case OV_ENOTVORBIS:
			err = "not a vorbis file";
			break;
		case OV_EVERSION:
			err = "vorbis version mismatch";
			break;
		case OV_EBADHEADER:
			err = "invalid Vorbis bitstream header";
			break;
		case OV_EFAULT:
			err = "internal (vorbis) logic fault";
			break;
		default:
			err = "unknown error";
	}

	return xstrdup (err);
}

/* Fill info structure with data from ogg comments */
static void ogg_info (const char *file_name, struct file_tags *info,
		const int tags_sel)
{
	OggVorbis_File vf;
	FILE *file;
	int ogg_time;
	int err_code;

	if (!(file = fopen (file_name, "r"))) {
		logit ("Can't open an OGG file: %s", strerror(errno));
		return;
	}

	/* ov_test() is faster than ov_open(), but we can't read file time
	 * with it. */
	if (tags_sel & TAGS_TIME) {
		if ((err_code = ov_open(file, &vf, NULL, 0)) < 0) {
			char *ogg_err = ogg_strerror (err_code);
			
			logit ("Can't open %s: %s", file_name, ogg_err);
			free (ogg_err);
			fclose (file);
			
			return;
		}
	}
	else {
		if ((err_code = ov_test(file, &vf, NULL, 0)) < 0) {
			char *ogg_err = ogg_strerror (err_code);
			
			logit ("Can't open %s: %s", file_name, ogg_err);
			free (ogg_err);
			fclose (file);
			
			return;
		}
	}

	if (tags_sel & TAGS_COMMENTS)
		get_comment_tags (&vf, info);

	if ((tags_sel & TAGS_TIME)
			&& (ogg_time = ov_time_total(&vf, -1)) != OV_EINVAL)
		info->time = ogg_time;

	ov_clear (&vf);
}

static size_t read_callback (void *ptr, size_t size, size_t nmemb,
		void *datasource)
{
	ssize_t res;

	res = io_read (datasource, ptr, size * nmemb);
	
	/* libvorbisfile expects the read callback to return >= 0 with errno
	 * set to non zero on error. */
	if (res < 0) {
		logit ("Read error");
		if (errno == 0)
			errno = 0xffff;
		res = 0;
	}
	else 
		res /= size;

	return res;
}

static int seek_callback (void *datasource, ogg_int64_t offset, int whence)
{
	debug ("Seek request to %ld (%s)", (long)offset,
			whence == SEEK_SET ? "SEEK_SET"
			: (whence == SEEK_CUR ? "SEEK_CUR" : "SEEK_END"));
	return io_seek (datasource, offset, whence);
}

static int close_callback (void *datasource ATTR_UNUSED)
{
	return 0;
}

static long tell_callback (void *datasource)
{
	return io_tell (datasource);
}

static void ogg_open_stream_internal (struct ogg_data *data)
{
	int res;
	ov_callbacks callbacks = {
		read_callback,
		seek_callback,
		close_callback,
		tell_callback
	};

	data->tags = tags_new ();
	
	if ((res = ov_open_callbacks(data->stream, &data->vf, NULL, 0,
					callbacks)) < 0) {
		char *ogg_err = ogg_strerror (res);
		
		decoder_error (&data->error, ERROR_FATAL, 0,
				ogg_err);
		debug ("ov_open error: %s", ogg_err);
		free (ogg_err);

		io_close (data->stream);
	}
	else {
		data->last_section = -1;
		data->bitrate = ov_bitrate(&data->vf, -1) / 1000;
		if ((data->duration = ov_time_total(&data->vf, -1))
				== OV_EINVAL)
			data->duration = -1;
		data->ok = 1;
		get_comment_tags (&data->vf, data->tags);
	}
}

static void *ogg_open (const char *file)
{
	struct ogg_data *data;
	data = (struct ogg_data *)xmalloc (sizeof(struct ogg_data));
	data->ok = 0;

	decoder_error_init (&data->error);
	data->tags_change = 0;
	data->tags = NULL;

	data->stream = io_open (file, 1);
	if (!io_ok(data->stream)) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Can't load OGG: %s",
				io_strerror(data->stream));
		io_close (data->stream);
	}
	else
		ogg_open_stream_internal (data);
	
	return data;
}

static int ogg_can_decode (struct io_stream *stream)
{
	char buf[34];

	if (io_peek(stream, buf, 34) == 34 && !memcmp(buf, "OggS", 5)
			&& !memcpy(buf + 28, "vorbis", 6))
		return 1;

	return 0;
}

static void *ogg_open_stream (struct io_stream *stream)
{
	struct ogg_data *data;

	data = (struct ogg_data *)xmalloc (sizeof(struct ogg_data));
	data->ok = 0;

	decoder_error_init (&data->error);
	data->stream = stream;
	ogg_open_stream_internal (data);
	
	return data;
}

static void ogg_close (void *prv_data)
{
	struct ogg_data *data = (struct ogg_data *)prv_data;

	if (data->ok) {
		ov_clear (&data->vf);
		io_close (data->stream);
	}

	decoder_error_clear (&data->error);
	if (data->tags)
		tags_free (data->tags);
	free (data);
}

static int ogg_seek (void *prv_data, int sec)
{
	struct ogg_data *data = (struct ogg_data *)prv_data;

	return ov_time_seek (&data->vf, sec) ? -1 : sec;
}

static int ogg_decode (void *prv_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
	struct ogg_data *data = (struct ogg_data *)prv_data;
	int ret;
	int current_section;
	int bitrate;
	vorbis_info *info;

	decoder_error_clear (&data->error);

	while (1) {
		ret = ov_read(&data->vf, buf, buf_len, 0, 2, 1,
				&current_section);
		if (ret == 0)
			return 0;
		if (ret < 0) {
			decoder_error (&data->error, ERROR_STREAM, 0,
					"Error in the stream!");
			continue;
		}
		
		if (current_section != data->last_section) {
			logit ("section change or first section");
			
			data->last_section = current_section;
			data->tags_change = 1;
			tags_free (data->tags);
			data->tags = tags_new ();
			get_comment_tags (&data->vf, data->tags);
		}

		info = ov_info (&data->vf, -1);
		assert (info != NULL);
		sound_params->channels = info->channels;
		sound_params->rate = info->rate;
		sound_params->fmt = SFMT_S16 | SFMT_LE;

		/* Update the bitrate information */
		bitrate = ov_bitrate_instant (&data->vf);
		if (bitrate > 0)
			data->bitrate = bitrate / 1000;

		break;
	}

	return ret;
}

static int ogg_current_tags (void *prv_data, struct file_tags *tags)
{
	struct ogg_data *data = (struct ogg_data *)prv_data;

	tags_copy (tags, data->tags);

	if (data->tags_change) {
		data->tags_change = 0;
		return 1;
	}

	return 0;
}

static int ogg_get_bitrate (void *prv_data)
{
	struct ogg_data *data = (struct ogg_data *)prv_data;

	return data->bitrate;
}

static int ogg_get_duration (void *prv_data)
{
	struct ogg_data *data = (struct ogg_data *)prv_data;

	return data->duration;
}

static struct io_stream *ogg_get_stream (void *prv_data)
{
	struct ogg_data *data = (struct ogg_data *)prv_data;

	return data->stream;
}

static void ogg_get_name (const char *file ATTR_UNUSED, char buf[4])
{
	strcpy (buf, "OGG");
}

static int ogg_our_format_ext (const char *ext)
{
	return !strcasecmp(ext, "ogg");
}

static void ogg_get_error (void *prv_data, struct decoder_error *error)
{
	struct ogg_data *data = (struct ogg_data *)prv_data;

	decoder_error_copy (error, &data->error);
}

static int ogg_our_mime (const char *mime)
{
	return !strcmp(mime, "application/ogg")
		|| !strcmp(mime, "application/x-ogg");
}

static struct decoder ogg_decoder = {
	DECODER_API_VERSION,
	ogg_open,
	ogg_open_stream,
	ogg_can_decode,
	ogg_close,
	ogg_decode,
	ogg_seek,
	ogg_info,
	ogg_get_bitrate,
	ogg_get_duration,
	ogg_get_error,
	ogg_our_format_ext,
	ogg_our_mime,
	ogg_get_name,
	ogg_current_tags,
	ogg_get_stream
};

struct decoder *plugin_init ()
{
	return &ogg_decoder;
}
