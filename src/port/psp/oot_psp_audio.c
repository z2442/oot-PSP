#include "attributes.h"
#include "audio.h"
#include "audiomgr.h"
#include "array_count.h"
#include "ocarina.h"
#include "save.h"
#include "seqcmd.h"
#include "sequence.h"
#include "sfx.h"

#include <string.h>

Vec3f gSfxDefaultPos = { 0.0f, 0.0f, 0.0f };
f32 gSfxDefaultFreqAndVolScale = 1.0f;
s8 gSfxDefaultReverb = 0;
u8 gMorphaTransposeTable[16] = { 0, 0, 0, 1, 1, 2, 4, 6, 8, 8, 8, 8, 8, 8, 8, 8 };

static u16 sPspActiveSeqIds[] = {
    NA_BGM_DISABLED,
    NA_BGM_DISABLED,
    NA_BGM_DISABLED,
    NA_BGM_DISABLED,
};
static u16 sPspPrevMainBgmSeqId = NA_BGM_DISABLED;
static s8 sPspFunc800F4524Reverb = 0;

static OcarinaStaff sPspPlayingStaff;
static OcarinaStaff sPspPlaybackStaff;
static OcarinaStaff sPspRecordingStaff;
static u8 sPspFrogsSongNotes[14] = {
    OCARINA_BTN_A,       OCARINA_BTN_C_LEFT,  OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_LEFT,
    OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN,  OCARINA_BTN_A,       OCARINA_BTN_C_DOWN, OCARINA_BTN_A,
    OCARINA_BTN_C_DOWN,  OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT,  OCARINA_BTN_A,
};
static OcarinaNote sPspScarecrowLongSongNotes[108] = {
    { OCARINA_PITCH_NONE, 0, 0, 0, 0, 0 },
    { OCARINA_PITCH_NONE, 0, 0, 0, 0, 0 },
};
static u8 sPspScarecrowSpawnSong[8];

u8* gFrogsSongPtr = sPspFrogsSongNotes;
OcarinaNote* gScarecrowLongSongPtr = sPspScarecrowLongSongNotes;
u8* gScarecrowSpawnSongPtr = sPspScarecrowSpawnSong;

OcarinaSongButtons gOcarinaSongButtons[OCARINA_SONG_MAX] = {
    { 6, { OCARINA_BTN_A, OCARINA_BTN_C_UP, OCARINA_BTN_C_LEFT, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT,
           OCARINA_BTN_C_RIGHT } },
    { 8, { OCARINA_BTN_C_DOWN, OCARINA_BTN_A, OCARINA_BTN_C_DOWN, OCARINA_BTN_A, OCARINA_BTN_C_RIGHT,
           OCARINA_BTN_C_DOWN, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN } },
    { 5, { OCARINA_BTN_A, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT } },
    { 6, { OCARINA_BTN_A, OCARINA_BTN_C_DOWN, OCARINA_BTN_A, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN,
           OCARINA_BTN_A } },
    { 7, { OCARINA_BTN_C_LEFT, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_RIGHT, OCARINA_BTN_A, OCARINA_BTN_C_LEFT,
           OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN } },
    { 6, { OCARINA_BTN_C_UP, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_UP, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT,
           OCARINA_BTN_C_UP } },
    { 6, { OCARINA_BTN_C_DOWN, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_RIGHT,
           OCARINA_BTN_C_LEFT } },
    { 6, { OCARINA_BTN_C_UP, OCARINA_BTN_C_LEFT, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_UP, OCARINA_BTN_C_LEFT,
           OCARINA_BTN_C_RIGHT } },
    { 6, { OCARINA_BTN_C_LEFT, OCARINA_BTN_C_UP, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_LEFT, OCARINA_BTN_C_UP,
           OCARINA_BTN_C_RIGHT } },
    { 6, { OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_UP, OCARINA_BTN_C_RIGHT, OCARINA_BTN_C_DOWN,
           OCARINA_BTN_C_UP } },
    { 6, { OCARINA_BTN_C_RIGHT, OCARINA_BTN_A, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_RIGHT, OCARINA_BTN_A,
           OCARINA_BTN_C_DOWN } },
    { 6, { OCARINA_BTN_A, OCARINA_BTN_C_DOWN, OCARINA_BTN_C_UP, OCARINA_BTN_A, OCARINA_BTN_C_DOWN,
           OCARINA_BTN_C_UP } },
    { 8, { 0 } },
    { 0, { 0 } },
};

static void OotPspAudio_SetSeq(u8 seqPlayerIndex, u16 seqId) {
    if (seqPlayerIndex >= ARRAY_COUNT(sPspActiveSeqIds)) {
        return;
    }

    sPspActiveSeqIds[seqPlayerIndex] = seqId;
    if (seqPlayerIndex == SEQ_PLAYER_BGM_MAIN) {
        gSaveContext.seqId = seqId & 0xFF;
    }
}

void AudioMgr_Init(UNUSED AudioMgr* audioMgr, UNUSED void* stack, UNUSED OSPri pri, UNUSED OSId id,
                   UNUSED Scheduler* sched, UNUSED IrqMgr* irqMgr) {
}

void AudioMgr_WaitForInit(UNUSED AudioMgr* audioMgr) {
}

void AudioMgr_StopAllSfx(void) {
}

void Audio_Init(void) {
    Audio_ResetActiveSequences();
}

void Audio_InitSound(void) {
    Audio_Init();
}

void Audio_Update(void) {
}

void Audio_PreNMI(void) {
    Audio_ResetActiveSequences();
}

void Audio_StartSequence(u8 seqPlayerIndex, u8 seqId, u8 seqArgs, UNUSED u16 fadeInDuration) {
    OotPspAudio_SetSeq(seqPlayerIndex, seqId | (seqArgs << 8));
}

void Audio_StopSequence(u8 seqPlayerIndex, UNUSED u16 fadeOutDuration) {
    OotPspAudio_SetSeq(seqPlayerIndex, NA_BGM_DISABLED);
}

void Audio_QueueSeqCmd(u32 cmd) {
    u8 op = (cmd >> 28) & 0xF;
    u8 seqPlayerIndex = (cmd >> 24) & 0xF;
    u8 fadeTimer = (cmd >> 16) & 0xFF;
    u8 seqArgs = (cmd >> 8) & 0xFF;
    u8 seqId = cmd & 0xFF;

    switch (op) {
        case SEQCMD_OP_PLAY_SEQUENCE:
        case SEQCMD_OP_QUEUE_SEQUENCE:
            Audio_StartSequence(seqPlayerIndex, seqId, seqArgs, fadeTimer);
            break;

        case SEQCMD_OP_STOP_SEQUENCE:
        case SEQCMD_OP_UNQUEUE_SEQUENCE:
            Audio_StopSequence(seqPlayerIndex, cmd & 0xFF);
            break;

        case SEQCMD_OP_GLOBAL_CMD:
            if (((cmd >> 8) & 0xF) == SEQCMD_SUB_OP_GLOBAL_SET_SOUND_OUTPUT_MODE) {
                Audio_SetSoundOutputMode(cmd & 0xFF);
            }
            break;

        default:
            break;
    }
}

void Audio_ProcessSeqCmds(void) {
}

u16 Audio_GetActiveSeqId(u8 seqPlayerIndex) {
    if (seqPlayerIndex >= ARRAY_COUNT(sPspActiveSeqIds)) {
        return NA_BGM_DISABLED;
    }

    return sPspActiveSeqIds[seqPlayerIndex];
}

s32 Audio_IsSeqCmdNotQueued(UNUSED u32 cmdVal, UNUSED u32 cmdMask) {
    return true;
}

void Audio_SetVolumeScale(UNUSED u8 seqPlayerIndex, UNUSED u8 scaleIndex, UNUSED u8 targetVol,
                          UNUSED u8 volFadeTimer) {
}

void Audio_UpdateActiveSequences(void) {
}

u8 func_800FAD34(void) {
    return 0;
}

void Audio_ResetActiveSequences(void) {
    s32 i;

    for (i = 0; i < ARRAY_COUNT(sPspActiveSeqIds); i++) {
        sPspActiveSeqIds[i] = NA_BGM_DISABLED;
    }
}

void Audio_ResetActiveSequencesAndVolume(void) {
    Audio_ResetActiveSequences();
}

void Audio_SetSfxBanksMute(UNUSED u16 muteMask) {
}

void Audio_QueueSeqCmdMute(UNUSED u8 channelIndex) {
}

void Audio_ClearBGMMute(UNUSED u8 channelIndex) {
}

void Audio_PlaySfxGeneral(UNUSED u16 sfxId, UNUSED Vec3f* pos, UNUSED u8 token, UNUSED f32* freqScale,
                          UNUSED f32* vol, UNUSED s8* reverbAdd) {
}

void Audio_ProcessSfxRequest(void) {
}

void Audio_ChooseActiveSfx(UNUSED u8 bankId) {
}

void Audio_PlayActiveSfx(UNUSED u8 bankId) {
}

void Audio_StopSfxByBank(UNUSED u8 bankId) {
}

void Audio_RemoveSfxFromBankByPos(UNUSED u8 bankId, UNUSED Vec3f* pos) {
}

void Audio_StopSfxByPosAndBank(UNUSED u8 bankId, UNUSED Vec3f* pos) {
}

void Audio_StopSfxByPos(UNUSED Vec3f* pos) {
}

void Audio_StopSfxByPosAndId(UNUSED Vec3f* pos, UNUSED u16 sfxId) {
}

void Audio_StopSfxByTokenAndId(UNUSED u8 token, UNUSED u16 sfxId) {
}

void Audio_StopSfxById(UNUSED u32 sfxId) {
}

void Audio_ProcessSfxRequests(void) {
}

void func_800F8F88(void) {
}

u8 Audio_IsSfxPlaying(UNUSED u32 sfxId) {
    return false;
}

void Audio_ResetSfx(void) {
}

void Audio_SetSfxProperties(UNUSED u8 bankId, UNUSED u8 entryIdx, UNUSED u8 channelIndex) {
}

void Audio_PlayCutsceneEffectsSequence(UNUSED u8 csEffectType) {
    OotPspAudio_SetSeq(SEQ_PLAYER_BGM_SUB, NA_BGM_CUTSCENE_EFFECTS);
}

void func_800F4010(Vec3f* pos, u16 sfxId, UNUSED f32 arg2) {
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultReverb);
}

void Audio_PlaySfxRandom(Vec3f* pos, u16 baseSfxId, UNUSED u8 randLim) {
    Audio_PlaySfxGeneral(baseSfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultReverb);
}

void func_800F4138(Vec3f* pos, u16 sfxId, f32 arg2) {
    func_800F4010(pos, sfxId, arg2);
}

void func_800F4190(Vec3f* pos, u16 sfxId) {
    func_800F4010(pos, sfxId, 1.0f);
}

void func_800F436C(Vec3f* pos, u16 sfxId, UNUSED f32 arg2) {
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultReverb);
}

void func_800F4414(Vec3f* pos, u16 sfxId, f32 arg2) {
    func_800F436C(pos, sfxId, arg2);
}

void func_800F44EC(UNUSED s8 arg0, UNUSED s8 arg1) {
}

void func_800F4524(Vec3f* pos, u16 sfxId, s8 arg2) {
    sPspFunc800F4524Reverb = arg2;
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale,
                         &sPspFunc800F4524Reverb);
}

void Audio_PlaySwordChargeSfx(Vec3f* pos, u8 level) {
    Audio_PlaySfxGeneral(NA_SE_IT_SWORD_CHARGE - 1 + level, pos, 4, &gSfxDefaultFreqAndVolScale,
                         &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void Audio_PlaySfxRiver(Vec3f* pos, f32 freqScale) {
    Audio_PlaySfxGeneral(NA_SE_EV_RIVER_STREAM, pos, 4, &freqScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void Audio_PlaySfxWaterfall(Vec3f* pos, f32 freqScale) {
    Audio_PlaySfxGeneral(NA_SE_EV_WATER_WALL, pos, 4, &freqScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void Audio_SetBgmVolumeOffDuringFanfare(void) {
}

void Audio_SetBgmVolumeOnDuringFanfare(void) {
}

void Audio_SetMainBgmVolume(UNUSED u8 targetVol, UNUSED u8 volFadeTimer) {
}

void Audio_SetGanonsTowerBgmVolumeLevel(UNUSED u8 ganonsTowerLevel) {
}

void Audio_LowerMainBgmVolume(UNUSED u8 volume) {
}

void Audio_PlaySfxIncreasinglyTransposed(Vec3f* pos, s16 sfxId, UNUSED u8* semitones) {
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void Audio_ResetIncreasingTranspose(void) {
}

void Audio_PlaySfxTransposed(Vec3f* pos, u16 sfxId, UNUSED s8 semitone) {
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void func_800F4C58(Vec3f* pos, u16 sfxId, UNUSED u8 arg2) {
    Audio_PlaySfxGeneral(sfxId, pos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
}

void func_800F4E30(UNUSED Vec3f* pos, UNUSED f32 arg1) {
}

void Audio_ClearSariaBgm(void) {
    if (Audio_GetActiveSeqId(SEQ_PLAYER_BGM_SUB) == NA_BGM_SARIA_THEME) {
        Audio_StopSequence(SEQ_PLAYER_BGM_SUB, 0);
    }
}

void Audio_ClearSariaBgmAtPos(UNUSED Vec3f* pos) {
    Audio_ClearSariaBgm();
}

void Audio_PlaySariaBgm(UNUSED Vec3f* pos, u16 seqId, UNUSED u16 distMax) {
    OotPspAudio_SetSeq(SEQ_PLAYER_BGM_SUB, seqId);
}

void Audio_ClearSariaBgm2(void) {
    Audio_ClearSariaBgm();
}

void Audio_PlayMorningSceneSequence(u16 seqId) {
    Audio_PlaySceneSequence(seqId);
}

void Audio_PlaySceneSequence(u16 seqId) {
    OotPspAudio_SetSeq(SEQ_PLAYER_BGM_MAIN, seqId);
    gSaveContext.forcedSeqId = seqId;
}

void Audio_SetMainBgmTempoFreqAfterFanfare(UNUSED f32 scaleTempoAndFreq, UNUSED u8 duration) {
}

void Audio_PlayWindmillBgm(void) {
    Audio_PlaySceneSequence(NA_BGM_WINDMILL);
}

void Audio_SetFastTempoForTimedMinigame(void) {
}

void Audio_PlaySequenceInCutscene(u16 seqId) {
    Audio_PlaySceneSequence(seqId);
}

void Audio_StopSequenceInCutscene(u16 seqId) {
    if (Audio_IsSequencePlaying(seqId)) {
        Audio_StopSequence(SEQ_PLAYER_BGM_MAIN, 0);
        Audio_StopSequence(SEQ_PLAYER_FANFARE, 0);
    }
}

s32 Audio_IsSequencePlaying(u16 seqId) {
    s32 i;

    for (i = 0; i < ARRAY_COUNT(sPspActiveSeqIds); i++) {
        if ((sPspActiveSeqIds[i] & 0xFF) == (seqId & 0xFF)) {
            return true;
        }
    }

    return false;
}

void func_800F5ACC(u16 seqId) {
    u16 curSeqId = Audio_GetActiveSeqId(SEQ_PLAYER_BGM_MAIN);

    if ((curSeqId != NA_BGM_DISABLED) && ((curSeqId & 0xFF) != (seqId & 0xFF))) {
        sPspPrevMainBgmSeqId = curSeqId;
    }

    Audio_PlaySceneSequence(seqId);
}

void func_800F5B58(void) {
    if (sPspPrevMainBgmSeqId != NA_BGM_DISABLED) {
        Audio_PlaySceneSequence(sPspPrevMainBgmSeqId);
        sPspPrevMainBgmSeqId = NA_BGM_DISABLED;
    }
}

void func_800F5BF0(u8 natureAmbienceId) {
    Audio_PlayNatureAmbienceSequence(natureAmbienceId);
}

void Audio_PlayFanfare(u16 seqId) {
    OotPspAudio_SetSeq(SEQ_PLAYER_FANFARE, seqId);
}

void func_800F5C2C(void) {
}

void Audio_PlaySequenceWithSeqPlayerIO(u8 seqPlayerIndex, u16 seqId, u8 fadeInDuration, UNUSED s8 ioPort,
                                       UNUSED s8 ioData) {
    Audio_StartSequence(seqPlayerIndex, seqId & 0xFF, (seqId >> 8) & 0xFF, fadeInDuration);
}

void Audio_SetSequenceMode(UNUSED u8 seqMode) {
}

void Audio_SetBgmEnemyVolume(UNUSED f32 dist) {
}

void Audio_UpdateMalonSinging(UNUSED f32 dist, u16 seqId) {
    OotPspAudio_SetSeq(SEQ_PLAYER_BGM_SUB, seqId);
}

void func_800F64E0(UNUSED u8 arg0) {
}

void Audio_ToggleMalonSinging(UNUSED u8 malonSingingDisabled) {
}

void Audio_SetEnvReverb(UNUSED s8 reverb) {
}

void Audio_SetCodeReverb(UNUSED s8 reverb) {
}

void Audio_SetSoundOutputMode(s8 soundSetting) {
    gSaveContext.soundSetting = soundSetting;
}

void Audio_SetBaseFilter(UNUSED u8 filter) {
}

void Audio_SetExtraFilter(UNUSED u8 filter) {
}

void Audio_SetCutsceneFlag(UNUSED s8 flag) {
}

void Audio_PlaySfxIfNotInCutscene(u16 sfxId) {
    SFX_PLAY_CENTERED(sfxId);
}

void func_800F6964(u16 fadeOutDuration) {
    Audio_StopBgmAndFanfare(fadeOutDuration);
}

void Audio_StopBgmAndFanfare(u16 fadeOutDuration) {
    Audio_StopSequence(SEQ_PLAYER_BGM_MAIN, fadeOutDuration);
    Audio_StopSequence(SEQ_PLAYER_FANFARE, fadeOutDuration);
}

void func_800F6B3C(void) {
}

void func_800F6BDC(void) {
}

void Audio_SetNatureAmbienceChannelIO(UNUSED u8 channelIdxRange, UNUSED u8 ioPort, UNUSED u8 ioData) {
}

void Audio_PlayNatureAmbienceSequence(u8 natureAmbienceId) {
    gSaveContext.natureAmbienceId = natureAmbienceId;
    OotPspAudio_SetSeq(SEQ_PLAYER_BGM_MAIN, NA_BGM_NATURE_AMBIENCE);
}

void func_800F7170(void) {
}

void func_800F71BC(UNUSED s32 arg0) {
}

void AudioOcarina_Start(UNUSED u16 ocarinaFlags) {
    sPspPlayingStaff.buttonIndex = OCARINA_BTN_INVALID;
    sPspPlayingStaff.state = 0;
    sPspPlayingStaff.pos = 0;
}

void AudioOcarina_SetInstrument(UNUSED u8 ocarinaInstrumentId) {
}

void AudioOcarina_SetPlaybackSong(s8 songIndexPlusOne, s8 playbackState) {
    sPspPlaybackStaff.buttonIndex = OCARINA_BTN_INVALID;
    sPspPlaybackStaff.state = songIndexPlusOne;
    sPspPlaybackStaff.pos = playbackState;
}

void AudioOcarina_SetRecordingState(u8 recordingState) {
    sPspRecordingStaff.state = recordingState;
    sPspRecordingStaff.pos = 0;
}

OcarinaStaff* AudioOcarina_GetRecordingStaff(void) {
    return &sPspRecordingStaff;
}

OcarinaStaff* AudioOcarina_GetPlayingStaff(void) {
    return &sPspPlayingStaff;
}

OcarinaStaff* AudioOcarina_GetPlaybackStaff(void) {
    return &sPspPlaybackStaff;
}

void AudioOcarina_MemoryGameInit(UNUSED u8 minigameRound) {
}

s32 AudioOcarina_MemoryGameNextNote(void) {
    return 1;
}

void AudioOcarina_PlayLongScarecrowSong(void) {
    AudioOcarina_SetPlaybackSong(OCARINA_SONG_SCARECROW_LONG + 1, 1);
}
