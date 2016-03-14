#include <ngx_http.h>
#include <ngx_md5.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/dash/dash_packager.h"
#include "vod/dash/edash_packager.h"
#include "vod/mkv/mkv_builder.h"
#include "vod/udrm.h"

// constants
#define SUPPORTED_CODECS_MP4 (VOD_CODEC_FLAG(AVC) | VOD_CODEC_FLAG(HEVC) | VOD_CODEC_FLAG(AAC))
#define SUPPORTED_CODECS_WEBM (VOD_CODEC_FLAG(VP8) | VOD_CODEC_FLAG(VP9) | VOD_CODEC_FLAG(VORBIS) | VOD_CODEC_FLAG(OPUS))
#define SUPPORTED_CODECS (SUPPORTED_CODECS_MP4 | SUPPORTED_CODECS_WEBM)
#define TIMESCALE (10000)

ngx_conf_enum_t  dash_manifest_formats[] = {
	{ ngx_string("segmentlist"), FORMAT_SEGMENT_LIST },
	{ ngx_string("segmenttemplate"), FORMAT_SEGMENT_TEMPLATE },
	{ ngx_string("segmenttimeline"), FORMAT_SEGMENT_TIMELINE },
	{ ngx_null_string, 0 }
};

// content types
static u_char mpd_content_type[] = "application/dash+xml";
static u_char mp4_audio_content_type[] = "audio/mp4";
static u_char mp4_video_content_type[] = "video/mp4";
static u_char webm_audio_content_type[] = "audio/webm";
static u_char webm_video_content_type[] = "video/webm";

// file extensions
static const u_char manifest_file_ext[] = ".mpd";
static const u_char init_segment_file_ext[] = ".mp4";
static const u_char fragment_file_ext[] = ".m4s";
static const u_char webm_file_ext[] = ".webm";

static ngx_int_t 
ngx_http_vod_dash_handle_manifest(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	ngx_str_t base_url = ngx_null_string;
	vod_status_t rc;
	ngx_str_t file_uri;

	if (submodule_context->conf->dash.absolute_manifest_urls)
	{
		if (submodule_context->conf->dash.mpd_config.manifest_format == FORMAT_SEGMENT_LIST)
		{
			file_uri.data = NULL;
			file_uri.len = 0;
		}
		else
		{
			file_uri = submodule_context->r->uri;
		}

		ngx_http_vod_get_base_url(submodule_context->r, &submodule_context->conf->https_header_name, NULL, 0, &file_uri, &base_url);
	}

	if (submodule_context->conf->drm_enabled)
	{
		rc = edash_packager_build_mpd(
			&submodule_context->request_context,
			&submodule_context->conf->dash.mpd_config,
			&base_url,
			&submodule_context->media_set,
			response);
	}
	else
	{
		rc = dash_packager_build_mpd(
			&submodule_context->request_context,
			&submodule_context->conf->dash.mpd_config,
			&base_url,
			&submodule_context->media_set,
			0,
			NULL,
			NULL,
			response);
	}

	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_handle_manifest: (e)dash_packager_build_mpd failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	content_type->data = mpd_content_type;
	content_type->len = sizeof(mpd_content_type) - 1;

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_mp4_handle_init_segment(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;

	if (submodule_context->conf->drm_enabled)
	{
		rc = edash_packager_build_init_mp4(
			&submodule_context->request_context,
			&submodule_context->media_set,
			submodule_context->conf->drm_clear_lead_segment_count > 0,
			ngx_http_vod_submodule_size_only(submodule_context),
			response);
	}
	else
	{
		rc = dash_packager_build_init_mp4(
			&submodule_context->request_context,
			&submodule_context->media_set,
			ngx_http_vod_submodule_size_only(submodule_context),
			NULL,
			NULL,
			response);
	}

	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_mp4_handle_init_segment: (e)dash_packager_build_init_mp4 failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	if (submodule_context->media_set.track_count[MEDIA_TYPE_VIDEO] != 0)
	{
		content_type->data = mp4_video_content_type;
		content_type->len = sizeof(mp4_video_content_type) - 1;
	}
	else
	{
		content_type->data = mp4_audio_content_type;
		content_type->len = sizeof(mp4_audio_content_type) - 1;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_mp4_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	dash_fragment_header_extensions_t header_extensions;
	fragment_writer_state_t* state;
	segment_writer_t edash_writer;
	vod_status_t rc;
	bool_t reuse_buffers = FALSE;
	bool_t size_only = ngx_http_vod_submodule_size_only(submodule_context);

	if (submodule_context->conf->drm_enabled && 
		submodule_context->request_params.segment_index >= submodule_context->conf->drm_clear_lead_segment_count)
	{
		// encyrpted fragment
		rc = edash_packager_get_fragment_writer(
			&edash_writer,
			&submodule_context->request_context,
			&submodule_context->media_set,
			submodule_context->request_params.segment_index,
			segment_writer,
			submodule_context->media_set.sequences[0].encryption_key,		// iv
			size_only,
			output_buffer,
			response_size);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_dash_mp4_init_frame_processor: edash_packager_get_fragment_writer failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		if (edash_writer.write_tail != NULL)
		{
			segment_writer = &edash_writer;
			reuse_buffers = TRUE;		// mp4_encrypt allocates new buffers
		}
	}
	else
	{
		// unencrypted
		ngx_memzero(&header_extensions, sizeof(header_extensions));

		rc = dash_packager_build_fragment_header(
			&submodule_context->request_context,
			&submodule_context->media_set,
			submodule_context->request_params.segment_index,
			submodule_context->conf->drm_enabled ? 2 : 0,	// sample description index
			&header_extensions,
			size_only,
			output_buffer,
			response_size);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_dash_mp4_init_frame_processor: dash_packager_build_fragment_header failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}
	}

	// initialize the frame processor
	if (!size_only || *response_size == 0)
	{
		rc = mp4_builder_frame_writer_init(
			&submodule_context->request_context,
			submodule_context->media_set.sequences,
			segment_writer->write_tail,
			segment_writer->context,
			reuse_buffers,
			&state);
		if (rc != VOD_OK)
		{
			ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
				"ngx_http_vod_dash_mp4_init_frame_processor: mp4_builder_frame_writer_init failed %i", rc);
			return ngx_http_vod_status_to_ngx_error(rc);
		}

		*frame_processor = (ngx_http_vod_frame_processor_t)mp4_builder_frame_writer_process;
		*frame_processor_state = state;
	}

	// set the 'Content-type' header
	if (submodule_context->media_set.track_count[MEDIA_TYPE_VIDEO] != 0)
	{
		content_type->len = sizeof(mp4_video_content_type) - 1;
		content_type->data = (u_char *)mp4_video_content_type;
	}
	else
	{
		content_type->len = sizeof(mp4_audio_content_type) - 1;
		content_type->data = (u_char *)mp4_audio_content_type;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_webm_handle_init_segment(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	vod_status_t rc;
	ngx_md5_t md5;
	ngx_str_t* uri;
	uint64_t track_uid;
	u_char uri_key[16];

	// calculate a uid for track based on the request uri
	uri = &submodule_context->r->uri;
	ngx_md5_init(&md5);
	ngx_md5_update(&md5, uri->data, uri->len);
	ngx_md5_final(uri_key, &md5);
	ngx_memcpy(&track_uid, uri_key, sizeof(track_uid));

	rc = mkv_build_init_segment(
		&submodule_context->request_context,
		submodule_context->media_set.sequences[0].filtered_clips[0].first_track,
		track_uid,
		response);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_webm_handle_init_segment: mkv_build_init_segment failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	if (submodule_context->media_set.track_count[MEDIA_TYPE_VIDEO] != 0)
	{
		content_type->data = webm_video_content_type;
		content_type->len = sizeof(webm_video_content_type) - 1;
	}
	else
	{
		content_type->data = webm_audio_content_type;
		content_type->len = sizeof(webm_audio_content_type) - 1;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_webm_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	vod_status_t rc;

	rc = mkv_builder_frame_writer_init(
		&submodule_context->request_context,
		submodule_context->media_set.sequences,
		segment_writer->write_tail,
		segment_writer->context,
		FALSE,
		output_buffer,
		response_size,
		frame_processor_state);
	if (rc != VOD_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, submodule_context->request_context.log, 0,
			"ngx_http_vod_dash_webm_init_frame_processor: mkv_builder_frame_writer_init failed %i", rc);
		return ngx_http_vod_status_to_ngx_error(rc);
	}

	*frame_processor = (ngx_http_vod_frame_processor_t)mkv_builder_frame_writer_process;

	// set the 'Content-type' header
	if (submodule_context->media_set.track_count[MEDIA_TYPE_VIDEO] != 0)
	{
		content_type->len = sizeof(webm_video_content_type) - 1;
		content_type->data = (u_char *)webm_video_content_type;
	}
	else
	{
		content_type->len = sizeof(webm_audio_content_type) - 1;
		content_type->data = (u_char *)webm_audio_content_type;
	}

	return NGX_OK;
}

static const ngx_http_vod_request_t dash_manifest_request = {
	REQUEST_FLAG_TIME_DEPENDENT_ON_LIVE,
	PARSE_FLAG_DURATION_LIMITS_AND_TOTAL_SIZE | PARSE_FLAG_CODEC_NAME,
	REQUEST_CLASS_MANIFEST,
	SUPPORTED_CODECS,
	TIMESCALE,
	ngx_http_vod_dash_handle_manifest,
	NULL,
};

static const ngx_http_vod_request_t dash_mp4_init_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_BASIC_METADATA_ONLY | PARSE_FLAG_SAVE_RAW_ATOMS,
	REQUEST_CLASS_OTHER,
	SUPPORTED_CODECS_MP4,
	TIMESCALE,
	ngx_http_vod_dash_mp4_handle_init_segment,
	NULL,
};

static const ngx_http_vod_request_t dash_mp4_fragment_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_FLAG_FRAMES_ALL,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS_MP4,
	TIMESCALE,
	NULL,
	ngx_http_vod_dash_mp4_init_frame_processor,
};

static const ngx_http_vod_request_t edash_mp4_fragment_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_PARSED_EXTRA_DATA,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS_MP4,
	TIMESCALE,
	NULL,
	ngx_http_vod_dash_mp4_init_frame_processor,
};

static const ngx_http_vod_request_t dash_webm_init_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_BASIC_METADATA_ONLY,
	REQUEST_CLASS_OTHER,
	SUPPORTED_CODECS_WEBM,
	TIMESCALE,
	ngx_http_vod_dash_webm_handle_init_segment,
	NULL,
};

static const ngx_http_vod_request_t dash_webm_fragment_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_FLAG_FRAMES_ALL,
	REQUEST_CLASS_SEGMENT,
	SUPPORTED_CODECS_WEBM,
	TIMESCALE,
	NULL,
	ngx_http_vod_dash_webm_init_frame_processor,
};

static void
ngx_http_vod_dash_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_dash_loc_conf_t *conf)
{
	conf->absolute_manifest_urls = NGX_CONF_UNSET;
	conf->mpd_config.manifest_format = NGX_CONF_UNSET_UINT;
	conf->mpd_config.duplicate_bitrate_threshold = NGX_CONF_UNSET_UINT;
}

static char *
ngx_http_vod_dash_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_dash_loc_conf_t *conf,
	ngx_http_vod_dash_loc_conf_t *prev)
{
	ngx_conf_merge_value(conf->absolute_manifest_urls, prev->absolute_manifest_urls, 1);

	ngx_conf_merge_str_value(conf->manifest_file_name_prefix, prev->manifest_file_name_prefix, "manifest");
	ngx_conf_merge_str_value(conf->mpd_config.profiles, prev->mpd_config.profiles, "urn:mpeg:dash:profile:isoff-main:2011");
	ngx_conf_merge_str_value(conf->mpd_config.init_file_name_prefix, prev->mpd_config.init_file_name_prefix, "init");
	ngx_conf_merge_str_value(conf->mpd_config.fragment_file_name_prefix, prev->mpd_config.fragment_file_name_prefix, "fragment");
	ngx_conf_merge_uint_value(conf->mpd_config.manifest_format, prev->mpd_config.manifest_format, FORMAT_SEGMENT_TIMELINE);
	ngx_conf_merge_uint_value(conf->mpd_config.duplicate_bitrate_threshold, prev->mpd_config.duplicate_bitrate_threshold, 4096);

	return NGX_CONF_OK;
}

static int
ngx_http_vod_dash_get_file_path_components(ngx_str_t* uri)
{
	return 1;
}

static ngx_int_t
ngx_http_vod_dash_parse_uri_file_name(
	ngx_http_request_t *r,
	ngx_http_vod_loc_conf_t *conf,
	u_char* start_pos,
	u_char* end_pos,
	request_params_t* request_params,
	const ngx_http_vod_request_t** request)
{
	ngx_int_t rc;
	bool_t expect_segment_index;

	// fragment
	if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.fragment_file_name_prefix, fragment_file_ext))
	{
		start_pos += conf->dash.mpd_config.fragment_file_name_prefix.len;
		end_pos -= (sizeof(fragment_file_ext) - 1);
		*request = conf->drm_enabled ? &edash_mp4_fragment_request : &dash_mp4_fragment_request;
		expect_segment_index = TRUE;
	}
	// init segment
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.init_file_name_prefix, init_segment_file_ext))
	{
		start_pos += conf->dash.mpd_config.init_file_name_prefix.len;
		end_pos -= (sizeof(init_segment_file_ext) - 1);
		*request = &dash_mp4_init_request;
		expect_segment_index = FALSE;
	}
	// webm fragment
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.fragment_file_name_prefix, webm_file_ext))
	{
		start_pos += conf->dash.mpd_config.fragment_file_name_prefix.len;
		end_pos -= (sizeof(webm_file_ext) - 1);
		*request = &dash_webm_fragment_request;
		expect_segment_index = TRUE;
	}
	// webm init segment
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.mpd_config.init_file_name_prefix, webm_file_ext))
	{
		start_pos += conf->dash.mpd_config.init_file_name_prefix.len;
		end_pos -= (sizeof(webm_file_ext) - 1);
		*request = &dash_webm_init_request;
		expect_segment_index = FALSE;
	}
	// manifest
	else if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->dash.manifest_file_name_prefix, manifest_file_ext))
	{
		start_pos += conf->dash.manifest_file_name_prefix.len;
		end_pos -= (sizeof(manifest_file_ext) - 1);
		*request = &dash_manifest_request;
		expect_segment_index = FALSE;
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_dash_parse_uri_file_name: unidentified request");
		return NGX_HTTP_BAD_REQUEST;
	}

	// parse the required tracks string
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, expect_segment_index, request_params);
	if (rc != NGX_OK)
	{
		ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
			"ngx_http_vod_dash_parse_uri_file_name: ngx_http_vod_parse_uri_file_name failed %i", rc);
		return rc;
	}

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_dash_parse_drm_info(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* drm_info,
	void** output)
{
	vod_status_t rc;

	rc = udrm_parse_response(
		&submodule_context->request_context,
		drm_info,
		TRUE,
		output);
	if (rc != VOD_OK)
	{
		return NGX_ERROR;
	}

	return NGX_OK;
}

DEFINE_SUBMODULE(dash);
