#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev.h>
#include "uvccap.h"

#define LOGE(fmt, ...) fprintf(stderr, "Error: " fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) fprintf(stdout, "Debug: " fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) fprintf(stdout,           fmt, ##__VA_ARGS__)

/* Data structure and constant values */
#define DEF_VIDEO_DEVICE   "/dev/video0"
#define DEF_CAPTURE_WIDTH  640
#define DEF_CAPTURE_HEIGHT 480
#define DEF_PIXEL_FORMAT     3
#define DEF_CAPTURE_PREFIX "video.cap"
#define DEF_CAPTURE_COUNT    1

typedef struct app_args_t_ {
	char *device;
	int   cap_width;
	int   cap_height;
	int   pixel_format;
	char *cap_prefix;
	int   cap_count;
} app_args_t;

static char const *PIXEL_FORMAT_NAMES[] = {
	"RGB565",
	"RGB32",
	"BGR32",
	"YUYV",
	"UYVY",
	"YUV420",
	"YUV410",
	"YUV422P",
	NULL // sentinel
};

/* Internal APIs */
static int do_capture(uvcc_handle_t handle, app_args_t const *args);
static int write_frame(uvcc_handle_t handle, app_args_t const *args, void const *buf, uint32_t size, int index);

static void usage() {
	int i;
	printf("Usage: uvccap [options]\n");
	printf("[Option]\n");
	printf("  -d device    : path to video device.\n");
	printf("  -w width     : width of capture image.\n");
	printf("  -h height    : height of capture image.\n");
	printf("  -f format    : pixel format of capture image (default: %d).\n", DEF_PIXEL_FORMAT);
	printf("  -p prefix    : prefix of saved file name (default: %s).\n", DEF_CAPTURE_PREFIX);
	printf("  -n count     : count of capture frames (default: %d).\n", DEF_CAPTURE_COUNT);
	printf("\n");
	printf("[Pixel format]\n");
	for (i = 0; NULL != PIXEL_FORMAT_NAMES[i]; ++i) {
		printf("  %d - %s\n", i, PIXEL_FORMAT_NAMES[i]); 
	}
	exit(NOERROR);
}

static int parse_args(int argc, char **argv, app_args_t *args) {
	int opt;

	if ((argc == 2) && ((0 == strcmp(argv[1], "--help")) || (0 == strcmp(argv[1], "-?")))) {
		usage();
		return 0;
	}

	while((opt = getopt(argc, argv, "d:w:h:f:p:n:")) != -1) {
		switch(opt) {
		case 'd':
			if (NULL == optarg || '\0' == *optarg) {
				LOGE("invalid device path.\n");
				return -1;
			}
			args->device = optarg;
			break;
		case 'w':
			args->cap_width = atoi(optarg);
			break;
		case 'h':
			args->cap_height = atoi(optarg);
			break;
		case 'f':
			args->pixel_format = atoi(optarg);
			if (args->pixel_format >= UVCC_PIX_FMT_COUNT) {
				LOGE("pixel format (%d) is not supported.\n", args->pixel_format);
				return -1;
			}
			break;
		case 'p':
			args->cap_prefix = optarg;
			break;
		case 'n':
			args->cap_count = atoi(optarg);
			break;
		}
	}
	return 0;
}

int main(int argc, char **argv) {
	app_args_t args = {
		DEF_VIDEO_DEVICE,
		DEF_CAPTURE_WIDTH,
		DEF_CAPTURE_HEIGHT,
		DEF_PIXEL_FORMAT,
		DEF_CAPTURE_PREFIX,
		DEF_CAPTURE_COUNT,
	};
	uvcc_handle_t handle;

	if (parse_args(argc, argv, &args)) {
		LOGE("failed to parse arguments.\n");
		return INVALID_ARGUMENTS;
	}

	int ret = uvcc_open_video_device(&handle, args.device);
	if (NOERROR != ret) {
		LOGE("failed to open video device.\n");
		return ret;
	}

	ret = uvcc_init_video_device(handle, args.cap_width, args.cap_height, args.pixel_format);
	if (NOERROR == ret) {
		ret = do_capture(handle, &args);
	} else {
		LOGE("failed to initialize video device.\n");
	}

	uvcc_close_video_device(handle);

	return ret;
}

static int do_capture(uvcc_handle_t handle, app_args_t const *args) {
	int result;
	int count;
	int i;
	uint32_t size;
	void *buf;

	assert(NULL != args);
	assert(NULL != dev);

	result = uvcc_start_capture(handle);
	if (NOERROR != result) {
		LOGE("colud not start capture.\n");
		return result;
	}

	size = uvcc_get_frame_size(handle);
	if (size == -1) {
		LOGE("colud not get frame size.\n");
		uvcc_stop_capture(handle);
		return INVALID_STATUS;
	}

	buf = malloc(size);
	if (NULL == buf) {
		LOGE("memory allocation failed.\n");
		uvcc_stop_capture(handle);
		return INSUFFICIENT_MEMORY;
	}

	// capture!
	count = args->cap_count;
	for (i = 0; i < count; ) {
		result = uvcc_capture(handle, buf, size);
		if (NOERROR == result) {
			result = write_frame(handle, args, buf, size, i);
		}
		if (NOERROR != result) {
			break;
		}
		++i;
	}

	free(buf);
	buf = NULL;

	uvcc_stop_capture(handle);

	return result;
}

static int write_frame(uvcc_handle_t handle, app_args_t const *args, void const *buf, uint32_t size, int index) {
	char path[4096];
	int fd;
	int result = NOERROR;
	uint8_t const *ptr;
	uint32_t wrote;
	int n;

	assert(NULL != args);
	assert(NULL != dev);

	// write captured data.
	snprintf(path, sizeof(path), "%s.%d", args->cap_prefix, index);
	LOGI("dump - %s\n", path);
	fd = open(path, O_WRONLY | O_CREAT, 0666);
	if (0 > fd) {
		if (EPERM == errno) {
			LOGE("operation not permitted to create new file (%s).\n", path);
			result = NOT_PERMITTED;
		} else {
			LOGE("failed to create new file (%s) (%s).\n", path, strerror(errno));
			result = IO_FILE_NOT_CREATED;
		}
	} else {
		ptr = (uint8_t const*)ptr;
		for (wrote = 0; wrote < size; ) {
			n = write(fd, ptr + wrote, size - wrote);
			if (n < 0) {
				break;
			}
			wrote += n;
		}
		close(fd);
	}

	return result;
}


