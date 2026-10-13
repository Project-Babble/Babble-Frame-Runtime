// Minimal V4L2 stateful encoder test for the headset's iris encoder
// (/dev/video-enc0). The measurements in docs/dataset-format.md were made
// with it. Build on the headset: cc -O2 -o hwenc hwenc.c
//
// usage: hwenc <dev> <w> <h> <fps> <HEVC|H264> <qp|0> <bitrate> <gop> < in.gray > out.es
// Input: raw 8-bit gray frames; chroma is filled with 128.
// qp > 0: constant QP. For HEVC that pins every QP limit to qp under VBR,
// because the driver ignores FRAME_RC_ENABLE=0. qp 0: VBR at bitrate.
// The stream is not tagged full range; mux with
//   ffmpeg -f hevc -r <fps> -i out.es -c copy -bsf:v hevc_metadata=video_full_range_flag=1 out.mkv
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NOUT 8
#define NCAP 8
static int fd;
static void die(const char *m) { fprintf(stderr, "%s: %s\n", m, strerror(errno)); exit(1); }
static int xioctl(unsigned long r, void *a) { int e; do e = ioctl(fd, r, a); while (e < 0 && errno == EINTR); return e; }
static void ctrl(uint32_t id, int32_t v, const char *name) {
    struct v4l2_control c = {.id = id, .value = v};
    if (xioctl(VIDIOC_S_CTRL, &c) < 0) fprintf(stderr, "ctrl %s=%d: %s\n", name, v, strerror(errno));
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

struct buf { void *p; size_t len; };

int main(int argc, char **argv) {
    if (argc < 9) { fprintf(stderr, "usage\n"); return 2; }
    int w = atoi(argv[2]), h = atoi(argv[3]), fps = atoi(argv[4]), qp = atoi(argv[6]), br = atoi(argv[7]), gop = atoi(argv[8]);
    uint32_t coded = strcmp(argv[5], "HEVC") == 0 ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    fd = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd < 0) die("open");

    struct v4l2_format cf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    cf.fmt.pix_mp.width = w; cf.fmt.pix_mp.height = h; cf.fmt.pix_mp.pixelformat = coded;
    cf.fmt.pix_mp.num_planes = 1; cf.fmt.pix_mp.plane_fmt[0].sizeimage = 4 << 20;
    if (xioctl(VIDIOC_S_FMT, &cf) < 0) die("S_FMT cap");
    struct v4l2_format of = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
    of.fmt.pix_mp.width = w; of.fmt.pix_mp.height = h; of.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    of.fmt.pix_mp.num_planes = 1;
    // Gray goes into Y unchanged, so ask for a full-range VUI.
    of.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
    of.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_709;
    of.fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
    of.fmt.pix_mp.quantization = V4L2_QUANTIZATION_FULL_RANGE;
    if (xioctl(VIDIOC_S_FMT, &of) < 0) die("S_FMT out");
    fprintf(stderr, "out colorspace %u ycbcr %u xfer %u quant %u\n", of.fmt.pix_mp.colorspace,
            of.fmt.pix_mp.ycbcr_enc, of.fmt.pix_mp.xfer_func, of.fmt.pix_mp.quantization);
    if (xioctl(VIDIOC_G_FMT, &cf) == 0)
        fprintf(stderr, "cap colorspace %u ycbcr %u xfer %u quant %u\n", cf.fmt.pix_mp.colorspace,
                cf.fmt.pix_mp.ycbcr_enc, cf.fmt.pix_mp.xfer_func, cf.fmt.pix_mp.quantization);
    uint32_t stride = of.fmt.pix_mp.plane_fmt[0].bytesperline, ah = of.fmt.pix_mp.height;
    uint32_t osize = of.fmt.pix_mp.plane_fmt[0].sizeimage;
    fprintf(stderr, "out %ux%u stride %u size %u planes %u; cap size %u\n", of.fmt.pix_mp.width, ah, stride, osize,
            of.fmt.pix_mp.num_planes, cf.fmt.pix_mp.plane_fmt[0].sizeimage);

    struct v4l2_streamparm sp = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
    sp.parm.output.timeperframe.numerator = 1; sp.parm.output.timeperframe.denominator = fps;
    if (xioctl(VIDIOC_S_PARM, &sp) < 0) fprintf(stderr, "S_PARM: %s\n", strerror(errno));

    ctrl(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "gop");
    ctrl(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, "bframes");
    ctrl(V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME, "header");
    if (coded == V4L2_PIX_FMT_H264) ctrl(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1, "prepend");
    if (qp > 0) {
        ctrl(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 0, "frame_rc");
        if (coded == V4L2_PIX_FMT_HEVC) {
            ctrl(V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP, qp, "iqp");
            ctrl(V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP, qp, "pqp");
            // The driver ignores rate control off; pin the QP range instead, with a bitrate cap it never reaches.
            ctrl(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1, "frame_rc");
            ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, "mode");
            ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, 240000000, "bitrate");
            ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, 245000000, "peak");
            ctrl(V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, qp, "minqp");
            ctrl(V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP, qp, "maxqp");
            ctrl(V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_MIN_QP, qp, "iminqp");
            ctrl(V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_MAX_QP, qp, "imaxqp");
            ctrl(V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_MIN_QP, qp, "pminqp");
            ctrl(V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_MAX_QP, qp, "pmaxqp");
        } else {
            ctrl(V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, qp, "iqp");
            ctrl(V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, qp, "pqp");
        }
    } else {
        ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, "mode");
        ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, br, "bitrate");
        ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, br * 2 > 245000000 ? 245000000 : br * 2, "peak");
    }

    struct buf ob[NOUT], cb[NCAP];
    struct v4l2_requestbuffers rq = {.count = NOUT, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(VIDIOC_REQBUFS, &rq) < 0) die("REQBUFS out");
    int nout = rq.count;
    for (int i = 0; i < nout; i++) {
        struct v4l2_plane pl[1] = {0};
        struct v4l2_buffer b = {.type = rq.type, .memory = V4L2_MEMORY_MMAP, .index = i, .m.planes = pl, .length = 1};
        if (xioctl(VIDIOC_QUERYBUF, &b) < 0) die("QUERYBUF out");
        ob[i].len = pl[0].length;
        ob[i].p = mmap(0, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pl[0].m.mem_offset);
        if (ob[i].p == MAP_FAILED) die("mmap out");
    }
    struct v4l2_requestbuffers rc = {.count = NCAP, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(VIDIOC_REQBUFS, &rc) < 0) die("REQBUFS cap");
    int ncap = rc.count;
    for (int i = 0; i < ncap; i++) {
        struct v4l2_plane pl[1] = {0};
        struct v4l2_buffer b = {.type = rc.type, .memory = V4L2_MEMORY_MMAP, .index = i, .m.planes = pl, .length = 1};
        if (xioctl(VIDIOC_QUERYBUF, &b) < 0) die("QUERYBUF cap");
        cb[i].len = pl[0].length;
        cb[i].p = mmap(0, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pl[0].m.mem_offset);
        if (cb[i].p == MAP_FAILED) die("mmap cap");
        if (xioctl(VIDIOC_QBUF, &b) < 0) die("QBUF cap");
    }
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(VIDIOC_STREAMON, &t) < 0) die("STREAMON cap");
    t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(VIDIOC_STREAMON, &t) < 0) die("STREAMON out");
    fprintf(stderr, "bufs out %d (%zu B) cap %d (%zu B)\n", nout, ob[0].len, ncap, cb[0].len);

    uint8_t *frame = malloc((size_t)w * h);
    int free_out[NOUT], nfree = nout;
    for (int i = 0; i < nout; i++) free_out[i] = i;
    long in = 0, outn = 0; size_t bytes = 0; int eof = 0, stopped = 0, last = 0;
    double t0 = now();
    while (!last) {
        while (!eof && nfree > 0) {
            if (fread(frame, 1, (size_t)w * h, stdin) != (size_t)w * h) { eof = 1; break; }
            int i = free_out[--nfree];
            uint8_t *y = ob[i].p, *uv = y + (size_t)stride * ah;
            for (int r = 0; r < h; r++) memcpy(y + (size_t)r * stride, frame + (size_t)r * w, w);
            for (int r = 0; r < h / 2; r++) memset(uv + (size_t)r * stride, 128, w);
            struct v4l2_plane pl[1] = {{.bytesused = osize}};
            struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP, .index = i,
                                    .m.planes = pl, .length = 1};
            b.timestamp.tv_sec = in / fps; b.timestamp.tv_usec = (in % fps) * 1000000 / fps;
            if (xioctl(VIDIOC_QBUF, &b) < 0) die("QBUF out");
            in++;
        }
        if (eof && !stopped && nfree == nout) {
            struct v4l2_encoder_cmd ec = {.cmd = V4L2_ENC_CMD_STOP};
            if (xioctl(VIDIOC_ENCODER_CMD, &ec) < 0) { fprintf(stderr, "ENC_CMD_STOP: %s\n", strerror(errno)); last = 1; }
            stopped = 1;
        }
        struct pollfd p = {.fd = fd, .events = POLLIN | POLLOUT | POLLPRI};
        int pr = poll(&p, 1, 3000);
        if (pr == 0) { fprintf(stderr, "timeout: in %ld out %ld free %d\n", in, outn, nfree); break; }
        if (p.revents & POLLERR) { fprintf(stderr, "POLLERR\n"); }
        for (;;) {
            struct v4l2_plane pl[1] = {0};
            struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1};
            if (xioctl(VIDIOC_DQBUF, &b) < 0) break;
            free_out[nfree++] = b.index;
        }
        for (;;) {
            struct v4l2_plane pl[1] = {0};
            struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1};
            if (xioctl(VIDIOC_DQBUF, &b) < 0) break;
            size_t n = pl[0].bytesused - pl[0].data_offset;
            if (n) { fwrite((uint8_t *)cb[b.index].p + pl[0].data_offset, 1, n, stdout); bytes += n; outn++; }
            if (b.flags & V4L2_BUF_FLAG_LAST) { last = 1; break; }
            pl[0].bytesused = 0;
            if (xioctl(VIDIOC_QBUF, &b) < 0) die("QBUF cap again");
        }
        if (p.revents & POLLERR && stopped && nfree == nout) break;
    }
    double dt = now() - t0;
    fprintf(stderr, "frames in %ld, packets out %ld, %zu bytes, %.1f fps\n", in, outn, bytes, in / dt);
    t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; xioctl(VIDIOC_STREAMOFF, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; xioctl(VIDIOC_STREAMOFF, &t);
    close(fd);
    return outn > 0 ? 0 : 1;
}
