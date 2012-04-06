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

#include <android/log.h>

#define LOG_TAG "uvccap"
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, fmt, ##__VA_ARGS__)

/* Data structure and constant values */
#define DEF_VIDEO_DEVICE   "/dev/video0"
#define DEF_CAPTURE_WIDTH  640
#define DEF_CAPTURE_HEIGHT 480
#define DEF_PIXEL_FORMAT     3
#define DEF_CAPTURE_PREFIX "video.cap"
#define DEF_CAPTURE_COUNT    1

#define NUMBER_OF_SUPPORTED_PIXEL_FORMATS 8

static uint32_t const PIXEL_FORMATS[NUMBER_OF_SUPPORTED_PIXEL_FORMATS + 1] = {
	V4L2_PIX_FMT_RGB565,
	V4L2_PIX_FMT_RGB32,
	V4L2_PIX_FMT_BGR32,
	V4L2_PIX_FMT_YUYV,
	V4L2_PIX_FMT_UYVY,
	V4L2_PIX_FMT_YUV420,
	V4L2_PIX_FMT_YUV410,
	V4L2_PIX_FMT_YUV422P,
	0, // sentinel
};

typedef union pixel_format_name_t_ {
	uint32_t u;
	char name[4];
} pixel_format_name_t;


typedef struct app_args_t_ {
	char *device;
	int   cap_width;
	int   cap_height;
	int   pixel_format;
	char *cap_prefix;
	int   cap_count;
} app_args_t;

typedef struct video_buf_t_ {
	void    *addr;
	uint32_t size;
} video_buf_t;

typedef struct video_dev_t_ {
	int                    fd;
	struct v4l2_capability caps;
	struct v4l2_cropcap    cropcaps;
	struct v4l2_crop       crop;
	struct v4l2_format     format;
	video_buf_t           *buffers;
	int                    buffer_count;
} video_dev_t;

enum ERRORS {
	NOERROR = 0,
	INVALID_ARGUMENTS = 100,
	INVALID_FORMAT_ARGUMENTS,
	VIDEO_DEVICE_BUSY,
	VIDEO_DEVICE_OPEN_FAILED,
	VIDEO_DEVICE_NOCAPS,
	VIDEO_DEVICE_NOCROPCAPS,
	VIDEO_DEVICE_CAPTURE_NOT_SUPPORTED,
	VIDEO_DEVICE_CROPPING_FAILED,
	VIDEO_DEVICE_ENUM_FORMAT_FAILED,
	VIDEO_DEVICE_QUERY_BUFFER_FAILED,
	VIDEO_DEVICE_STREAMING_FAILED,
	IO_METHOD_NOT_SUPPORTED,
	IO_ERROR,
	IO_FILE_NOT_CREATED,
	MEMORY_MAPPING_FAILED,
	MEMORY_QUEUEING_FAILED,
	MEMORY_DEQUEUEING_FAILED,
	INSUFFICIENT_MEMORY,
	NOT_PERMITTED,
};

/* Internal APIs */
static int open_video_device(app_args_t const *args, video_dev_t *dev); 
static void close_video_device(video_dev_t const *dev);
static int init_video_device(app_args_t const *args, video_dev_t *dev);
static void print_capability(struct v4l2_capability const *caps);
static void print_format_desc(struct v4l2_fmtdesc const *desc);
static uint32_t to_v4l2_pixel_format(int format);
static int init_buffer(video_dev_t *dev);
static int do_capture(app_args_t const *args, video_dev_t const *dev);
static int start_capture(video_dev_t const *dev);
static void stop_capture(video_dev_t const *dev);
static int read_frame(app_args_t const *args, video_dev_t const *dev, int index);

static void usage() {
	int i;
	pixel_format_name_t name;
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
	for (i = 0; PIXEL_FORMATS[i]; ++i) {
		name.u = PIXEL_FORMATS[i];
		printf("  %d - %c%c%c%c\n", i, name.name[0], name.name[1], name.name[2], name.name[3]); 
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
				LOGE("Invalid device path.");
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
			if (args->pixel_format >= NUMBER_OF_SUPPORTED_PIXEL_FORMATS) {
				LOGE("Pixel format (%d) is not supported.", args->pixel_format);
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
	video_dev_t dev = {
		-1,
	};

	if (parse_args(argc, argv, &args)) {
		LOGE("failed to parse arguments.");
		return INVALID_ARGUMENTS;
	}

	int ret = open_video_device(&args, &dev);
	if (NOERROR != ret) {
		return ret;
	}

	ret = init_video_device(&args, &dev);
	if (NOERROR == ret) {
		ret = do_capture(&args, &dev);
	}

	close_video_device(&dev);

	return ret;
}

static int do_capture(app_args_t const *args, video_dev_t const *dev) {
	int result;
	int count;
	int i;
	fd_set rfds;
	struct timeval tv;
	int n;

	assert(NULL != args);
	assert(NULL != dev);

	result = start_capture(dev);
	if (NOERROR != result) {
		return result;
	}

	// capture!
	count = args->cap_count;
	for (i = 0; i < count; ) {
		FD_ZERO(&rfds);
		FD_SET(dev->fd, &rfds);

		tv.tv_sec = 0;
		tv.tv_usec = 40000;

		n = select(dev->fd + 1, &rfds, NULL, NULL, &tv);

		if (n < 0) {
			if (ETIMEDOUT == errno) {
				continue;
			}
			LOGE("Failed to wait for capturable frame (%s).", strerror(errno));
			break;
		}

		if (!FD_ISSET(dev->fd, &rfds)) {
			continue;
		}

		result = read_frame(args, dev, i);
		if (NOERROR != result) {
			break;
		}

		++i;
	}

	stop_capture(dev);

	return result;
}

static int read_frame(app_args_t const *args, video_dev_t const *dev, int index) {
	struct v4l2_buffer buf;
	char path[4096];
	int fd;
	int result = NOERROR;
	uint8_t const *ptr;
	uint32_t size;
	int wrote, n;

	assert(NULL != args);
	assert(NULL != dev);

	memset(&buf, 0, sizeof(buf));
	buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;

	if (0 > ioctl(dev->fd, VIDIOC_DQBUF, &buf)) {
		LOGE("Failed to dequeueing buffer (%s).", strerror(errno));
		return MEMORY_DEQUEUEING_FAILED;
	}

	assert(buf.index < dev->buffer_count);

	// write captured data.
	snprintf(path, sizeof(path), "%s.%d", args->cap_prefix, index);
	LOGD("dump - %s", path);
	fd = open(path, O_WRONLY | O_CREAT, 0666);
	if (0 > fd) {
		if (EPERM == errno) {
			LOGE("Operation not permitted to create new file (%s).", path);
			result = NOT_PERMITTED;
		} else {
			LOGE("Failed to create new file (%s) (%s).", path, strerror(errno));
			result = IO_FILE_NOT_CREATED;
		}
	} else {
		ptr = (uint8_t const*)dev->buffers[buf.index].addr;
		size = dev->buffers[buf.index].size;
		for (wrote = 0; wrote < buf.length; ) {
			n = write(fd, ptr + wrote, size - wrote);
			if (n < 0) {
				break;
			}
			wrote += n;
		}
		close(fd);
	}

	if (0 > ioctl(dev->fd, VIDIOC_QBUF, &buf)) {
		LOGE("Failed to queueing buffer (%s).", strerror(errno));
		result = MEMORY_QUEUEING_FAILED;
	}

	return result;
}

static int open_video_device(app_args_t const *args, video_dev_t *dev) {
	uint32_t i;
	struct v4l2_fmtdesc desc;

	assert(NULL != args);
	assert(NULL != dev);

	dev->fd = open(args->device, O_RDONLY);
	if (dev->fd < 0) {
		LOGE("Can't open video devicie (%s).", args->device);
		if (EBUSY == errno) {
			LOGE("Vide device is busy.");
			return VIDEO_DEVICE_BUSY;
		}
		if (EPERM == errno) {
			LOGE("Operation not permitted.");
			return NOT_PERMITTED;
		}
		LOGE("Unknown error (%s).", strerror(errno));
		return VIDEO_DEVICE_OPEN_FAILED;
	}

	// get device capabilities
	if (0 > ioctl(dev->fd, VIDIOC_QUERYCAP, &dev->caps)) {
		LOGE("Video device capability can not get (%s).", strerror(errno));
		close(dev->fd);
		dev->fd = -1;
		return VIDEO_DEVICE_NOCAPS;
	}

	print_capability(&dev->caps);

	if (0 == (dev->caps.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		LOGE("Capture is not supported.");
		close(dev->fd);
		dev->fd = -1;
		return VIDEO_DEVICE_CAPTURE_NOT_SUPPORTED;
	}

	// get cropping capabilities
	memset(&dev->cropcaps, 0, sizeof(dev->cropcaps));
	dev->cropcaps.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (0 > ioctl(dev->fd, VIDIOC_CROPCAP, &dev->cropcaps)) {
		LOGE("Video device crop capability can not get (%s).", strerror(errno));
		return VIDEO_DEVICE_NOCROPCAPS;
	}

	// enumerate pixel formats
	for (i = 0; ; ++i) {
		memset(&desc, 0, sizeof(desc));
		desc.index = i;
		desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (0 > ioctl(dev->fd, VIDIOC_ENUM_FMT, &desc)) {
			if (EINVAL == errno) {
				break;
			} else {
				LOGE("Failed to enumerate pixel formats (%s).", strerror(errno));
				return VIDEO_DEVICE_ENUM_FORMAT_FAILED;
			}
		}
		print_format_desc(&desc);
	}

	return NOERROR;
}

static void close_video_device(video_dev_t const *dev) {
	uint32_t i;

	if (NULL == dev) {
		return;
	}

	if (0 > dev->fd) {
		return;
	}

	if (NULL != dev->buffers) {
		for (i = 0; i < dev->buffer_count; ++i) {
			if (MAP_FAILED == dev->buffers[i].addr) {
				break;
			}
			munmap(dev->buffers[i].addr, dev->buffers[i].size);
		}
		free(dev->buffers);
	}

	int ret = -1;
	do {
		ret = close(dev->fd);
	} while (ret < 0);
}

static void print_capability(struct v4l2_capability const *caps) {
	assert(NULL != caps);

	printf("Video device capabilities...\n");
	printf("  Driver : %s\n", (char const*)caps->driver);
	printf("  Card   : %s\n", (char const*)caps->card);
	printf("  Bus    : %s\n", (char const*)caps->bus_info);
	printf("  Version: %u\n", caps->version);
	printf("  Flags  : ");
	if (0 != (V4L2_CAP_VIDEO_CAPTURE & caps->capabilities))
		printf("capture ");
	if (0 != (V4L2_CAP_VIDEO_OUTPUT & caps->capabilities))
		printf("output ");
	if (0 != (V4L2_CAP_VIDEO_OVERLAY & caps->capabilities))
		printf("overlay ");
	if (0 != (V4L2_CAP_VBI_CAPTURE & caps->capabilities))
		printf("vbi_capture ");
	if (0 != (V4L2_CAP_VBI_OUTPUT & caps->capabilities))
		printf("vbi_output ");
	if (0 != (V4L2_CAP_SLICED_VBI_CAPTURE & caps->capabilities))
		printf("sliced_vbi_capture ");
	if (0 != (V4L2_CAP_SLICED_VBI_OUTPUT & caps->capabilities))
		printf("sliced_vbi_output ");
	if (0 != (V4L2_CAP_RDS_CAPTURE & caps->capabilities))
		printf("rds_capture ");
	if (0 != (V4L2_CAP_TUNER & caps->capabilities))
		printf("tuner ");
	if (0 != (V4L2_CAP_AUDIO & caps->capabilities))
		printf("audio ");
	if (0 != (V4L2_CAP_RADIO & caps->capabilities))
		printf("radio ");
	if (0 != (V4L2_CAP_READWRITE & caps->capabilities))
		printf("read_write ");
	if (0 != (V4L2_CAP_ASYNCIO & caps->capabilities))
		printf("async_io ");
	if (0 != (V4L2_CAP_STREAMING & caps->capabilities))
		printf("streaming ");
	printf("\n");
}

static void print_format_desc(struct v4l2_fmtdesc const *desc) {
	pixel_format_name_t name = { desc->pixelformat };

	printf("Format descriptor...\n");
	printf("  index       : %u\n", desc->index);
	printf("  flags       : %s\n", (0 != (V4L2_FMT_FLAG_COMPRESSED & desc->flags)) ? "compressed" : "none");
	printf("  description : %s\n", (char const*)desc->description);
	printf("  pixelformat : %c%c%c%c\n", name.name[0], name.name[1], name.name[2], name.name[3]);
}

static void print_pixel_format(struct v4l2_pix_format const *fmt) {
	pixel_format_name_t name = { fmt->pixelformat };
	printf("Pixel format...\n");
	printf("  width        : %u\n", fmt->width);
	printf("  height       : %u\n", fmt->height);
	printf("  pixelformat  : %c%c%c%c\n", name.name[0], name.name[1], name.name[2], name.name[3]);
	printf("  bytesperline : %u\n", fmt->bytesperline);
	printf("  sizeimage    : %u\n", fmt->sizeimage);
	printf("  colorspace   : %d\n", fmt->colorspace);
	printf("  private      : %u\n", fmt->priv);
}

static int init_video_device(app_args_t const *args, video_dev_t *dev) {
	struct v4l2_format fmt;

	assert(NULL != dev);

	// set cropping area
	memset(&dev->crop, 0, sizeof(dev->crop));
	dev->crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dev->crop.c = dev->cropcaps.defrect;
	if (0 > ioctl(dev->fd, VIDIOC_S_CROP, &dev->crop)) {
		if (EINVAL == errno) {
			LOGW("Cropping is not supported.");
		} else {
			LOGE("Failed to set cropping area (%s).", strerror(errno));
			return VIDEO_DEVICE_CROPPING_FAILED;
		}
	}

	// set format
	memset(&dev->format, 0, sizeof(dev->format));
	dev->format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dev->format.fmt.pix.width  = args->cap_width;
	dev->format.fmt.pix.height = args->cap_height;
	dev->format.fmt.pix.pixelformat = to_v4l2_pixel_format(args->pixel_format);
	dev->format.fmt.pix.field = V4L2_FIELD_INTERLACED;
	if (0 > ioctl(dev->fd, VIDIOC_S_FMT, &dev->format)) {
		if (EBUSY == errno) {
			LOGE("Video format can not be changed at this time.");
			return VIDEO_DEVICE_BUSY;
		}
		if (EINVAL == errno) {
			LOGE("Invalid format argument are set.");
			return INVALID_FORMAT_ARGUMENTS;
		}
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (0 == ioctl(dev->fd, VIDIOC_G_FMT, &fmt)) {
		print_pixel_format(&fmt.fmt.pix);
	}

	return init_buffer(dev);
}

static uint32_t to_v4l2_pixel_format(int format) {
	if (format >= NUMBER_OF_SUPPORTED_PIXEL_FORMATS) {
		return PIXEL_FORMATS[DEF_PIXEL_FORMAT];
	}	
	return PIXEL_FORMATS[format];
}

static int init_buffer(video_dev_t *dev) {
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	uint32_t i, count;
	video_buf_t *buf_ptr;
	int result = NOERROR;

	dev->buffers = NULL;
	dev->buffer_count = 0;

	memset(&req, 0, sizeof(req));
	req.count = 2;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (0 > ioctl(dev->fd, VIDIOC_REQBUFS, &req)) {
		if (EBUSY == errno) {
			LOGE("Buffer is already in progress.");
			return VIDEO_DEVICE_BUSY;
		}
		if (EINVAL == errno) {
			LOGE("Memory mapping is not supported.");
			return IO_METHOD_NOT_SUPPORTED;
		}
	}

	count = req.count;

	if (count < 2) {
		LOGE("Insufficient memory in video device driver.");
		return INSUFFICIENT_MEMORY;
	}

	buf_ptr = malloc(sizeof(video_buf_t) * count);
	if (NULL == buf_ptr) {
		LOGE("Insufficient memory in application.");
		return INSUFFICIENT_MEMORY;
	}

	for (i = 0; i < count; ++i) {
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.type = V4L2_MEMORY_MMAP;
		buf.index = i;

		buf_ptr[i].size = 0;
		buf_ptr[i].addr = MAP_FAILED;

		if (0 > ioctl(dev->fd, VIDIOC_QUERYBUF, &buf)) {
			if (EINVAL == errno) {
				break;
			} else {
				LOGE("Failed to query buffer (%s).", strerror(errno));
				result = VIDEO_DEVICE_QUERY_BUFFER_FAILED;
				break;
			}
		}

		buf_ptr[i].size = buf.length;
		buf_ptr[i].addr = mmap(NULL, buf.length, PROT_READ, MAP_SHARED, dev->fd, buf.m.offset);

		if (MAP_FAILED == buf_ptr[i].addr) {
			LOGE("Failed to map the video memory (%s).", strerror(errno));
			result = MEMORY_MAPPING_FAILED;
			break;
		}
	}

	if (NOERROR != result) {
		for (i = 0; i < count; ++i) {
			if (MAP_FAILED == buf_ptr[i].addr) {
				break;
			}
			munmap(buf_ptr[i].addr, buf_ptr[i].size);
			buf_ptr[i].size = 0;
			buf_ptr[i].addr = NULL;
		}
		free(buf_ptr);
	} else {
		dev->buffers      = buf_ptr;
		dev->buffer_count = i;
	}

	return result;
}

static int start_capture(video_dev_t const *dev) {
	struct v4l2_buffer buf;
	uint32_t i;
	uint32_t const count = (NULL == dev) ? 0 : dev->buffer_count;
	int retry;
	int result = NOERROR;
	enum v4l2_buf_type type;

	assert(NULL != dev);

	for (i = 0; i < count; ++i) {
		for (retry = 0; retry < 5; ++retry) {
			memset(&buf, 0, sizeof(buf));
			buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index  = i;

			if (0 == ioctl(dev->fd, VIDIOC_QBUF, &buf)) {
				break;
			}

			switch (errno) {
			case EINVAL:
				LOGE("Non-blocking I/O has been selected and no buffer.");
				result = MEMORY_QUEUEING_FAILED;
				break;
			case EIO:
				LOGE("Internal I/O error in video device.");
				result = IO_ERROR;
				break;
			case ENOMEM:
			case EAGAIN:
				usleep(10000);
				break;
			default:
				LOGE("Failed to queueing the buffer to start (%s).", strerror(errno));
				result = MEMORY_QUEUEING_FAILED;
				break;
			}

			if (NOERROR != result) {
				break;
			}
		}

		if (retry >= 5) {
			LOGE("Retry failed.");
			result = MEMORY_QUEUEING_FAILED;
		}

		if (NOERROR != result) {
			break;
		}
	}

	if (NOERROR != result) {
		return result;
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (0 > ioctl(dev->fd, VIDIOC_STREAMON, &type)) {
		LOGE("Failed to start streaming (%s).", strerror(errno));
		result = VIDEO_DEVICE_STREAMING_FAILED;
	}

	return result;
}

static void stop_capture(video_dev_t const *dev) {
	enum v4l2_buf_type type;

	assert(NULL != dev);

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (0 > ioctl(dev->fd, VIDIOC_STREAMOFF, &type)) {
		LOGW("Failed to stop streaming (%s).", strerror(errno));
	}
}

