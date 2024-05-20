#include "ctx.h"
#include "vpss_helper.h"
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cvi_isp.h"
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string.h>
#include "cvi_isp.h"
#include "cvi_comm_isp.h"
#include "utils.h"
#include <chrono>
#include <math.h>

#define TEAISPPQ_LIB "libcvi_tdl.so"
#define USE_VI_RAW
#define TEAISP_PQ_SLEEP_TIME 200000

#define LOAD_SYMBOL(dl, sym, type, fn) \
do { \
	if (NULL == fn && NULL == (fn = (type)dlsym(dl, sym))) { \
		printf("load symbol %s fail, %s\n", sym, dlerror()); \
		return -1; \
	} \
} while(0);

static const char *teaisppq_scene_str[5] = {
	"SNOW",
	"FOG",
	"BACKLIGHT",
	"GRASS",
	"COMMON"
};

static CVI_U64 inference_count;

static int load_teaisppq_symbol(SERVICE_CTX *ctx)
{
	if (!ctx->ai_dl) {
		std::cout << "Loading " TEAISPPQ_LIB " ..." << std::endl;
		ctx->ai_dl = dlopen(TEAISPPQ_LIB, RTLD_LAZY);

		if (!ctx->ai_dl) {
			std::cout << "dlopen " TEAISPPQ_LIB " fail: " << std::endl;
			std::cout << dlerror() << std::endl;
			return -1;
		}
	}

	for (int idx=0; idx<ctx->dev_num; idx++) {
		SERVICE_CTX_ENTITY *ent = &ctx->entity[idx];
		// common
		LOAD_SYMBOL(ctx->ai_dl, "CVI_TDL_CreateHandle2", AI_CreateHandle2, ent->ai_create_handle2);
		LOAD_SYMBOL(ctx->ai_dl, "CVI_TDL_CreateHandle", AI_CreateHandle, ent->ai_create_handle);
		LOAD_SYMBOL(ctx->ai_dl, "CVI_TDL_DestroyHandle", AI_DestroyHandle, ent->ai_destroy_handle);
		LOAD_SYMBOL(ctx->ai_dl, "CVI_TDL_SetSkipVpssPreprocess", AI_SetSkipVpssPreprocess, ent->ai_set_skip_vpss_preprocess);
		LOAD_SYMBOL(ctx->ai_dl, "CVI_TDL_OpenModel", AI_OpenModel, ent->ai_open_model);
		LOAD_SYMBOL(ctx->ai_dl, "CVI_TDL_Service_CreateHandle", AI_Service_CreateHandle, ent->ai_service_create_handle);
		LOAD_SYMBOL(ctx->ai_dl, "CVI_TDL_Service_DestroyHandle", AI_Service_DestroyHandle, ent->ai_service_destroy_handle);
		// scene classification
		LOAD_SYMBOL(ctx->ai_dl, "CVI_TDL_Set_Raw_Image_Cls_Param", AI_Set_Image_Cls_Param, ent->ai_set_img_cls_param);
		LOAD_SYMBOL(ctx->ai_dl, "CVI_TDL_Raw_Image_Classification", AI_Image_Cls, ent->ai_img_cls);
		LOAD_SYMBOL(ctx->ai_dl, "CVI_TDL_Service_ObjectWriteText", AI_Service_ObjectWriteText, ent->ai_write_text);

	}

	return 0;
}

static void set_teaisppq_scene(SERVICE_CTX_ENTITY *ent, cvtdl_class_meta_t &cls_meta)
{
	TEAISP_PQ_SCENE_INFO scene_info;

	switch (cls_meta.cls[0]) {
	case 0:
		scene_info.scene = SCENE_SNOW;
		break;
	case 1:
		scene_info.scene = SCENE_FOG;
		break;
	case 2:
		scene_info.scene = SCENE_BACKLIGHT;
		break;
	case 3:
		scene_info.scene = SCENE_GRASS;
		break;
	default:
		scene_info.scene = SCENE_COMMON;
		break;
	}

	for (int i = 0; i < 5; i++) {
		int scene_id = cls_meta.cls[i];
		float score = cls_meta.score[i];
		//std::cout << scene_id << ": " << score << std::endl;
		scene_info.scene_score[scene_id] = score * 100;
	}

	CVI_TEAISP_PQ_SetSceneInfo(ent->ViPipe, &scene_info);
}

int run_teaisppq_vi(SERVICE_CTX_ENTITY *ent)
{
	int ret = -1;
	int frmNum = 1;
	std::string ret_name;
	cvtdl_class_meta_t cls_meta = {0};
	VI_PIPE pipe = ent->ViPipe;
	VIDEO_FRAME_INFO_S stVideoFrame[2] = {};

	get_vi_raw(pipe, stVideoFrame, &frmNum);

	// inference
	// TODO: force set the 1, ignore the se frame
	frmNum = 1;
	for (int i = 0; i < frmNum; i++) {
		++inference_count;
		//std::cout << "---------- do the inference cnt: " << ++inference_count << " ----------" << std::endl;
		ent->ai_set_skip_vpss_preprocess(ent->teaisppq_handle, CVI_TDL_SUPPORTED_MODEL_RAW_IMAGE_CLASSIFICATION, true);

		auto start_time = std::chrono::high_resolution_clock::now();

		ret = ent->ai_img_cls(ent->teaisppq_handle, stVideoFrame + i, &cls_meta);

		auto end_time = std::chrono::high_resolution_clock::now();
		auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
		//std::cout << "inference time: " << duration_ms.count() << " ms" << std::endl;

		if (ret != CVI_SUCCESS) {
			std::cout << "Error: inference return type: " << ret << std::endl;
			CLASS_FREE(&cls_meta);
			goto INFERENCE_FAIL;
		}
	}

	set_teaisppq_scene(ent, cls_meta);

	// dump raw, keep here
	if (access("dump_raw_12_bits", F_OK) == 0) {
		// string format: scene-0_20-1_20-2_20-3_20-4_20-cnt_1.raw
		int int_cls_score[5] = {0};
		std::string ret_name = teaisppq_scene_str[cls_meta.cls[0]] + std::string("-");

		for (int i = 0; i < 5; ++i) {
			int_cls_score[cls_meta.cls[i]] = cls_meta.score[i] * 100;
		}

		for (int i = 0; i < 5; ++i) {
			ret_name += std::to_string(i) + "_" + std::to_string(int_cls_score[i]);
			if (i != 4) {
				ret_name += "-";
			}
		}
		ret_name +=  "-cnt" + std::to_string(inference_count) + ".raw";
		dump_raw_with_ret(stVideoFrame, frmNum, ret_name.c_str(), RAW_12_BIT);
		std::cout << "dump raw: " << ret_name << std::endl;

	} else if (access("dump_raw_16_bits", F_OK) == 0) {
		dump_raw(stVideoFrame, frmNum, RAW_16_BIT);
	}

	CLASS_FREE(&cls_meta);

INFERENCE_FAIL:
	// release raw
	CVI_VI_ReleasePipeFrame(pipe, stVideoFrame);

	return ret;
}

int init_teaisppq(SERVICE_CTX *ctx)
{
	int ret = 0;

	for (int idx = 0; idx < ctx->dev_num; idx++) {
		SERVICE_CTX_ENTITY *ent = &ctx->entity[idx];

		if (!ent->enableTeaisppq) {
			continue;
		}

		if (0 > load_teaisppq_symbol(ctx)) {
			printf("load_teaisppq_symbol fail\n");
		}

		if (ent->ai_create_handle(&ent->teaisppq_handle) != CVI_SUCCESS) {
			printf("Create teaisppq handle failed!\n");
			return -1;
		}

		// TODO:setup preprocess, may remove
		VpssPreParam p_preprocess_cfg;
		float mean[3] = {123.675, 116.28, 103.52};
		float std[3] = {58.395, 57.12, 57.375};

		for (int i = 0; i < 3; i++) {
			p_preprocess_cfg.mean[i] = mean[i] / std[i];
			p_preprocess_cfg.factor[i] = 1.0 / std[i];
		}

		p_preprocess_cfg.format = PIXEL_FORMAT_RGB_888_PLANAR;
		p_preprocess_cfg.use_quantize_scale = true;
		p_preprocess_cfg.rescale_type = RESCALE_CENTER;
		p_preprocess_cfg.keep_aspect_ratio = true;

		ret = ent->ai_set_img_cls_param(ent->teaisppq_handle, &p_preprocess_cfg);

		if (ret != CVI_SUCCESS) {
			printf("Can not set image classification parameters %#x\n", ret);
		}

		// open model
		struct stat st = {0};

		if (0 == strlen(ctx->teaisppq_model_path) || 0 != stat(ctx->teaisppq_model_path, &st)) {
			printf("scene classification model %s not found\n", ctx->teaisppq_model_path);
			return -1;
		}

		if (ent->ai_open_model(ent->teaisppq_handle, CVI_TDL_SUPPORTED_MODEL_RAW_IMAGE_CLASSIFICATION, ctx->teaisppq_model_path) != CVI_SUCCESS) {
			printf("CVI_AI_SetModelPath: %s failed!\n", ctx->teaisppq_model_path);
			return -1;
		}

		ent->ai_set_skip_vpss_preprocess(ent->teaisppq_handle, CVI_TDL_SUPPORTED_MODEL_RAW_IMAGE_CLASSIFICATION, true);
	}

	ctx->tdl_ref_count++;

	return ret;
}

void deinit_teaisppq(SERVICE_CTX *ctx)
{
	for (int idx = 0; idx < ctx->dev_num; idx++) {
		SERVICE_CTX_ENTITY *ent = &ctx->entity[idx];

		if (!ent->enableTeaisppq) {
			continue;
		}

		if (ent->teaisppq_handle != NULL) {
			ent->ai_destroy_handle(ent->teaisppq_handle);
		}
	}

	if (ctx->ai_dl && --ctx->tdl_ref_count <= 0) {
		dlclose(ctx->ai_dl);
		ctx->ai_dl = NULL;
	}
}

void *teaisppqTask(void *arg)
{
	int rc = -1;
	SERVICE_CTX_ENTITY *ent = (SERVICE_CTX_ENTITY *)arg;

	while (ent->running && ent->enableTeaisppq) {
		pthread_mutex_lock(&ent->teaisppq_mutex);
		//std::cout << "run the teaisppq ..." << std::endl;
#ifdef USE_VI_RAW
		rc = run_teaisppq_vi(ent);
		usleep(TEAISP_PQ_SLEEP_TIME);
#else
		//std::cout << "not run teaisppq ..." << std::endl;
#endif
		if (0 != rc) {
			std::cout << "fail to run the teaisppq" << std::endl;
			break;
		}

		pthread_mutex_unlock(&ent->teaisppq_mutex);
	}

	return NULL;
}

