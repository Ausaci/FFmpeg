/*
 * Copyright (c) 2002 The FFmpeg Project
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/mem.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "h263.h"
#include "mpegvideo.h"
#include "mpegvideoenc.h"
#include "msmpeg4.h"
#include "msmpeg4enc.h"
#include "msmpeg4data.h"
#include "msmpeg4_vc1_data.h"
#include "put_bits.h"
#include "wmv2.h"

#define WMV2_EXTRADATA_SIZE 4

typedef struct WMV2EncContext {
    MSMPEG4EncContext msmpeg4;
    WMV2Context common;
    int j_type_bit;
    int j_type;
    int abt_flag;
    int abt_type;
    int per_mb_abt;
    int mspel_bit;
    int cbp_table_index;
    int top_left_mv_flag;
    int per_mb_rl_bit;
} WMV2EncContext;

static int encode_ext_header(WMV2EncContext *w)
{
    MPVEncContext *const s = &w->msmpeg4.m.s;
    PutBitContext pb;
    int code;

    init_put_bits(&pb, s->c.avctx->extradata, WMV2_EXTRADATA_SIZE);

    put_bits(&pb, 5, s->c.avctx->time_base.den / s->c.avctx->time_base.num); // yes 29.97 -> 29
    put_bits(&pb, 11, FFMIN(w->msmpeg4.m.bit_rate / 1024, 2047));

    put_bits(&pb, 1, w->mspel_bit        = 1);
    put_bits(&pb, 1, s->loop_filter);
    put_bits(&pb, 1, w->abt_flag         = 1);
    put_bits(&pb, 1, w->j_type_bit       = 1);
    put_bits(&pb, 1, w->top_left_mv_flag = 0);
    put_bits(&pb, 1, w->per_mb_rl_bit    = 1);
    put_bits(&pb, 3, code                = 1);

    flush_put_bits(&pb);

    s->slice_height = s->c.mb_height / code;

    return 0;
}

static int wmv2_encode_picture_header(MPVMainEncContext *const m)
{
    WMV2EncContext *const w = (WMV2EncContext *) m;
    MSMPEG4EncContext *const ms = &w->msmpeg4;
    MPVEncContext *const s = &m->s;

    put_bits_assume_flushed(&s->pb);

    put_bits(&s->pb, 1, s->c.pict_type - 1);
    if (s->c.pict_type == AV_PICTURE_TYPE_I)
        put_bits(&s->pb, 7, 0);
    put_bits(&s->pb, 5, s->c.qscale);

    ms->dc_table_index  = 1;
    ms->mv_table_index  = 1; /* only if P-frame */
    ms->per_mb_rl_table = 0;
    s->c.mspel          = 0;
    w->per_mb_abt      = 0;
    w->abt_type        = 0;
    w->j_type          = 0;

    av_assert0(s->flipflop_rounding);

    if (s->c.pict_type == AV_PICTURE_TYPE_I) {
        av_assert0(s->c.no_rounding == 1);
        if (w->j_type_bit)
            put_bits(&s->pb, 1, w->j_type);

        if (w->per_mb_rl_bit)
            put_bits(&s->pb, 1, ms->per_mb_rl_table);

        if (!ms->per_mb_rl_table) {
            ff_msmpeg4_code012(&s->pb, ms->rl_chroma_table_index);
            ff_msmpeg4_code012(&s->pb, ms->rl_table_index);
        }

        put_bits(&s->pb, 1, ms->dc_table_index);

        s->c.inter_intra_pred = 0;
    } else {
        int cbp_index;

        put_bits(&s->pb, 2, SKIP_TYPE_NONE);

        ff_msmpeg4_code012(&s->pb, cbp_index = 0);
        w->cbp_table_index = wmv2_get_cbp_table_index(s->c.qscale, cbp_index);

        if (w->mspel_bit)
            put_bits(&s->pb, 1, s->c.mspel);

        if (w->abt_flag) {
            put_bits(&s->pb, 1, w->per_mb_abt ^ 1);
            if (!w->per_mb_abt)
                ff_msmpeg4_code012(&s->pb, w->abt_type);
        }

        if (w->per_mb_rl_bit)
            put_bits(&s->pb, 1, ms->per_mb_rl_table);

        if (!ms->per_mb_rl_table) {
            ff_msmpeg4_code012(&s->pb, ms->rl_table_index);
            ms->rl_chroma_table_index = ms->rl_table_index;
        }
        put_bits(&s->pb, 1, ms->dc_table_index);
        put_bits(&s->pb, 1, ms->mv_table_index);

        s->c.inter_intra_pred = 0; // (s->c.width * s->c.height < 320 * 240 && m->bit_rate <= II_BITRATE);
    }
    s->esc3_level_length = 0;
    ms->esc3_run_length  = 0;

    return 0;
}

/* Nearly identical to wmv1 but that is just because we do not use the
 * useless M$ crap features. It is duplicated here in case someone wants
 * to add support for these crap features. */
static void wmv2_encode_mb(MPVEncContext *const s, int16_t block[][64],
                           int motion_x, int motion_y)
{
    WMV2EncContext *const w = (WMV2EncContext *) s;
    int cbp, coded_cbp, i;
    int pred_x, pred_y;
    uint8_t *coded_block;

    ff_msmpeg4_handle_slices(s);

    if (!s->c.mb_intra) {
        /* compute cbp */
        cbp = 0;
        for (i = 0; i < 6; i++)
            if (s->c.block_last_index[i] >= 0)
                cbp |= 1 << (5 - i);

        put_bits(&s->pb,
                 ff_wmv2_inter_table[w->cbp_table_index][cbp + 64][1],
                 ff_wmv2_inter_table[w->cbp_table_index][cbp + 64][0]);

        s->misc_bits += get_bits_diff(s);
        /* motion vector */
        ff_h263_pred_motion(&s->c, 0, 0, &pred_x, &pred_y);
        ff_msmpeg4_encode_motion(&w->msmpeg4, motion_x - pred_x,
                                 motion_y - pred_y);
        s->mv_bits += get_bits_diff(s);
    } else {
        /* compute cbp */
        cbp       = 0;
        coded_cbp = 0;
        for (i = 0; i < 6; i++) {
            int val = (s->c.block_last_index[i] >= 1);

            cbp |= val << (5 - i);
            if (i < 4) {
                /* predict value for close blocks only for luma */
                int pred     = ff_msmpeg4_coded_block_pred(&s->c, i, &coded_block);
                *coded_block = val;
                val          = val ^ pred;
            }
            coded_cbp |= val << (5 - i);
        }

        if (s->c.pict_type == AV_PICTURE_TYPE_I)
            put_bits(&s->pb,
                     ff_msmp4_mb_i_table[coded_cbp][1],
                     ff_msmp4_mb_i_table[coded_cbp][0]);
        else
            put_bits(&s->pb,
                     ff_wmv2_inter_table[w->cbp_table_index][cbp][1],
                     ff_wmv2_inter_table[w->cbp_table_index][cbp][0]);
        put_bits(&s->pb, 1, 0);         /* no AC prediction yet */
        if (s->c.inter_intra_pred) {
            s->c.h263_aic_dir = 0;
            put_bits(&s->pb,
                     ff_table_inter_intra[s->c.h263_aic_dir][1],
                     ff_table_inter_intra[s->c.h263_aic_dir][0]);
        }
        s->misc_bits += get_bits_diff(s);
    }

    for (i = 0; i < 6; i++)
        ff_msmpeg4_encode_block(s, block[i], i);
    if (s->c.mb_intra)
        s->i_tex_bits += get_bits_diff(s);
    else
        s->p_tex_bits += get_bits_diff(s);
}

static av_cold int wmv2_encode_init(AVCodecContext *avctx)
{
    WMV2EncContext *const w = avctx->priv_data;
    MPVEncContext *const s = &w->msmpeg4.m.s;
    int ret;

    w->msmpeg4.m.encode_picture_header = wmv2_encode_picture_header;
    s->encode_mb                       = wmv2_encode_mb;
    s->c.private_ctx = &w->common;
    ret = ff_mpv_encode_init(avctx);
    if (ret < 0)
        return ret;

    ff_wmv2_common_init(&s->c);

    avctx->extradata_size = WMV2_EXTRADATA_SIZE;
    avctx->extradata      = av_mallocz(avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!avctx->extradata)
        return AVERROR(ENOMEM);

    encode_ext_header(w);

    return 0;
}

const FFCodec ff_wmv2_encoder = {
    .p.name         = "wmv2",
    CODEC_LONG_NAME("Windows Media Video 8"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_WMV2,
    .p.priv_class   = &ff_mpv_enc_class,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .priv_data_size = sizeof(WMV2EncContext),
    .init           = wmv2_encode_init,
    FF_CODEC_ENCODE_CB(ff_mpv_encode_picture),
    .close          = ff_mpv_encode_end,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .color_ranges   = AVCOL_RANGE_MPEG,
    CODEC_PIXFMTS(AV_PIX_FMT_YUV420P),
};
