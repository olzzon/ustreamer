/*****************************************************************************
#                                                                            #
#    uStreamer - Lightweight and fast MJPG-HTTP streamer.                    #
#                                                                            #
#    Copyright (C) 2018  Maxim Devaev <mdevaev@gmail.com>                    #
#                                                                            #
#    This program is free software: you can redistribute it and/or modify    #
#    it under the terms of the GNU General Public License as published by    #
#    the Free Software Foundation, either version 3 of the License, or       #
#    (at your option) any later version.                                     #
#                                                                            #
#    This program is distributed in the hope that it will be useful,         #
#    but WITHOUT ANY WARRANTY; without even the implied warranty of          #
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           #
#    GNU General Public License for more details.                            #
#                                                                            #
#    You should have received a copy of the GNU General Public License       #
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.  #
#                                                                            #
*****************************************************************************/


#include "encoder.h"


static const struct {
	const char *name;
	const encoder_type_e type;
} _ENCODER_TYPES[] = {
	{"CPU",		ENCODER_TYPE_CPU},
	{"HW",		ENCODER_TYPE_HW},
#	ifdef WITH_OMX
	{"OMX",		ENCODER_TYPE_OMX},
#	endif
#	ifdef WITH_RAWSINK
	{"NOOP",	ENCODER_TYPE_NOOP},
#	endif
};


#define ER(_next)	encoder->run->_next
#define DR(_next)	dev->run->_next


encoder_s *encoder_init(void) {
	encoder_runtime_s *run;
	A_CALLOC(run, 1);
	run->type = ENCODER_TYPE_CPU;
	run->quality = 80;
	A_MUTEX_INIT(&run->mutex);

	encoder_s *encoder;
	A_CALLOC(encoder, 1);
	encoder->type = run->type;
	encoder->quality = run->quality;
	encoder->n_workers = get_cores_available();
	encoder->run = run;
	return encoder;
}

void encoder_destroy(encoder_s *encoder) {
#	ifdef WITH_OMX
	if (ER(omxs)) {
		for (unsigned index = 0; index < ER(n_omxs); ++index) {
			if (ER(omxs[index])) {
				omx_encoder_destroy(ER(omxs[index]));
			}
		}
		free(ER(omxs));
	}
#	endif
	A_MUTEX_DESTROY(&ER(mutex));
	free(encoder->run);
	free(encoder);
}

encoder_type_e encoder_parse_type(const char *str) {
	for (unsigned index = 0; index < ARRAY_LEN(_ENCODER_TYPES); ++index) {
		if (!strcasecmp(str, _ENCODER_TYPES[index].name)) {
			return _ENCODER_TYPES[index].type;
		}
	}
	return ENCODER_TYPE_UNKNOWN;
}

const char *encoder_type_to_string(encoder_type_e type) {
	for (unsigned index = 0; index < ARRAY_LEN(_ENCODER_TYPES); ++index) {
		if (_ENCODER_TYPES[index].type == type) {
			return _ENCODER_TYPES[index].name;
		}
	}
	return _ENCODER_TYPES[0].name;
}

void encoder_prepare(encoder_s *encoder, device_s *dev) {
	encoder_type_e type = (ER(cpu_forced) ? ENCODER_TYPE_CPU : encoder->type);
	unsigned quality = encoder->quality;
	bool cpu_forced = false;

	ER(n_workers) = min_u(encoder->n_workers, DR(n_buffers));

	if ((DR(format) == V4L2_PIX_FMT_MJPEG || DR(format) == V4L2_PIX_FMT_JPEG) && type != ENCODER_TYPE_HW) {
		LOG_INFO("Switching to HW encoder because the input format is (M)JPEG");
		type = ENCODER_TYPE_HW;
	}

	if (type == ENCODER_TYPE_HW) {
		if (DR(format) != V4L2_PIX_FMT_MJPEG && DR(format) != V4L2_PIX_FMT_JPEG) {
			LOG_INFO("Switching to CPU encoder because the input format is not (M)JPEG");
			goto use_cpu;
		}

		if (hw_encoder_prepare(dev, quality) < 0) {
			quality = 0;
		}

		ER(n_workers) = 1;
	}
#	ifdef WITH_OMX
	else if (type == ENCODER_TYPE_OMX) {
		for (unsigned index = 0; index < encoder->n_glitched_resolutions; ++index) {
			if (
				encoder->glitched_resolutions[index][0] == DR(width)
				&& encoder->glitched_resolutions[index][1] == DR(height)
			) {
				LOG_INFO("Switching to CPU encoder the resolution %ux%u marked as glitchy for OMX",
					DR(width), DR(height));
				goto use_cpu;
			}
		}

		LOG_DEBUG("Preparing OMX encoder ...");

		if (ER(n_workers) > OMX_MAX_ENCODERS) {
			LOG_INFO("OMX encoder sets limit for worker threads: %u", OMX_MAX_ENCODERS);
			ER(n_workers) = OMX_MAX_ENCODERS;
		}

		if (ER(omxs) == NULL) {
			A_CALLOC(ER(omxs), OMX_MAX_ENCODERS);
		}

		// Начинаем с нуля и доинициализируем на следующих заходах при необходимости
		for (; ER(n_omxs) < ER(n_workers); ++ER(n_omxs)) {
			if ((ER(omxs[ER(n_omxs)]) = omx_encoder_init()) == NULL) {
				LOG_ERROR("Can't initialize OMX encoder, falling back to CPU");
				goto force_cpu;
			}
		}

		for (unsigned index = 0; index < ER(n_omxs); ++index) {
			if (omx_encoder_prepare(ER(omxs[index]), dev, quality) < 0) {
				LOG_ERROR("Can't prepare OMX encoder, falling back to CPU");
				goto force_cpu;
			}
		}
	}
#	endif
#	ifdef WITH_RAWSINK
	else if (type == ENCODER_TYPE_NOOP) {
		ER(n_workers) = 1;
		quality = 0;
	}
#	endif

	goto ok;

#	pragma GCC diagnostic ignored "-Wunused-label"
#	pragma GCC diagnostic push
	// cppcheck-suppress unusedLabel
	force_cpu:
		cpu_forced = true;
#	pragma GCC diagnostic pop

	use_cpu:
		type = ENCODER_TYPE_CPU;
		quality = encoder->quality;

	ok:
#		ifdef WITH_RAWSINK
		if (type == ENCODER_TYPE_NOOP) {
			LOG_INFO("Using JPEG NOOP encoder");
		} else
#		endif
		if (quality == 0) {
			LOG_INFO("Using JPEG quality: encoder default");
		} else {
			LOG_INFO("Using JPEG quality: %u%%", quality);
		}

		A_MUTEX_LOCK(&ER(mutex));
		ER(type) = type;
		ER(quality) = quality;
		if (cpu_forced) {
			ER(cpu_forced) = true;
		}
		A_MUTEX_UNLOCK(&ER(mutex));
}

void encoder_get_runtime_params(encoder_s *encoder, encoder_type_e *type, unsigned *quality) {
	A_MUTEX_LOCK(&ER(mutex));
	*type = ER(type);
	*quality = ER(quality);
	A_MUTEX_UNLOCK(&ER(mutex));
}

#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic push
int encoder_compress(encoder_s *encoder, unsigned worker_number, frame_s *src, frame_s *dest) {
#pragma GCC diagnostic pop

	assert(ER(type) != ENCODER_TYPE_UNKNOWN);
	assert(src->used > 0);

	frame_copy_meta(src, dest);
	dest->format = V4L2_PIX_FMT_JPEG;
	dest->encode_begin_ts = get_now_monotonic();
	dest->used = 0;

	if (ER(type) == ENCODER_TYPE_CPU) {
		LOG_VERBOSE("Compressing buffer using CPU");
		cpu_encoder_compress(src, dest, ER(quality));
	} else if (ER(type) == ENCODER_TYPE_HW) {
		LOG_VERBOSE("Compressing buffer using HW (just copying)");
		hw_encoder_compress(src, dest);
	}
#	ifdef WITH_OMX
	else if (ER(type) == ENCODER_TYPE_OMX) {
		LOG_VERBOSE("Compressing buffer using OMX");
		if (omx_encoder_compress(ER(omxs[worker_number]), src, dest) < 0) {
			goto error;
		}
	}
#	endif
#	ifdef WITH_RAWSINK
	else if (ER(type) == ENCODER_TYPE_NOOP) {
		LOG_VERBOSE("Compressing buffer using NOOP (do nothing)");
		usleep(5000); // Просто чтобы работала логика desired_fps
	}
#	endif

	dest->encode_end_ts = get_now_monotonic();
	return 0;

#	pragma GCC diagnostic ignored "-Wunused-label"
#	pragma GCC diagnostic push
	// cppcheck-suppress unusedLabel
	error:
		LOG_INFO("Error while compressing buffer, falling back to CPU");
		A_MUTEX_LOCK(&ER(mutex));
		ER(cpu_forced) = true;
		A_MUTEX_UNLOCK(&ER(mutex));
		return -1;
#	pragma GCC diagnostic pop
}

#undef DR
#undef ER
