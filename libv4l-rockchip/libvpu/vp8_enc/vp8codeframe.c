/*------------------------------------------------------------------------------
--                                                                            --
--       This software is confidential and proprietary and may be used        --
--        only as expressly authorized by a licensing agreement from          --
--                                                                            --
--                            Hantro Products Oy.                             --
--                                                                            --
--                   (C) COPYRIGHT 2006 HANTRO PRODUCTS OY                    --
--                            ALL RIGHTS RESERVED                             --
--                                                                            --
--                 The entire notice above must be reproduced                 --
--                  on all copies and should not be removed.                  --
--                                                                            --
--------------------------------------------------------------------------------
*/

#include <memory.h>
#include "enccommon.h"
#include "encasiccontroller.h"
#include "vp8codeframe.h"
#include "vp8ratecontrol.h"
#include "vp8header.h"
#include "vp8entropy.h"

/* Intra 16x16 mode tree penalty values */
static const int32_t const intra16ModeTreePenalty[] = {
  305, 841, 914, 1082
};


/* Intra 4x4 mode tree penalty values */
static const int32_t const intra4ModeTreePenalty[] = {
  280, 622, 832, 1177, 1240, 1341, 1085, 1259, 1357, 1495
};

/* This is visually fitted. TODO use GNUPLOT or octave to fit curve at
 * given data. ~round((2*(2+exp((x+22)/39)) + (2+exp((x+15)/32)))/3) */
const int32_t const weight[128] = {
  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
  4,  4,  4,  4,  4,  5,  5,  5,  5,  5,
  5,  5,  5,  5,  5,  5,  5,  6,  6,  6,
  6,  6,  6,  6,  6,  6,  6,  7,  7,  7,
  7,  7,  7,  7,  7,  8,  8,  8,  8,  8,
  8,  8,  9,  9,  9,  9,  9, 10, 10, 10,
  10, 11, 11, 11, 12, 12, 13, 13, 13, 13,
  14, 14, 14, 14, 15, 15, 15, 16, 16, 17,
  17, 18, 18, 19, 19, 20, 20, 20, 21, 22,
  23, 23, 24, 24, 25, 25, 26, 27, 28, 28,
  29, 30, 31, 32, 32, 33, 34, 35, 36, 37,
  38, 39, 40, 41, 42, 44, 44, 46, 47, 48,
  50, 51, 52, 54, 55, 57, 58, 61
};

/* experimentally fitted, 24.893*exp(0.02545*qp) */
const int32_t vp8SplitPenalty[128] = {
  24, 25, 26, 26, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 36,
  37, 38, 39, 40, 41, 42, 43, 44, 45, 47, 48, 49, 50, 52, 53, 54,
  56, 57, 59, 60, 62, 63, 65, 67, 68, 70, 72, 74, 76, 78, 80, 82,
  84, 86, 88, 91, 93, 95, 98, 100, 103, 106, 108, 111, 114, 117, 120, 123,
  126, 130, 133, 136, 140, 144, 147, 151, 155, 159, 163, 167, 172, 176, 181, 185,
  190, 195, 200, 205, 211, 216, 222, 227, 233, 239, 245, 252, 258, 265, 272, 279,
  286, 293, 301, 309, 317, 325, 333, 342, 351, 360, 369, 379, 388, 398, 409, 419,
  430, 441, 453, 464, 476, 488, 501, 514, 527, 541, 555, 569, 584, 599, 614, 630
};

static void VP8SetNewFrame(vp8Instance_s* inst);
static void SetIntraPredictionPenalties(regValues_s* regs, uint32_t qp);
static void SetSegmentation(vp8Instance_s* inst);
static void SetFilterParameters(vp8Instance_s* inst);

void VP8SetFrameParams(vp8Instance_s* inst) {
  pps* pps = inst->ppss.pps;  /* Active picture parameters */
  sps* sps = &inst->sps;
  int32_t qp = inst->rateControl.qpHdr;
  int32_t i;

  /* Segment parameters, testId/ROI may override qpSgm and
   * auto filter level setting may override levelSgm. */
  for (i = 0; i < SGM_CNT; i++) {
    pps->qpSgm[i] = qp;
    pps->levelSgm[i] = sps->filterLevel;
  }
}

void VP8CodeFrame(vp8Instance_s* inst, EncoderParameters* cml) {
  /* Initialize probability tables for frame header. */
  InitEntropy(inst);
  SetSegmentation(inst);
  SetFilterParameters(inst);

  /* Write frame headers, also updates segmentation probs. */
  VP8FrameHeader(inst);
  VP8FrameTag(inst);
  VP8SetNewFrame(inst);

  /* Write final probability tables for ASIC. */
  WriteEntropyTables(inst);

  VP8_EncAsicFrameStart(&inst->asic.regs);
}

void VP8SetNewFrame(vp8Instance_s* inst) {
  regValues_s* regs = &inst->asic.regs;
  sps* sps = &inst->sps;
  int32_t qp, i;

  /* We tell HW the size of DCT partition buffers, they all are equal size.
   * There is no overflow check for control partition on ASIC, but since
   * the stream buffers are in one linear memory the overflow of control
   * partition will only corrupt the first DCT partition and SW will
   * notice this and discard the frame. */
  regs->outputStrmSize /= 8;  /* 64-bit addresses */
  regs->outputStrmSize &= (~0x07);    /* 8 multiple size */

  /* Since frame tag is 10 bytes the stream base is not 64-bit aligned.
   * Now the frame headers have been written so we must align the base for
   * HW and set the header remainder properly. */
  regs->outputStrmBase = inst->buffer[0].byteCnt;
  regs->outputStrmBase += inst->buffer[1].byteCnt;

  /* bit offset in the last 64-bit word */
  /** TODO, no StrmBase here, firstFreeBit also need to be
   *  config in the driver. */
  regs->firstFreeBit = (regs->outputStrmBase & 0x07) * 8;

  /* 64-bit aligned HW base */
  regs->outputStrmBase = regs->outputStrmBase & (~0x07);

  /* header remainder is byte aligned, max 7 bytes = 56 bits */
  if (regs->firstFreeBit != 0) {
    /* 64-bit aligned stream pointer */
    uint8_t* pTmp = (uint8_t*)((size_t)(inst->buffer[1].data) & (uint32_t)(~0x07));
    uint32_t val;

    /* Clear remaining bits */
    for (val = 6; val >= regs->firstFreeBit / 8; val--)
      pTmp[val] = 0;

    val = pTmp[0] << 24;
    val |= pTmp[1] << 16;
    val |= pTmp[2] << 8;
    val |= pTmp[3];

    regs->strmStartMSB = val;  /* 32 bits to MSB */

    if (regs->firstFreeBit > 32) {
      val = pTmp[4] << 24;
      val |= pTmp[5] << 16;
      val |= pTmp[6] << 8;

      regs->strmStartLSB = val;
    } else
      regs->strmStartLSB = 0;
  } else {
    regs->strmStartMSB = regs->strmStartLSB = 0;
  }

  /* Quarter pixel MV mode */
  if (sps->quarterPixelMv == 0)
    regs->disableQuarterPixelMv = 1;
  else if (sps->quarterPixelMv == 1) {
    /* Adaptive setting. When resolution larger than 1080p = 8160 macroblocks
     * there is not enough time to do 1/4 pixel ME */
    if (inst->mbPerFrame > 8160)
      regs->disableQuarterPixelMv = 1;
    else
      regs->disableQuarterPixelMv = 0;
  } else
    regs->disableQuarterPixelMv = 0;

  /* Cabac enable bit signals ASIC to read probability tables */
  regs->enableCabac = 1;

  /* Split MV mode */
  if (sps->splitMv == 0)
    regs->splitMvMode = 0;
  else if (sps->splitMv == 1) {
    /* Adaptive setting. When resolution larger than 4CIF = 1584 macroblocks
     * there is no benefit from using split MVs */
    if (inst->mbPerFrame > 1584)
      regs->splitMvMode = 0;
    else
      regs->splitMvMode = 1;
  } else
    regs->splitMvMode = 1;

  qp = inst->rateControl.qpHdr;

  /* If favor has not been set earlier by testId use default */
  if (regs->interFavor == ASIC_PENALTY_UNDEFINED) {
    int32_t tmp = 128 - inst->entropy->intraProb;

    /* This is called intraPenalty in system model */
    if (tmp < 0) {
      regs->interFavor = tmp & 0xFFFF;    /* Signed 16-bit value */
    } else {
      tmp = qp * 2 - 40;
      regs->interFavor = MAX(0, tmp);
    }
  }
  if (regs->diffMvPenalty[0] == ASIC_PENALTY_UNDEFINED)
    regs->diffMvPenalty[0] = 64 / 2;
  if (regs->diffMvPenalty[1] == ASIC_PENALTY_UNDEFINED)
    regs->diffMvPenalty[1] = 60 / 2 * 32;
  if (regs->diffMvPenalty[2] == ASIC_PENALTY_UNDEFINED)
    regs->diffMvPenalty[2] = 8;
  if (regs->skipPenalty == ASIC_PENALTY_UNDEFINED)
    regs->skipPenalty = (qp >= 100) ? (3 * qp / 4) : 0;    /* Zero/nearest/near */
  if (regs->goldenPenalty == ASIC_PENALTY_UNDEFINED)
    regs->goldenPenalty = MAX(0, 5 * qp / 4 - 10);
  if (regs->splitPenalty[0] == ASIC_PENALTY_UNDEFINED)
    regs->splitPenalty[0] = MIN(1023, vp8SplitPenalty[qp] / 2);
  if (regs->splitPenalty[1] == ASIC_PENALTY_UNDEFINED)
    regs->splitPenalty[1] = MIN(1023, (2 * vp8SplitPenalty[qp] + 40) / 4);
  if (regs->splitPenalty[3] == ASIC_PENALTY_UNDEFINED)
    regs->splitPenalty[3] = MIN(511, (8 * vp8SplitPenalty[qp] + 500) / 16);

  /* DMV penalty tables */
  for (i = 0; i < ASIC_PENALTY_TABLE_SIZE; i++) {
    int32_t y, x;

    regs->dmvPenalty[i] = i * 2;
    y = CostMv(i * 2, inst->entropy->mvProb[0]);  /* mv y */
    x = CostMv(i * 2, inst->entropy->mvProb[1]);  /* mv x */
    regs->dmvQpelPenalty[i] = MIN(255, (y + x + 1) / 2 * weight[qp] >> 8);
  }

  /* Quantization tables for each segment */
  for (i = 0; i < SGM_CNT; i++) {
    qp = inst->ppss.pps->qpSgm[i];
    regs->qpY1QuantDc[i] = inst->qpY1[qp].quant[0];
    regs->qpY1QuantAc[i] = inst->qpY1[qp].quant[1];
    regs->qpY2QuantDc[i] = inst->qpY2[qp].quant[0];
    regs->qpY2QuantAc[i] = inst->qpY2[qp].quant[1];
    regs->qpChQuantDc[i] = inst->qpCh[qp].quant[0];
    regs->qpChQuantAc[i] = inst->qpCh[qp].quant[1];
    regs->qpY1ZbinDc[i] = inst->qpY1[qp].zbin[0];
    regs->qpY1ZbinAc[i] = inst->qpY1[qp].zbin[1];
    regs->qpY2ZbinDc[i] = inst->qpY2[qp].zbin[0];
    regs->qpY2ZbinAc[i] = inst->qpY2[qp].zbin[1];
    regs->qpChZbinDc[i] = inst->qpCh[qp].zbin[0];
    regs->qpChZbinAc[i] = inst->qpCh[qp].zbin[1];
    regs->qpY1RoundDc[i] = inst->qpY1[qp].round[0];
    regs->qpY1RoundAc[i] = inst->qpY1[qp].round[1];
    regs->qpY2RoundDc[i] = inst->qpY2[qp].round[0];
    regs->qpY2RoundAc[i] = inst->qpY2[qp].round[1];
    regs->qpChRoundDc[i] = inst->qpCh[qp].round[0];
    regs->qpChRoundAc[i] = inst->qpCh[qp].round[1];
    regs->qpY1DequantDc[i] = inst->qpY1[qp].dequant[0];
    regs->qpY1DequantAc[i] = inst->qpY1[qp].dequant[1];
    regs->qpY2DequantDc[i] = inst->qpY2[qp].dequant[0];
    regs->qpY2DequantAc[i] = inst->qpY2[qp].dequant[1];
    regs->qpChDequantDc[i] = inst->qpCh[qp].dequant[0];
    regs->qpChDequantAc[i] = inst->qpCh[qp].dequant[1];

    regs->filterLevel[i] = inst->ppss.pps->levelSgm[i];
  }

  regs->boolEncValue = inst->buffer[1].bottom;
  regs->boolEncValueBits = 24 - inst->buffer[1].bitsLeft;
  regs->boolEncRange = inst->buffer[1].range;

  regs->cpTarget = NULL;

  /* Select frame type */
  if (inst->picBuffer.cur_pic->i_frame)
    regs->frameCodingType = ASIC_INTRA;
  else
    regs->frameCodingType = ASIC_INTER;

  regs->dctPartitions          = sps->dctPartitions;
  regs->filterDisable          = sps->filterType;
  regs->filterSharpness        = sps->filterSharpness;
  regs->segmentEnable          = inst->ppss.pps->segmentEnabled;
  regs->segmentMapUpdate       = inst->ppss.pps->sgm.mapModified;

  /* For next frame the segmentation map is not needed unless it is modified. */
  inst->ppss.pps->sgm.mapModified = false;

  for (i = 0; i < 4; i++) {
    regs->lfRefDelta[i] = sps->refDelta[i];
    regs->lfModeDelta[i] = sps->modeDelta[i];
  }

  SetIntraPredictionPenalties(regs, qp);

  memset(inst->asic.probCount.vir_addr, 0,
         inst->asic.probCount.size);
}

void SetIntraPredictionPenalties(regValues_s* regs, uint32_t qp) {

  int32_t i, tmp;

  /* Intra 4x4 mode */
  tmp = qp * 2 + 8;
  for (i = 0; i < 10; i++) {
    regs->intraBmodePenalty[i] = (intra4ModeTreePenalty[i] * tmp) >> 8;

  }

  /* Intra 16x16 mode */
  tmp = qp * 2 + 64;
  for (i = 0; i < 4; i++) {
    regs->intraModePenalty[i] = (intra16ModeTreePenalty[i] * tmp) >> 8;
  }

  /* If favor has not been set earlier by testId use default */
  if (regs->intra16Favor == ASIC_PENALTY_UNDEFINED)
    regs->intra16Favor = qp * 1024 / 128;
}

void SetSegmentation(vp8Instance_s* inst) {
  regValues_s* regs = &inst->asic.regs;
  uint32_t* map = inst->asic.segmentMap.vir_addr;
  ppss* ppss = &inst->ppss;
  pps* pps = inst->ppss.pps;  /* Active picture parameters */
  int32_t qp = inst->rateControl.qpHdr;
  uint32_t x, y, mb, mask, id;
  uint32_t mapSize = (inst->mbPerFrame + 15) / 16 * 8;   /* Bytes, 64-bit multiple */

  /* Set the segmentation parameters according to ROI settings.
   * This will override any earlier segmentation settings. */

  if (regs->roi1DeltaQp)
    pps->qpSgm[1] = CLIP3(qp - regs->roi1DeltaQp, 0, 127);

  if (regs->roi2DeltaQp)
    pps->qpSgm[2] = CLIP3(qp - regs->roi2DeltaQp, 0, 127);

  if (regs->roi1DeltaQp || regs->roi2DeltaQp) {
    pps->segmentEnabled = 1;

    memset(pps->sgm.idCnt, 0, sizeof(pps->sgm.idCnt));

    /* Set ROI 1 (ID == 1) and ROI 2 (ID == 2) into map */
    for (y = 0, mb = 0, mask = 0; y < inst->mbPerCol; y++) {
      for (x = 0; x < inst->mbPerRow; x++) {
        id = 0;
        if ((x >= regs->roi1Left) && (x <= regs->roi1Right) &&
            (y >= regs->roi1Top) && (y <= regs->roi1Bottom)) id = 1;
        if ((x >= regs->roi2Left) && (x <= regs->roi2Right) &&
            (y >= regs->roi2Top) && (y <= regs->roi2Bottom)) id = 2;

        pps->sgm.idCnt[id]++;

        mask |= id << (28 - 4 * (mb % 8));
        if ((mb % 8) == 7) {
          *map++ = mask;
          mask = 0;
        }
        mb++;
      }
    }
    *map++ = mask;
    EncSwapEndianess((uint32_t*)inst->asic.segmentMap.vir_addr, mapSize);
  } else if (pps->segmentEnabled && pps->sgm.mapModified) {
    memset(pps->sgm.idCnt, 0, sizeof(pps->sgm.idCnt));

    /* Use the map to calculate id counts */
    for (mb = 0, mask = 0; mb < mapSize / 4; mb++) {
      mask = map[mb];
      for (x = 0; x < 8; x++) {
        if (mb * 8 + x < inst->mbPerFrame) {
          id = (mask >> (28 - 4 * x)) & 0xF;
          pps->sgm.idCnt[id]++;
        }
      }
    }
    EncSwapEndianess((uint32_t*)inst->asic.segmentMap.vir_addr, mapSize);
  }

  /* If current frame is key frame or segmentation is not enabled old
   * segmentation data is not valid anymore, set out of range data to
   * inform Segmentation(). */
  if (inst->picBuffer.cur_pic->i_frame || !pps->segmentEnabled) {
    memset(ppss->qpSgm, 0xff, sizeof(ppss->qpSgm));
    memset(ppss->levelSgm, 0xff, sizeof(ppss->levelSgm));
    ppss->prevPps = NULL;
  } else {
    ppss->prevPps = ppss->pps;
  }
}

void SetFilterParameters(vp8Instance_s* inst) {
  sps* sps = &inst->sps;
  pps* pps = inst->ppss.pps;  /* Active picture parameters */
  uint32_t qp = inst->rateControl.qpHdr;
  uint32_t tmp, i;
  uint32_t iframe = inst->picBuffer.cur_pic->i_frame;
  const int32_t const interLevel[128] = {
    8,  8,  8,  9,  9,  9,  9,  9,  9,  9,
    9,  9,  9,  9,  9,  9, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 11, 11, 11, 11, 11,
    11, 11, 12, 12, 12, 12, 12, 12, 13, 13,
    13, 13, 13, 14, 14, 14, 14, 15, 15, 15,
    15, 16, 16, 16, 16, 17, 17, 17, 18, 18,
    18, 19, 19, 20, 20, 20, 21, 21, 22, 22,
    23, 23, 24, 24, 25, 25, 26, 26, 27, 28,
    28, 29, 30, 30, 31, 32, 33, 33, 34, 35,
    36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
    46, 48, 49, 50, 51, 53, 54, 56, 57, 59,
    60, 62, 63, 63, 63, 63, 63, 63, 63, 63,
    63, 63, 63, 63, 63, 63, 63, 63
  };


  /* auto level */
  if (sps->autoFilterLevel) {
    if (iframe) {
      tmp = (qp * 64) / 128 + 8;
      sps->filterLevel = CLIP3(tmp, 0, 63);
      pps->levelSgm[0] = CLIP3((pps->qpSgm[0] * 64) / 128 + 8, 0, 63);
      pps->levelSgm[1] = CLIP3((pps->qpSgm[1] * 64) / 128 + 8, 0, 63);
      pps->levelSgm[2] = CLIP3((pps->qpSgm[2] * 64) / 128 + 8, 0, 63);
      pps->levelSgm[3] = CLIP3((pps->qpSgm[3] * 64) / 128 + 8, 0, 63);
    } else {
      sps->filterLevel = interLevel[qp];
      pps->levelSgm[0] = interLevel[pps->qpSgm[0]];
      pps->levelSgm[1] = interLevel[pps->qpSgm[1]];
      pps->levelSgm[2] = interLevel[pps->qpSgm[2]];
      pps->levelSgm[3] = interLevel[pps->qpSgm[3]];
    }
  }
  /* auto sharpness */
  if (sps->autoFilterSharpness) {
    sps->filterSharpness = 0;
  }

  if (!sps->filterDeltaEnable) return;

  if (sps->filterDeltaEnable == 2) {
    /* Special meaning, test ID set filter delta values */
    sps->filterDeltaEnable = true;
    return;
  }

  /* force deltas to 0 if filterLevel == 0 (assumed to mean that filtering
   * is completely disabled) */
  if (sps->filterLevel == 0) {
    sps->refDelta[0] =  0;      /* Intra frame */
    sps->refDelta[1] =  0;      /* Last frame */
    sps->refDelta[2] =  0;      /* Golden frame */
    sps->refDelta[3] =  0;      /* Altref frame */
    sps->modeDelta[0] = 0;      /* BPRED */
    sps->modeDelta[1] = 0;      /* Zero */
    sps->modeDelta[2] = 0;      /* New mv */
    sps->modeDelta[3] = 0;      /* Split mv */
    return;
  }

  if (!inst->picBuffer.cur_pic->ipf && !inst->picBuffer.cur_pic->grf &&
      !inst->picBuffer.cur_pic->arf) {
    /* Frame is droppable, ie. doesn't update ipf, grf nor arf so don't
     * update the filter level deltas. */
    memcpy(sps->refDelta, sps->oldRefDelta, sizeof(sps->refDelta));
    memcpy(sps->modeDelta, sps->oldModeDelta, sizeof(sps->modeDelta));
    return;
  }

  /* Adjustment based on reference frame */
  sps->refDelta[0] =  2;      /* Intra frame */
  sps->refDelta[1] =  0;      /* Last frame */
  sps->refDelta[2] = -2;      /* Golden frame */
  sps->refDelta[3] = -2;      /* Altref frame */

  /* Adjustment based on mb mode */
  sps->modeDelta[0] =  4;     /* BPRED */
  sps->modeDelta[1] = -2;     /* Zero */
  sps->modeDelta[2] =  2;     /* New mv */
  sps->modeDelta[3] =  4;     /* Split mv */

  /* ABS(delta) is 6bits, see FilterLevelDelta() */
  for (i = 0; i < 4; i++) {
    sps->refDelta[i] = CLIP3(sps->refDelta[i], -0x3f, 0x3f);
    sps->modeDelta[i] = CLIP3(sps->modeDelta[i], -0x3f, 0x3f);
  }
}

