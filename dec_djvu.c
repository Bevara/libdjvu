/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / DjVu decoder filter, based on DjVuLibre
 *  (https://djvu.sourceforge.net/) through its ddjvu C API.
 *
 *  DjVu is a document format: a file holds one or more pages, each one a mix of
 *  a bitonal mask and compressed background/foreground layers. The first page
 *  is rendered to RGB here, matching the single-image convention the other
 *  document filter in this repo (poppler/PDF) follows.
 *
 *  ddjvu is asynchronous by design - decoding advances as data arrives and
 *  reports through a message queue. The whole file is already in memory here,
 *  so it is pushed in one go and the queue drained until the page is ready.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <libdjvu/ddjvuapi.h>

/* DjVuLibre signals failures by throwing, and no solver exports __cxa_throw
 * (checked on solver_1 and solver_minimal_1), so the side module would not
 * instantiate without this definition. A file that makes the library throw ends
 * the module rather than coming back as an error; well formed documents never
 * reach it. Same stub as libcharls and libape.
 *
 * The rest of what this module needs - iconv, the pthread stubs, getpwuid and
 * friends - is exported by solver_1 only, which is why the test and the demo
 * use it rather than solver_minimal_1. */
void __cxa_throw(void *thrown_exception, void *tinfo, void (*dest)(void *))
{
	GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DJVUDec] DjVuLibre raised an exception, aborting the module\n"));
	abort();
}

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
} GF_DJVUDecCtx;

/* Drains the message queue, which is also how decoding errors surface.
 * Returns GF_FALSE if the library reported a fatal error.
 *
 * Deliberately never calls ddjvu_message_wait: that call blocks until a
 * message arrives, and in this single-threaded wasm build nothing else can
 * produce one, so it hangs the page for good. All the data is pushed up front,
 * so polling is enough. */
static Bool djvudec_handle_messages(ddjvu_context_t *ctx)
{
	const ddjvu_message_t *msg;
	Bool ok = GF_TRUE;

	while ((msg = ddjvu_message_peek(ctx)) != NULL)
	{
		if (msg->m_any.tag == DDJVU_ERROR)
		{
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DJVUDec] %s\n",
				msg->m_error.message ? msg->m_error.message : "decoding error"));
			ok = GF_FALSE;
		}
		ddjvu_message_pop(ctx);
	}
	return ok;
}

static GF_Err djvudec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_DJVUDecCtx *ctx = (GF_DJVUDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_RGB));

	return GF_OK;
}

static Bool djvudec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_DJVUDecCtx *ctx = (GF_DJVUDecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static GF_Err djvudec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size, out_size, width, height, guard;
	ddjvu_context_t *dctx = NULL;
	ddjvu_document_t *doc = NULL;
	ddjvu_page_t *page = NULL;
	ddjvu_format_t *fmt = NULL;
	ddjvu_rect_t prect, rrect;
	GF_Err e = GF_NON_COMPLIANT_BITSTREAM;
	GF_DJVUDecCtx *ctx = (GF_DJVUDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	dctx = ddjvu_context_create("gpac-djvudec");
	if (!dctx)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	/* NULL url + cache: the data is fed by hand on stream 0 rather than read
	 * from a file, which is the documented way to decode from memory. */
	doc = ddjvu_document_create(dctx, NULL, 1);
	if (!doc)
		goto exit;

	ddjvu_stream_write(doc, 0, (const char *)data, size);
	ddjvu_stream_close(doc, 0, 0);

	/* Bounded loops: if the library ever stops making progress the filter
	 * gives up instead of spinning for ever. */
	guard = 0;
	while (!ddjvu_document_decoding_done(doc) && (guard++ < 100000))
	{
		if (!djvudec_handle_messages(dctx))
			goto exit;
	}
	if (!ddjvu_document_decoding_done(doc))
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DJVUDec] Document never finished decoding\n"));
		goto exit;
	}
	if (ddjvu_document_decoding_error(doc))
		goto exit;

	if (ddjvu_document_get_pagenum(doc) < 1)
		goto exit;

	page = ddjvu_page_create_by_pageno(doc, 0);
	if (!page)
		goto exit;

	guard = 0;
	while (!ddjvu_page_decoding_done(page) && (guard++ < 100000))
	{
		if (!djvudec_handle_messages(dctx))
			goto exit;
	}
	if (!ddjvu_page_decoding_done(page))
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DJVUDec] Page never finished decoding\n"));
		goto exit;
	}
	if (ddjvu_page_decoding_error(page))
		goto exit;

	width = (u32)ddjvu_page_get_width(page);
	height = (u32)ddjvu_page_get_height(page);
	if (!width || !height)
		goto exit;

	/* DDJVU_FORMAT_RGB24 writes three bytes per pixel in R,G,B order;
	 * row order has to be flipped, ddjvu numbering rows from the bottom. */
	fmt = ddjvu_format_create(DDJVU_FORMAT_RGB24, 0, NULL);
	if (!fmt)
		goto exit;
	ddjvu_format_set_row_order(fmt, 1);

	out_size = width * height * 3;

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(width));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(height));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, &PROP_UINT(width * 3));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_RGB));

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
	if (!dst_pck)
	{
		e = GF_OUT_OF_MEM;
		goto exit;
	}
	memset(output, 0xFF, out_size);

	prect.x = 0; prect.y = 0; prect.w = width; prect.h = height;
	rrect = prect;
	if (!ddjvu_page_render(page, DDJVU_RENDER_COLOR, &prect, &rrect, fmt, width * 3, (char *)output))
	{
		gf_filter_pck_discard(dst_pck);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DJVUDec] Failed to render page 1\n"));
		goto exit;
	}

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);
	e = GF_EOS;

exit:
	if (fmt)
		ddjvu_format_release(fmt);
	if (page)
		ddjvu_page_release(page);
	if (doc)
		ddjvu_document_release(doc);
	if (dctx)
		ddjvu_context_release(dctx);
	gf_filter_pid_drop_packet(ctx->ipid);

	if (e == GF_EOS)
		gf_filter_pid_set_eos(ctx->opid);
	return e;
}

static void djvudec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability DJVUDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "djvu|djv"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "image/vnd.djvu|image/x-djvu"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister DJVUDecoderRegister = {
	.name = "djvudec",
	GF_FS_SET_DESCRIPTION("DjVu document decoder")
		GF_FS_SET_HELP("This filter renders the first page of a DjVu document to a raw image using DjVuLibre.")
			.private_size = sizeof(GF_DJVUDecCtx),
	SETCAPS(DJVUDecCaps),
	.configure_pid = djvudec_configure_pid,
	.process = djvudec_process,
	.process_event = djvudec_process_event,
	.finalize = djvudec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE djvudec_register(GF_FilterSession *session)
{
	return &DJVUDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_djvudec(void) {
    gf_filter_auto_register("djvudec", djvudec_register);
}
