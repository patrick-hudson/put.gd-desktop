// A PipeWire video source that announces its buffers the way a compositor's
// screen cast does, so pipewiresrc's buffer negotiation can be tested without a
// desktop session (#137). "kwin" offers KWin 6.3-6.6's Buffers param (2-4
// MemFd buffers), "mutter" offers Mutter's (2-16). Frames carry a moving bar.
//
//   screencast_like_src kwin|mutter SECONDS
//
// Prints "node <id>" once the stream exists and "buffers <n>" for each buffer
// the link allocates, and exits after SECONDS of streaming. A link that fails
// to allocate leaves it waiting, so run it under a timeout (run_buffers.sh).
#define _GNU_SOURCE
#include <fcntl.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define WIDTH 640
#define HEIGHT 360
#define FPS 30

struct data {
    struct pw_main_loop* loop;
    struct pw_stream* stream;
    struct spa_hook listener;
    struct spa_source* timer;
    struct spa_video_info_raw format;
    int stride;
    int minBuffers, defaultBuffers, maxBuffers;
    int buffers;
    unsigned frame;
    int seconds;
    int status;
};

static void onProcess(void* user) {
    struct data* d = user;
    struct pw_buffer* b = pw_stream_dequeue_buffer(d->stream);
    if (!b) return;
    struct spa_buffer* buf = b->buffer;
    uint8_t* p = buf->datas[0].data;
    if (!p) { pw_stream_queue_buffer(d->stream, b); return; }
    struct spa_meta_header* h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h));
    if (h) { h->pts = -1; h->flags = 0; h->seq = d->frame; h->dts_offset = 0; }
    const int bar = (int)(d->frame * 8 % d->format.size.width);
    for (uint32_t y = 0; y < d->format.size.height; ++y) {
        uint32_t* row = (uint32_t*)(p + y * d->stride);
        for (uint32_t x = 0; x < d->format.size.width; ++x) row[x] = ((int)x >= bar && (int)x < bar + 40) ? 0xffffffffu : 0xff202020u;
    }
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->size = d->stride * d->format.size.height;
    buf->datas[0].chunk->stride = d->stride;
    pw_stream_queue_buffer(d->stream, b);
    if (++d->frame >= (unsigned)(d->seconds * FPS)) pw_main_loop_quit(d->loop);
}

static void onTimeout(void* user, uint64_t expirations) {
    (void)expirations;
    struct data* d = user;
    pw_stream_trigger_process(d->stream);
}

static void onState(void* user, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
    (void)old;
    struct data* d = user;
    printf("state %s%s%s\n", pw_stream_state_as_string(state), error ? ": " : "", error ? error : "");
    fflush(stdout);
    if (state == PW_STREAM_STATE_PAUSED) {
        printf("node %u\n", pw_stream_get_node_id(d->stream));
        fflush(stdout);
        pw_loop_update_timer(pw_main_loop_get_loop(d->loop), d->timer, NULL, NULL, false);
    } else if (state == PW_STREAM_STATE_STREAMING) {
        struct timespec start = {0, 1}, interval = {0, SPA_NSEC_PER_SEC / FPS};
        pw_loop_update_timer(pw_main_loop_get_loop(d->loop), d->timer, &start, &interval, false);
    } else if (state == PW_STREAM_STATE_ERROR) {
        d->status = 2;
        pw_main_loop_quit(d->loop);
    }
}

static void onAddBuffer(void* user, struct pw_buffer* buffer) {
    struct data* d = user;
    struct spa_data* sd = buffer->buffer->datas;
    if (!(sd[0].type & (1 << SPA_DATA_MemFd))) { fprintf(stderr, "unexpected data type %x\n", sd[0].type); return; }
    sd[0].type = SPA_DATA_MemFd;
    sd[0].flags = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE;
    sd[0].fd = memfd_create("screencast-like", MFD_CLOEXEC);
    sd[0].mapoffset = 0;
    sd[0].maxsize = d->stride * d->format.size.height;
    if (sd[0].fd < 0 || ftruncate(sd[0].fd, sd[0].maxsize) < 0) { perror("memfd"); return; }
    sd[0].data = mmap(NULL, sd[0].maxsize, PROT_READ | PROT_WRITE, MAP_SHARED, sd[0].fd, 0);
    printf("buffers %d\n", ++d->buffers);
    fflush(stdout);
}

static void onRemoveBuffer(void* user, struct pw_buffer* buffer) {
    (void)user;
    struct spa_data* sd = buffer->buffer->datas;
    if (sd[0].data && sd[0].data != MAP_FAILED) munmap(sd[0].data, sd[0].maxsize);
    if (sd[0].fd >= 0) close(sd[0].fd);
}

static void onParamChanged(void* user, uint32_t id, const struct spa_pod* param) {
    struct data* d = user;
    if (!param || id != SPA_PARAM_Format) return;
    spa_format_video_raw_parse(param, &d->format);
    d->stride = SPA_ROUND_UP_N(d->format.size.width * 4, 4);
    uint8_t storage[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_pod* params[2];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(d->defaultBuffers, d->minBuffers, d->maxBuffers),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size, SPA_POD_Int(d->stride * d->format.size.height),
        SPA_PARAM_BUFFERS_stride, SPA_POD_Int(d->stride),
        SPA_PARAM_BUFFERS_align, SPA_POD_Int(16));
    params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    pw_stream_update_params(d->stream, params, 2);
}

static const struct pw_stream_events events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = onState,
    .param_changed = onParamChanged,
    .add_buffer = onAddBuffer,
    .remove_buffer = onRemoveBuffer,
    .process = onProcess,
};

int main(int argc, char** argv) {
    if (argc != 3 || (strcmp(argv[1], "kwin") && strcmp(argv[1], "mutter"))) {
        fprintf(stderr, "usage: %s kwin|mutter SECONDS\n", argv[0]);
        return 64;
    }
    struct data d = {0};
    const int kwin = !strcmp(argv[1], "kwin");
    d.defaultBuffers = kwin ? 3 : 16;
    d.minBuffers = 2;
    d.maxBuffers = kwin ? 4 : 16;
    d.seconds = atoi(argv[2]);
    pw_init(&argc, &argv);
    d.loop = pw_main_loop_new(NULL);
    d.timer = pw_loop_add_timer(pw_main_loop_get_loop(d.loop), onTimeout, &d);
    d.stream = pw_stream_new_simple(pw_main_loop_get_loop(d.loop), kwin ? "kwin-like-screencast" : "mutter-like-screencast",
        pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_NODE_NAME, kwin ? "kwin-like-screencast" : "mutter-like-screencast", NULL),
        &events, &d);
    uint8_t storage[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_pod* params[1];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
        SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(WIDTH, HEIGHT)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
        SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(FPS, 1), &SPA_FRACTION(1, 1), &SPA_FRACTION(FPS, 1)));
    pw_stream_connect(d.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS, params, 1);
    pw_main_loop_run(d.loop);
    printf("frames %u\n", d.frame);
    pw_stream_destroy(d.stream);
    pw_main_loop_destroy(d.loop);
    pw_deinit();
    return d.status;
}
