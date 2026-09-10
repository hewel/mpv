#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/bsf.h>

#include "common/av_common.h"
#include "demux/demux.h"
#include "demux/dovi_split.h"
#include "demux/packet.h"
#include "demux/stheader.h"
#include "mpv_talloc.h"
#include "test_utils.h"

// Stream registration and codec parameters are fixtures: packet allocation,
// dispatch and the installed FFmpeg filter run their production implementations.
struct sh_stream *demux_alloc_sh_stream(enum stream_type type)
{
    struct sh_stream *sh = talloc_zero(NULL, struct sh_stream);
    sh->type = type;
    sh->index = -1;
    sh->codec = talloc_zero(sh, struct mp_codec_params);
    sh->codec->type = type;
    return sh;
}

void demux_add_sh_stream(struct demuxer *demuxer, struct sh_stream *sh)
{
    assert_int_equal(sh->type, STREAM_VIDEO);
    assert_int_equal(sh->index, -1);
    sh->index = 1;
    talloc_steal(demuxer, sh);
}

AVCodecParameters *mp_codec_params_to_av(const struct mp_codec_params *c)
{
    assert_string_equal(c->codec, "hevc");
    AVCodecParameters *par = avcodec_parameters_alloc();
    assert_true(par);
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = AV_CODEC_ID_HEVC;
    return par;
}

AVRational mp_get_codec_timebase(const struct mp_codec_params *c)
{
    return (AVRational){c->native_tb_num, c->native_tb_den};
}

// The splitter parses NAL headers, not slice contents. Distinct payloads make
// it possible to detect a buffered output being paired with the next input.
static const uint8_t bl_only[] = {
    0, 0, 1, 0x02, 0x01, 0x11,
};

static const uint8_t two_el[] = {
    0, 0, 1, 0x02, 0x01, 0x11,
    0, 0, 1, 0x7e, 0x01, 0x02, 0x01, 0x21,
    0, 0, 1, 0x7e, 0x01, 0x02, 0x01, 0x22,
};

static const uint8_t two_el_output[] = {
    0, 0, 0, 1, 0x02, 0x01, 0x21,
    0, 0, 0, 1, 0x02, 0x01, 0x22,
};

static const uint8_t one_el[] = {
    0, 0, 1, 0x7e, 0x01, 0x02, 0x01, 0x31,
};

static const uint8_t one_el_output[] = {
    0, 0, 0, 1, 0x02, 0x01, 0x31,
};

static void dispatch(struct mp_dovi_split *split, const uint8_t *data, size_t len,
                     const uint8_t *expected, size_t expected_len,
                     int64_t pos, double pts, double dts, bool keyframe)
{
    struct demux_packet *bl = new_demux_packet(NULL, len);
    assert_true(bl);
    memcpy(bl->buffer, data, len);
    bl->pts = pts;
    bl->dts = dts;
    bl->duration = 0.04;
    bl->pos = pos;
    bl->keyframe = keyframe;
    bl->stream = 0;

    struct demux_packet *el = mp_dovi_split_dispatch(split, bl);
    if (expected) {
        assert_true(el);
        assert_int_equal(el->len, expected_len);
        assert_memcmp(el->buffer, expected, expected_len);
        assert_int_equal(el->stream, mp_dovi_split_el_stream(split)->index);
        assert_true(el->pts == pts);
        assert_true(el->dts == dts);
        assert_true(el->duration == bl->duration);
        assert_int_equal(el->keyframe, keyframe);
        assert_int_equal(el->pos, pos);
    } else {
        assert_false(el);
    }

    assert_int_equal(bl->len, len);
    assert_memcmp(bl->buffer, data, len);
    assert_int_equal(bl->stream, 0);
    assert_int_equal(bl->pos, pos);
    assert_true(bl->pts == pts);
    assert_true(bl->dts == dts);
    assert_int_equal(bl->keyframe, keyframe);
    talloc_free(el);
    talloc_free(bl);
}

int main(void)
{
    if (!av_bsf_get_by_name("dovi_split")) {
        puts("SKIP: libavcodec has no dovi_split bitstream filter");
        return 77;
    }

    struct demuxer *demuxer = talloc_zero(NULL, struct demuxer);
    struct sh_stream *bl = demux_alloc_sh_stream(STREAM_VIDEO);
    talloc_steal(demuxer, bl);
    bl->index = 0;
    bl->codec->codec = "hevc";
    bl->codec->native_tb_num = 1;
    bl->codec->native_tb_den = 1000;

    struct mp_dovi_split *split = mp_dovi_split_create(demuxer, bl);
    assert_true(split);
    assert_int_equal(mp_dovi_split_el_stream(split)->index, 1);

    // MKV HEVC can have no DTS: the inherited file position is then the only
    // packet identity available for demux refresh after an audio-track switch.
    int64_t pos = INT64_C(1) << 33;
    dispatch(split, two_el, sizeof(two_el), two_el_output, sizeof(two_el_output),
             pos, 1.0, MP_NOPTS_VALUE, true);
    dispatch(split, bl_only, sizeof(bl_only), NULL, 0,
             pos + 100, 1.04, MP_NOPTS_VALUE, false);
    dispatch(split, one_el, sizeof(one_el), one_el_output, sizeof(one_el_output),
             pos + 200, 1.08, 1.04, false);

    // A no-output input followed by reset must not retain a pending packet.
    dispatch(split, bl_only, sizeof(bl_only), NULL, 0,
             pos + 300, 1.12, MP_NOPTS_VALUE, false);
    mp_dovi_split_reset(split);
    dispatch(split, two_el, sizeof(two_el), two_el_output, sizeof(two_el_output),
             pos, 0.5, MP_NOPTS_VALUE, true);
    dispatch(split, one_el, sizeof(one_el), one_el_output, sizeof(one_el_output),
             -1, 0.54, 0.5, false);

    talloc_free(demuxer);
    return 0;
}
